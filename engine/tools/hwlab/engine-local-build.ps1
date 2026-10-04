#Requires -Version 5.1
<#
.SYNOPSIS
  Builds the media engine against the pinned libwebrtc release on this machine and runs
  its call test (hwlab kit, engine/tools/hwlab). No administrator rights needed.

.DESCRIPTION
  The same steps as the engine-webrtc job of .github/workflows/engine.yml, on the lab PC:

    1. Reads the pinned release from engine/cmake/webrtc-release.cmake and prints the
       hosts and URLs the download step will contact. Without -AllowDownload nothing is
       downloaded: the script prints this plan and stops with exit code 10, so that the
       lab session can show the hosts to the owner first. (If a complete, sha256-verified
       download is already in the fetch folder, nothing has to be downloaded and the
       script goes on without -AllowDownload.)
    2. Finds a Visual Studio with the MSVC x64 tools through vswhere (any edition, Build
       Tools included, no hard-coded path), takes its environment from vcvars64.bat, and
       finds cmake and ninja.
    3. cmake -P engine/cmake/fetch_webrtc.cmake into <WorkDir>\webrtc-fetch: the release
       files and the Chromium clang package, every file checked against the sha256 pinned
       in the repository. Then cmake/verify_pins.cmake is run once more on the result.
    4. Configures (Ninja, the downloaded toolchain.cmake, QMEDIA_WITH_WEBRTC=ON,
       QMEDIA_BUILD_TESTS=ON) and builds qaudion-media, qaudion-media-ci and
       qmedia_call_test into <WorkDir>\build-engine.
    5. Runs qmedia_call_test.exe (two engine processes, one full call over loopback).
    6. Writes <ReportDir>\engine-build-<stamp>.json and engine-build-<stamp>.log.

  The attestation of webrtc.lib is NOT verified here: CI does it with `gh attestation
  verify`, which needs the GitHub CLI and a token, and the lab holds neither by design.
  Locally only the sha256 pins of the repository apply (verify_pins.cmake enforces them
  at configure time). The report says so.

  Long steps (the download, the build) run in the foreground of this script. Start the
  script in the background and follow the log file instead of waiting for it.

  Output is scrubbed of the account name, the machine name, MAC and IP patterns (the
  helpers of hwlab-common.ps1) before it reaches the log or the report.

  Exit code: 0 built and tested, 1 toolchain or repository problem, 2 download failed,
  3 configure failed, 4 build failed, 5 call test failed, 10 plan printed and nothing
  downloaded (see -AllowDownload).

.PARAMETER RepoDir
  The clone made by setup-hwlab.ps1 (default: the repository this script is part of,
  found from the script's own location).

.PARAMETER WorkDir
  Default C:\hwlab\work. The download goes to <WorkDir>\webrtc-fetch, the build to
  <WorkDir>\build-engine.

.PARAMETER ReportDir
  Default C:\hwlab\reports.

.PARAMETER AllowDownload
  Required to download. Passing it states that the owner has seen the hosts of the plan
  and agreed (commondatastorage.googleapis.com in particular, which is not otherwise on
  the lab's list of hosts).

.PARAMETER PlanOnly
  Print the plan and exit with code 10, whatever else is given.

.PARAMETER ResolveClangUrl
  With the plan: fetch only build-flags.json from the release on github.com (sha256
  checked against the pin) to print the exact URL of the clang package.

.PARAMETER PreflightOnly
  Check the toolchain (vswhere, vcvars64.bat, cmake, ninja) and exit 0 or 1.

.PARAMETER Update
  Fast-forward the clone from origin before building.

.PARAMETER Clean
  Delete <WorkDir>\build-engine first.
#>
[CmdletBinding()]
param(
  [string]$RepoDir   = '',
  [string]$WorkDir   = 'C:\hwlab\work',
  [string]$ReportDir = 'C:\hwlab\reports',
  [switch]$AllowDownload,
  [switch]$PlanOnly,
  [switch]$ResolveClangUrl,
  [switch]$PreflightOnly,
  [switch]$Update,
  [switch]$Clean,
  [int]$FetchTimeoutMinutes = 90,
  [int]$BuildTimeoutMinutes  = 90,
  [int]$TestTimeoutMinutes   = 15
)

Set-StrictMode -Version 1.0
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'hwlab-common.ps1')
Initialize-Scrubbing

$VsCppComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$ClangHost      = 'commondatastorage.googleapis.com'
$ClangUrlRegex  = '^https://commondatastorage\.googleapis\.com/chromium-browser-clang/Win/clang-[A-Za-z0-9._-]+\.tar\.xz$'

$script:LogFile  = $null
$script:Warnings = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------------

function Write-Log([string]$Message, [string]$Level = 'INFO') {
  $line = ('[{0}] {1,-5} {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, (Protect-Text $Message))
  switch ($Level) {
    'ERROR' { Write-Host $line -ForegroundColor Red }
    'WARN'  { Write-Host $line -ForegroundColor Yellow }
    'OK'    { Write-Host $line -ForegroundColor Green }
    default { Write-Host $line }
  }
  if ($script:LogFile) {
    try { [System.IO.File]::AppendAllText($script:LogFile, $line + "`r`n") } catch { }
  }
}

function Stop-Build([string]$Message, [int]$Code) {
  Write-Log $Message 'ERROR'
  $script:FailureMessage = $Message
  $script:ExitOnFail = $Code
  throw [System.Exception]::new('engine-local-build: ' + $Message)
}

# ---------------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------------

function Quote-Arg([string]$A) {
  if ($A -eq '' -or $A -match '[\s"]') { return '"' + ($A -replace '"', '\"') + '"' }
  return $A
}

function Join-Args([string[]]$ArgList) {
  return (($ArgList | ForEach-Object { Quote-Arg $_ }) -join ' ')
}

function ConvertTo-Slash([string]$Path) { return ($Path -replace '\\', '/') }

function Get-FirstLine([string]$Text) {
  foreach ($l in ($Text -split "`r?`n")) { if ($l.Trim()) { return $l.Trim() } }
  return $null
}

# Console program without a window; returns @{ Code; Out } (stdout only, not scrubbed:
# for version strings and paths that are not written anywhere).
function Invoke-Capture([string]$Exe, [string]$ArgumentString, [int]$TimeoutSeconds = 60) {
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Exe
  $psi.Arguments = $ArgumentString
  $psi.UseShellExecute = $false
  $psi.CreateNoWindow = $true
  $psi.RedirectStandardOutput = $true
  $psi.RedirectStandardError = $true
  try { $p = [System.Diagnostics.Process]::Start($psi) } catch { return @{ Code = -1; Out = '' } }
  $outTask = $p.StandardOutput.ReadToEndAsync()
  $errTask = $p.StandardError.ReadToEndAsync()
  if (-not $p.WaitForExit($TimeoutSeconds * 1000)) {
    try { $p.Kill() } catch { }
    return @{ Code = -2; Out = '' }
  }
  $p.WaitForExit()
  return @{ Code = [int]$p.ExitCode; Out = [string]$outTask.Result }
}

function Update-SessionPath {
  $have = @($env:Path -split ';' | Where-Object { $_ })
  $fresh = @()
  foreach ($scope in 'Machine', 'User') {
    $v = [Environment]::GetEnvironmentVariable('Path', $scope)
    if ($v) { $fresh += @($v -split ';' | Where-Object { $_ }) }
  }
  foreach ($extra in @("$env:ProgramFiles\Git\cmd", "$env:ProgramFiles\CMake\bin", "$env:ProgramFiles\WinGet\Links", "$env:LOCALAPPDATA\Microsoft\WinGet\Links")) {
    if (Test-Path -LiteralPath $extra) { $fresh += $extra }
  }
  foreach ($d in $fresh) { if ($have -notcontains $d) { $have += $d } }
  $env:Path = ($have -join ';')
}

function Find-Tool([string]$Name, [string[]]$Fallbacks) {
  $c = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($c) { return $c.Source }
  foreach ($p in $Fallbacks) { if ($p -and (Test-Path -LiteralPath $p)) { return $p } }
  return $null
}

# ---------------------------------------------------------------------------------
# The pins and the plan
# ---------------------------------------------------------------------------------

# Reads the release repository, tag, file list and sha256 pins out of
# engine/cmake/webrtc-release.cmake, the single place that pins them.
function Get-ReleasePins([string]$PinsFile) {
  if (-not (Test-Path -LiteralPath $PinsFile)) { throw "the pin file was not found: $PinsFile" }
  $text = [System.IO.File]::ReadAllText($PinsFile)
  $repo = [regex]::Match($text, 'set\(QMEDIA_WEBRTC_REPO\s+"([^"]+)"\)').Groups[1].Value
  $tag  = [regex]::Match($text, 'set\(QMEDIA_WEBRTC_TAG\s+"([^"]+)"\)').Groups[1].Value
  $block = [regex]::Match($text, '(?s)set\(QMEDIA_WEBRTC_FILES\s+(.*?)\)').Groups[1].Value
  $files = @($block -split '\s+' | Where-Object { $_ })
  if ($repo -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') { throw 'the pinned repository name is not valid' }
  if ($tag -notmatch '^[A-Za-z0-9_.-]+$') { throw 'the pinned release tag is not valid' }
  if ($files.Count -lt 1) { throw 'the pin file lists no release files' }
  $sha = @{}
  foreach ($m in [regex]::Matches($text, 'set\(QMEDIA_SHA256_(\S+)\s+"([0-9a-f]{64})"\)')) { $sha[$m.Groups[1].Value] = $m.Groups[2].Value }
  foreach ($f in $files) {
    if ($f -notmatch '^[A-Za-z0-9_.-]+$') { throw "a pinned file name is not valid: $f" }
    if (-not $sha.ContainsKey($f)) { throw "no sha256 is pinned for $f" }
  }
  return [pscustomobject]@{ Repo = $repo; Tag = $tag; Files = $files; Sha = $sha }
}

function Get-ReleaseUrl($Pins, [string]$File) {
  return ('https://github.com/{0}/releases/download/{1}/{2}' -f $Pins.Repo, $Pins.Tag, $File)
}

# The clang package url named by build-flags.json, when that file is on disk and is the
# pinned one. Returns $null otherwise.
function Get-ClangPackage($Pins, [string]$FlagsFile) {
  if (-not (Test-Path -LiteralPath $FlagsFile)) { return $null }
  $have = (Get-FileHash -Algorithm SHA256 -LiteralPath $FlagsFile).Hash.ToLower()
  if ($have -ne $Pins.Sha['build-flags.json']) { return $null }
  $j = [System.IO.File]::ReadAllText($FlagsFile) | ConvertFrom-Json
  $url = [string]$j.toolchain.clang_package_url
  $sum = [string]$j.toolchain.clang_package_sha256
  if ($url -notmatch $ClangUrlRegex) { return $null }
  if ($sum -notmatch '^[0-9a-f]{64}$') { return $null }
  return [pscustomobject]@{ Url = $url; Sha256 = $sum }
}

function Show-Plan($Pins, $Clang, [bool]$Cached) {
  Write-Host ''
  Write-Host 'engine-local-build: download plan'
  Write-Host ''
  Write-Host ('  Pinned release : {0}, tag {1}' -f $Pins.Repo, $Pins.Tag)
  Write-Host '  Pinned in      : engine/cmake/webrtc-release.cmake (every file is checked against its sha256)'
  Write-Host ''
  Write-Host '  Hosts the download step contacts:'
  Write-Host '    github.com                        the release files below. GitHub serves the bytes from its own'
  Write-Host '                                      download hosts (githubusercontent.com) after a redirect.'
  Write-Host ('    {0}  the Chromium clang package that build-flags.json of the release names' -f $ClangHost)
  Write-Host '                                      (the compiler of the engine build); this host is NOT on the lab''s'
  Write-Host '                                      usual list, so the owner has to agree to it.'
  Write-Host ''
  Write-Host '  Release files:'
  foreach ($f in $Pins.Files) {
    Write-Host ('    {0}' -f (Get-ReleaseUrl $Pins $f))
    Write-Host ('      sha256 {0}' -f $Pins.Sha[$f])
  }
  Write-Host ''
  if ($Clang) {
    Write-Host '  Clang package (exact file, from the pinned build-flags.json):'
    Write-Host ('    {0}' -f $Clang.Url)
    Write-Host ('      sha256 {0}' -f $Clang.Sha256)
  } else {
    Write-Host '  Clang package: https://commondatastorage.googleapis.com/chromium-browser-clang/Win/clang-<name>.tar.xz'
    Write-Host '    The exact file name and its sha256 are in build-flags.json of the release, which is'
    Write-Host '    fetched first. Run with -PlanOnly -ResolveClangUrl to fetch just that file from'
    Write-Host '    github.com and print the exact url before anything else is downloaded.'
  }
  Write-Host ''
  if ($Cached) {
    Write-Host '  A complete, verified download is already in the fetch folder: nothing has to be downloaded.'
  } else {
    Write-Host '  Nothing is downloaded until the script is run with -AllowDownload.'
  }
  Write-Host ''
}

# Downloads only build-flags.json (from github.com, sha256 checked against the pin) to
# learn the exact clang package url.
function Resolve-ClangFromRelease($Pins, [string]$PlanDir) {
  if (-not (Test-Path -LiteralPath $PlanDir)) { $null = New-Item -ItemType Directory -Path $PlanDir -Force }
  $dest = Join-Path $PlanDir 'build-flags.json'
  try {
    [Net.ServicePointManager]::SecurityProtocol = [Net.ServicePointManager]::SecurityProtocol -bor [Net.SecurityProtocolType]::Tls12
    Invoke-WebRequest -UseBasicParsing -Uri (Get-ReleaseUrl $Pins 'build-flags.json') -OutFile $dest -TimeoutSec 120
  } catch {
    Write-Log ('could not fetch build-flags.json from github.com: ' + $_.Exception.Message) 'WARN'
    return $null
  }
  $c = Get-ClangPackage $Pins $dest
  if (-not $c) { Write-Log 'build-flags.json did not match its pin or names no valid clang package url' 'WARN' }
  return $c
}

# ---------------------------------------------------------------------------------
# Toolchain
# ---------------------------------------------------------------------------------

# vswhere, any edition (Build Tools, Community, Professional, Enterprise). Visual Studio
# 2022 first; any other version only as a fallback, with a warning.
function Find-VisualStudio {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { return $null }
  foreach ($range in @('[17.0,18.0)', $null)) {
    $a = @('-products', '*', '-requires', $VsCppComponent, '-latest', '-format', 'json')
    if ($range) { $a += @('-version', $range) }
    $r = Invoke-Capture $vswhere (Join-Args $a)
    if ($r.Code -ne 0) { continue }
    $arr = @($r.Out | ConvertFrom-Json)
    if ($arr.Count -lt 1) { continue }
    $vs = $arr[0]
    $vcvars = Join-Path $vs.installationPath 'VC\Auxiliary\Build\vcvars64.bat'
    if (-not (Test-Path -LiteralPath $vcvars)) { continue }
    return [pscustomobject]@{
      Path     = [string]$vs.installationPath
      VcVars   = $vcvars
      Version  = [string]$vs.installationVersion
      Product  = [string]$vs.productId
      Name     = [string]$vs.displayName
      Is2022   = [bool]($range -ne $null)
    }
  }
  return $null
}

# Runs vcvars64.bat in a cmd.exe and copies the environment it sets into this process.
function Import-VcVars([string]$VcVars) {
  $r = Invoke-Capture $env:ComSpec ('/d /s /c ""' + $VcVars + '" >nul 2>&1 && set"') 300
  if ($r.Code -ne 0) { return $false }
  $n = 0
  foreach ($line in ($r.Out -split "`r?`n")) {
    $i = $line.IndexOf('=')
    if ($i -lt 1) { continue }
    [Environment]::SetEnvironmentVariable($line.Substring(0, $i), $line.Substring($i + 1), 'Process')
    $n++
  }
  return ($n -gt 0 -and [bool]$env:INCLUDE -and [bool]$env:LIB)
}

function Get-ToolVersion([string]$Exe, [string]$ArgText, [string]$Strip) {
  if (-not $Exe) { return $null }
  $r = Invoke-Capture $Exe $ArgText
  $l = Get-FirstLine $r.Out
  if (-not $l) { return $null }
  if ($Strip) { $l = $l -replace $Strip, '' }
  return $l.Trim()
}

# ---------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------

# Default: the repository this script is part of (engine/tools/hwlab/..).
if (-not $RepoDir) { $RepoDir = Join-Path $PSScriptRoot '..\..\..' }
$RepoDir = [System.IO.Path]::GetFullPath($RepoDir)
$engineDir = Join-Path $RepoDir 'engine'
$fetchScript = Join-Path $engineDir 'cmake\fetch_webrtc.cmake'
$verifyScript = Join-Path $engineDir 'cmake\verify_pins.cmake'
$pinsFile = Join-Path $engineDir 'cmake\webrtc-release.cmake'
$FetchDir = Join-Path $WorkDir 'webrtc-fetch'
$BuildDir = Join-Path $WorkDir 'build-engine'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'

if (-not (Test-Path -LiteralPath $fetchScript)) {
  Write-Host ('FAILED: {0} was not found; point -RepoDir at the clone made by setup-hwlab.ps1' -f $fetchScript) -ForegroundColor Red
  exit 1
}

try {
  $pins = Get-ReleasePins $pinsFile
} catch {
  Write-Host ('FAILED: ' + $_.Exception.Message) -ForegroundColor Red
  exit 1
}

$clangPkg = Get-ClangPackage $pins (Join-Path $FetchDir 'release\build-flags.json')
if (-not $clangPkg) { $clangPkg = Get-ClangPackage $pins (Join-Path $FetchDir 'plan\build-flags.json') }
if (-not $clangPkg -and $ResolveClangUrl) { $clangPkg = Resolve-ClangFromRelease $pins (Join-Path $FetchDir 'plan') }

# Is a complete, verified download already there? (cmake only; nothing is contacted.)
function Test-FetchComplete {
  $tc = Join-Path $FetchDir 'toolchain.cmake'
  if (-not (Test-Path -LiteralPath $tc)) { return $false }
  $cmakeExe = Find-Tool 'cmake.exe' @("$env:ProgramFiles\CMake\bin\cmake.exe")
  if (-not $cmakeExe) { return $false }
  $r = Invoke-Capture $cmakeExe (Join-Args @(('-DQMEDIA_WEBRTC_DIR=' + (ConvertTo-Slash (Join-Path $FetchDir 'release'))), ('-DQMEDIA_CLANG_ROOT=' + (ConvertTo-Slash (Join-Path $FetchDir 'clang'))), '-P', $verifyScript)) 120
  return ($r.Code -eq 0)
}
Update-SessionPath
$cached = Test-FetchComplete

if ($PlanOnly) {
  Show-Plan $pins $clangPkg $cached
  Write-Host 'PLAN ONLY: nothing was downloaded (exit code 10).'
  exit 10
}

if (-not $PreflightOnly -and -not $cached -and -not $AllowDownload) {
  Show-Plan $pins $clangPkg $false
  Write-Host 'STOPPED: nothing was downloaded. Show the hosts above to the owner and, when they agree,' -ForegroundColor Yellow
  Write-Host '         run the script again with -AllowDownload (exit code 10).' -ForegroundColor Yellow
  exit 10
}

# --- from here on the script really does something: log file, report ---------------

if (-not $PreflightOnly) {
  if (-not (Test-Path -LiteralPath $ReportDir)) { $null = New-Item -ItemType Directory -Path $ReportDir -Force }
  $script:LogFile = Join-Path $ReportDir ('engine-build-' + $stamp + '.log')
  [System.IO.File]::WriteAllText($script:LogFile, '')
}

# Note: @($steps) throws 'Argument types do not match' in Windows PowerShell 5.1 for a List[object]; use ToArray().
$steps = New-Object System.Collections.Generic.List[object]
$report = [ordered]@{}
$exitCode = 0
$failedStep = $null
$script:FailureMessage = $null
$toolchain = [ordered]@{}
$testInfo = [ordered]@{ ran = $false; ok = $false; exit_code = $null; seconds = $null; tail = @() }
$pinsVerified = $false
$fetchSkipped = $false
$commit = $null
$total = [System.Diagnostics.Stopwatch]::StartNew()

function Add-Step([string]$Name, $Result, [bool]$Ok) {
  $steps.Add([ordered]@{ name = $Name; ok = $Ok; seconds = $Result.Seconds; exit_code = $Result.Code; timed_out = $Result.TimedOut })
}

function Run-Step([string]$Name, [string]$Exe, [string[]]$ArgList, [int]$Minutes, [int]$FailCode, [string]$Dir = '') {
  Write-Log ('step: ' + $Name)
  $res = Invoke-LoggedProcess -Exe $Exe -ArgumentString (Join-Args $ArgList) -LogFile $script:LogFile -TimeoutMinutes $Minutes -WorkingDirectory $Dir -TailLines 20
  $ok = ($res.Code -eq 0 -and -not $res.TimedOut)
  Add-Step $Name $res $ok
  if ($ok) {
    Write-Log ('{0}: ok in {1} s' -f $Name, $res.Seconds) 'OK'
  } else {
    $why = if ($res.TimedOut) { 'timed out after ' + $Minutes + ' minutes' } else { 'exit code ' + $res.Code }
    Stop-Build ('{0} failed ({1})' -f $Name, $why) $FailCode
  }
  return $res
}
$script:ExitOnFail = 1

try {
  Write-Log 'engine-local-build starting'
  Write-Log ('repository: {0}' -f $RepoDir)

  # 1. Repository -------------------------------------------------------------------
  $git = Find-Tool 'git.exe' @("$env:ProgramFiles\Git\cmd\git.exe")
  $gitSafe = @('-c', ('safe.directory=' + (ConvertTo-Slash $RepoDir)))
  if ($Update) {
    if (-not $git) { Stop-Build 'git was not found, cannot update the repository' 1 }
    $env:GIT_TERMINAL_PROMPT = '0'
    $res = Invoke-LoggedProcess -Exe $git -ArgumentString (Join-Args (@('-c', 'credential.helper=') + $gitSafe + @('-C', $RepoDir, 'pull', '--ff-only'))) -LogFile $script:LogFile -TimeoutMinutes 10
    if ($res.Code -ne 0) { Stop-Build ('git pull --ff-only failed (exit code {0}); inspect the clone by hand' -f $res.Code) 1 }
  }
  if ($git) {
    $r = Invoke-Capture $git (Join-Args ($gitSafe + @('-C', $RepoDir, 'rev-parse', 'HEAD')))
    $line = Get-FirstLine $r.Out
    if ($r.Code -eq 0 -and $line -match '^[0-9a-f]{40}$') { $commit = $line }
  }
  Write-Log ('repository commit: {0}' -f $(if ($commit) { $commit } else { 'unknown' }))

  # 2. Toolchain --------------------------------------------------------------------
  Write-Log 'toolchain'
  $problems = New-Object System.Collections.Generic.List[string]
  $vs = Find-VisualStudio
  if (-not $vs) {
    $problems.Add('no Visual Studio with the MSVC x64 tools was found by vswhere (run setup-hwlab.ps1)')
  } else {
    Write-Log ('Visual Studio: {0} {1} ({2}), found by vswhere' -f $vs.Name, $vs.Version, $vs.Product)
    if (-not $vs.Is2022) { $script:Warnings.Add('the Visual Studio found is not a 2022 (17.x) one'); Write-Log 'this is not a Visual Studio 2022 (17.x); the engine is built and tested with 2022' 'WARN' }
    if (-not (Import-VcVars $vs.VcVars)) {
      $problems.Add('vcvars64.bat did not set up the build environment')
    }
  }
  Update-SessionPath
  $cmakeExe = Find-Tool 'cmake.exe' @("$env:ProgramFiles\CMake\bin\cmake.exe")
  if (-not $cmakeExe -and $vs) { $cmakeExe = Find-Tool 'cmake.exe' @((Join-Path $vs.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe')) }
  $ninjaExe = Find-Tool 'ninja.exe' @("$env:ProgramFiles\WinGet\Links\ninja.exe", "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe")
  if (-not $ninjaExe -and $vs) { $ninjaExe = Find-Tool 'ninja.exe' @((Join-Path $vs.Path 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe')) }
  if (-not $cmakeExe) { $problems.Add('cmake was not found') }
  if (-not $ninjaExe) { $problems.Add('ninja was not found') }
  $cmakeVer = Get-ToolVersion $cmakeExe '--version' '^cmake version\s+'
  $ninjaVer = Get-ToolVersion $ninjaExe '--version' ''
  $toolchain['visual_studio'] = [ordered]@{ name = $(if ($vs) { $vs.Name } else { $null }); version = $(if ($vs) { $vs.Version } else { $null }); product = $(if ($vs) { $vs.Product } else { $null }); found_by = 'vswhere' }
  $toolchain['msvc_tools_version'] = $env:VCToolsVersion
  $toolchain['windows_sdk_version'] = ($env:WindowsSDKVersion -replace '\\', '')
  $toolchain['cmake'] = $cmakeVer
  $toolchain['ninja'] = $ninjaVer
  $toolchain['clang_cl'] = $null
  Write-Log ('MSVC tools {0}, Windows SDK {1}, cmake {2}, ninja {3}' -f $env:VCToolsVersion, ($env:WindowsSDKVersion -replace '\\', ''), $cmakeVer, $ninjaVer)
  if ($problems.Count -gt 0) {
    foreach ($p in $problems) { Write-Log ('missing: ' + $p) 'ERROR' }
    Stop-Build 'the toolchain is incomplete. Run setup-hwlab.ps1 once from an elevated PowerShell (a restart may be needed after it), then run this script again.' 1
  }
  if ($PreflightOnly) {
    Write-Log 'preflight ok' 'OK'
    exit 0
  }

  # Disk space: the release, the clang package unpacked and the build need a few GB.
  try {
    $drive = (Split-Path -Qualifier $WorkDir)
    $disk = Get-CimInstance -ClassName Win32_LogicalDisk -Filter ("DeviceID='{0}'" -f $drive)
    if ($disk -and ([double]$disk.FreeSpace / 1GB) -lt 8) {
      $script:Warnings.Add('less than 8 GiB free where the work folder is')
      Write-Log ('only {0:N1} GiB free on {1}; the download and the build need about 8 GiB' -f ([double]$disk.FreeSpace / 1GB), $drive) 'WARN'
    }
  } catch { }

  # 3. Download and verify ---------------------------------------------------------
  if ($cached) {
    $fetchSkipped = $true
    Write-Log 'the fetch folder already holds a complete, verified download: nothing is downloaded' 'OK'
  } else {
    Show-Plan $pins $clangPkg $false
    Write-Log ('downloading (-AllowDownload given): hosts github.com and {0}' -f $ClangHost)
    $null = Run-Step 'fetch' $cmakeExe @(('-DQMEDIA_FETCH_DIR=' + (ConvertTo-Slash $FetchDir)), '-P', (ConvertTo-Slash $fetchScript)) $FetchTimeoutMinutes 2
  }
  $null = Run-Step 'verify_pins' $cmakeExe @(('-DQMEDIA_WEBRTC_DIR=' + (ConvertTo-Slash (Join-Path $FetchDir 'release'))), ('-DQMEDIA_CLANG_ROOT=' + (ConvertTo-Slash (Join-Path $FetchDir 'clang'))), '-P', (ConvertTo-Slash $verifyScript)) 10 2
  $pinsVerified = $true
  $clangPkg = Get-ClangPackage $pins (Join-Path $FetchDir 'release\build-flags.json')
  $clangCl = Join-Path $FetchDir 'clang\bin\clang-cl.exe'
  $toolchain['clang_cl'] = Get-ToolVersion $clangCl '--version' ''

  # 4. Configure and build ---------------------------------------------------------
  if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
    if ((Split-Path -Path $BuildDir -Leaf) -notlike 'build-*') { Stop-Build ('refusing to delete {0}: the folder name must start with build-' -f $BuildDir) 1 }
    Write-Log 'removing the old build folder'
    Remove-Item -LiteralPath $BuildDir -Recurse -Force
  }
  $null = Run-Step 'configure' $cmakeExe @('-S', (ConvertTo-Slash $engineDir), '-B', (ConvertTo-Slash $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', ('-DCMAKE_TOOLCHAIN_FILE=' + (ConvertTo-Slash (Join-Path $FetchDir 'toolchain.cmake'))), '-DQMEDIA_WITH_WEBRTC=ON', '-DQMEDIA_BUILD_TESTS=ON', '-DCMAKE_CXX_FLAGS=') 15 3
  $null = Run-Step 'build' $cmakeExe @('--build', (ConvertTo-Slash $BuildDir), '--target', 'qaudion-media', 'qaudion-media-ci', 'qmedia_call_test') $BuildTimeoutMinutes 4

  # 5. The call test ---------------------------------------------------------------
  $testExe = Join-Path $BuildDir 'qmedia_call_test.exe'
  if (-not (Test-Path -LiteralPath $testExe)) { Stop-Build ('the build finished but {0} is missing' -f $testExe) 4 }
  Write-Log 'step: call test (two engine processes, one call over loopback)'
  $tres = Invoke-LoggedProcess -Exe $testExe -ArgumentString '' -LogFile $script:LogFile -TimeoutMinutes $TestTimeoutMinutes -WorkingDirectory $BuildDir -TailLines 20
  $tok = ($tres.Code -eq 0 -and -not $tres.TimedOut)
  Add-Step 'call_test' $tres $tok
  $testInfo = [ordered]@{ ran = $true; ok = $tok; exit_code = $tres.Code; seconds = $tres.Seconds; timed_out = $tres.TimedOut; tail = @($tres.Tail) }
  if (-not $tok) {
    Stop-Build ('the call test failed (exit code {0}{1}). A Windows Firewall prompt for qaudion-media-ci.exe may be waiting for the owner; the test uses loopback only.' -f $tres.Code, $(if ($tres.TimedOut) { ', timed out' } else { '' })) 5
  }
  Write-Log ('call test: ok in {0} s' -f $tres.Seconds) 'OK'
} catch {
  $exitCode = $script:ExitOnFail
  if (-not $script:FailureMessage) {
    # An error that did not come from Stop-Build: say where it happened, so that it is not lost.
    $script:FailureMessage = $_.Exception.Message
    Write-Log ('unexpected error: {0} (script line {1})' -f $_.Exception.Message, $_.InvocationInfo.ScriptLineNumber) 'ERROR'
  }
  $failedStep = if ($steps.Count -gt 0 -and -not $steps[$steps.Count - 1].ok) { $steps[$steps.Count - 1].name } else { 'preflight' }
}

# ---------------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------------

$total.Stop()
$ok = ($exitCode -eq 0)
$jsonFile = Join-Path $ReportDir ('engine-build-' + $stamp + '.json')
$report = [ordered]@{
  tool          = [ordered]@{ name = 'hwlab-engine-local-build'; report_version = 1 }
  generated_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  ok            = $ok
  exit_code     = $exitCode
  failed_step   = $failedStep
  failure       = $script:FailureMessage
  total_seconds = [math]::Round($total.Elapsed.TotalSeconds, 1)
  repository    = [ordered]@{ commit = $commit; path_kind = 'clone made by setup-hwlab.ps1' }
  release       = [ordered]@{ repository = $pins.Repo; tag = $pins.Tag; pinned_in = 'engine/cmake/webrtc-release.cmake' }
  download      = [ordered]@{
    allowed_by_caller = [bool]$AllowDownload
    skipped_cache_complete = $fetchSkipped
    hosts = @('github.com', $ClangHost)
    clang_package_url = $(if ($clangPkg) { $clangPkg.Url } else { $null })
  }
  pins_verified = [ordered]@{
    all_files_sha256 = $pinsVerified
    method = 'engine/cmake/fetch_webrtc.cmake (every download) and engine/cmake/verify_pins.cmake (after it and again at configure time)'
  }
  attestation   = [ordered]@{
    verified = $false
    skipped  = $true
    reason   = 'gh attestation verify needs the GitHub CLI and a GitHub token, and the lab holds neither by design. CI verifies the attestation of webrtc.lib; locally only the sha256 pins of the repository apply (verify_pins.cmake).'
  }
  toolchain     = $toolchain
  steps         = $steps.ToArray()
  test          = $testInfo
  warnings      = @($script:Warnings)
  log_file      = ('engine-build-' + $stamp + '.log')
}
$json = $report | ConvertTo-Json -Depth 8
$prot = Protect-Json $json
if ($prot.Leaks -gt 0) { Write-Warning ('identifying text was found in the report and replaced by placeholders ({0} pattern(s)); read it before sharing it.' -f $prot.Leaks) }
$null = New-Item -ItemType Directory -Path $ReportDir -Force -ErrorAction SilentlyContinue
[System.IO.File]::WriteAllText($jsonFile, $prot.Text, (New-Object System.Text.UTF8Encoding($false)))

Write-Host ''
Write-Host '----- engine-local-build summary -----'
foreach ($s in $steps) { Write-Host ('  step {0,-12} {1,-6} {2,8} s  exit {3}' -f $s.name, $(if ($s.ok) { 'ok' } else { 'FAILED' }), $s.seconds, $s.exit_code) }
Write-Host ('  attestation  : skipped locally (no gh, no token); sha256 pins verified: {0}' -f $pinsVerified)
Write-Host ('  report       : {0}' -f $jsonFile)
if ($script:LogFile) { Write-Host ('  log          : {0}' -f $script:LogFile) }
if ($ok) { Write-Host '  RESULT: engine built and call test passed' -ForegroundColor Green } else { Write-Host ('  RESULT: failed at {0} (exit code {1})' -f $failedStep, $exitCode) -ForegroundColor Red }
exit $exitCode
