#Requires -Version 5.1
<#
.SYNOPSIS
  Builds and runs the HEVC capability probe (engine/tools/hevc-probe) on this machine.

.DESCRIPTION
  Part of the hwlab kit (engine/tools/hwlab). No administrator rights needed.

    1. Checks the toolchain (CMake, Visual Studio 2022 with the MSVC x64 tools, Windows
       SDK) and stops with a clear message when something is missing.
    2. Configures and builds engine/tools/hevc-probe with CMake and the Visual Studio
       2022 generator, Release, static CRT (as its CMakeLists.txt sets), into
       C:\hwlab\work\build-hevc-probe.
    3. Runs the exe for HEVC and then with --codec h264 (the control run), saves both
       JSON reports in C:\hwlab\reports as hevc-probe-<codec>-<stamp>.json and prints
       the "summary" object of each.

  The H.264 run tells a missing HEVC path from a broken probe: if H.264 passes and HEVC
  does not, the machine has no usable HEVC path. The probe has its own watchdogs and a
  30 minute cap; normally it takes a few minutes.

  Does not fetch the repository unless -Update is given.

  When a codec's 1080p hardware encode is not ok although its 720p one was, the script
  also runs the probe for 1080p alone (--resolutions 1080) in a fresh process and says
  whether the failure depends on the 720p attempts before it.

  Exit code: 0 the reports were written, 1 toolchain or repository problem, 2 configure or
  build failed, 3 a probe run produced no report.

.PARAMETER RepoDir
  The clone made by setup-hwlab.ps1. Default C:\hwlab\work\webrtc-aes256-build.

.PARAMETER BuildDir
  CMake build folder. Default C:\hwlab\work\build-hevc-probe.

.PARAMETER ReportDir
  Where the reports go. Default C:\hwlab\reports.

.PARAMETER Update
  Fast-forward the clone from origin before building (read-only use of the repository).

.PARAMETER Clean
  Delete the build folder first and rebuild from scratch.
#>
[CmdletBinding()]
param(
  [string]$RepoDir   = 'C:\hwlab\work\webrtc-aes256-build',
  [string]$BuildDir  = 'C:\hwlab\work\build-hevc-probe',
  [string]$ReportDir = 'C:\hwlab\reports',
  [switch]$Update,
  [switch]$Clean
)

Set-StrictMode -Version 1.0
$ErrorActionPreference = 'Stop'

$VsCppComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$Generator      = 'Visual Studio 17 2022'

# ---------------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------------

function Write-Step([string]$Text) { Write-Host ''; Write-Host ('== ' + $Text) -ForegroundColor Cyan }

function Fail([string]$Message, [int]$Code) {
  Write-Host ''
  Write-Host ('FAILED: ' + $Message) -ForegroundColor Red
  exit $Code
}

function Update-SessionPath {
  $have = @($env:Path -split ';' | Where-Object { $_ })
  $fresh = @()
  foreach ($scope in 'Machine', 'User') {
    $v = [Environment]::GetEnvironmentVariable('Path', $scope)
    if ($v) { $fresh += @($v -split ';' | Where-Object { $_ }) }
  }
  foreach ($extra in @("$env:ProgramFiles\Git\cmd", "$env:ProgramFiles\CMake\bin")) {
    if (Test-Path -LiteralPath $extra) { $fresh += $extra }
  }
  foreach ($d in $fresh) { if ($have -notcontains $d) { $have += $d } }
  $env:Path = ($have -join ';')
}

function Find-Tool([string]$Name, [string[]]$Fallbacks) {
  $c = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($c) { return $c.Source }
  foreach ($p in $Fallbacks) { if (Test-Path -LiteralPath $p) { return $p } }
  return $null
}

# Console program: output shown (or discarded), exit code returned.
function Invoke-Native([string]$Exe, [string[]]$ArgList, [switch]$DiscardOutput) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    if ($DiscardOutput) { & $Exe @ArgList | Out-Null } else { & $Exe @ArgList | Out-Host }
    return [int]$LASTEXITCODE
  } catch {
    return -1
  } finally {
    $ErrorActionPreference = $old
  }
}

# Console program without a window; returns @{ Code; Out } (stdout only).
function Invoke-Capture([string]$Exe, [string[]]$ArgList, [int]$TimeoutSeconds = 60) {
  $quoted = foreach ($a in $ArgList) {
    if ($a -eq '' -or $a -match '[\s"]') { '"' + ($a -replace '"', '\"') + '"' } else { $a }
  }
  $psi = New-Object System.Diagnostics.ProcessStartInfo
  $psi.FileName = $Exe
  $psi.Arguments = ($quoted -join ' ')
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

function Get-FirstLine([string]$Text) {
  foreach ($l in ($Text -split "`r?`n")) { if ($l.Trim()) { return $l.Trim() } }
  return $null
}

# ---------------------------------------------------------------------------------
# 1. Toolchain
# ---------------------------------------------------------------------------------

Write-Step 'toolchain'
Update-SessionPath

$problems = New-Object System.Collections.Generic.List[string]

$vsPath = $null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (Test-Path -LiteralPath $vswhere) {
  $r = Invoke-Capture $vswhere @('-products', '*', '-version', '[17.0,18.0)', '-requires', $VsCppComponent, '-property', 'installationPath')
  if ($r.Code -eq 0) { $vsPath = Get-FirstLine $r.Out }
}
if ($vsPath) {
  Write-Host '  Visual Studio 2022 with the MSVC x64 tools: found'
} else {
  $problems.Add('Visual Studio 2022 (or its Build Tools) with the C++ x64 tools was not found')
}

# CMake from the kit (Kitware.CMake); the copy that ships inside Visual Studio is the
# fallback.
$cmakeFallbacks = @("$env:ProgramFiles\CMake\bin\cmake.exe")
if ($vsPath) { $cmakeFallbacks += (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe') }
$cmake = Find-Tool 'cmake.exe' $cmakeFallbacks
if ($cmake) {
  $r = Invoke-Capture $cmake @('--version')
  Write-Host ('  cmake      : {0}' -f (Get-FirstLine $r.Out))
} else {
  $problems.Add('CMake was not found')
}

$kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
$sdk = $null
if (Test-Path -LiteralPath $kits) {
  $sdk = Get-ChildItem -Path $kits -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\mfapi.h') } | Select-Object -First 1
}
if ($sdk) {
  Write-Host ('  Windows SDK: {0}' -f $sdk.Name)
} else {
  $problems.Add('the Windows SDK (Media Foundation headers) was not found')
}

$git = Find-Tool 'git.exe' @("$env:ProgramFiles\Git\cmd\git.exe")

if ($problems.Count -gt 0) {
  foreach ($p in $problems) { Write-Host ('  missing    : ' + $p) -ForegroundColor Red }
  Fail 'the toolchain is incomplete. Run setup-hwlab.ps1 once from an elevated PowerShell (a restart may be needed after it), then run this script again.' 1
}

# ---------------------------------------------------------------------------------
# 2. Source
# ---------------------------------------------------------------------------------

Write-Step 'source'
$srcDir = Join-Path $RepoDir 'engine\tools\hevc-probe'
if (-not (Test-Path -LiteralPath (Join-Path $srcDir 'CMakeLists.txt'))) {
  Fail ('the probe sources were not found in {0}. Run setup-hwlab.ps1 first (it clones the repository).' -f $srcDir) 1
}

$gitSafe = @()
if ($git) { $gitSafe = @('-c', ('safe.directory=' + ($RepoDir -replace '\\', '/'))) }

if ($Update) {
  if (-not $git) { Fail 'git was not found, cannot update the repository' 1 }
  $env:GIT_TERMINAL_PROMPT = '0'
  Write-Host '  updating the clone (fast-forward only)'
  $code = Invoke-Native $git (@('-c', 'credential.helper=') + $gitSafe + @('-C', $RepoDir, 'pull', '--ff-only'))
  if ($code -ne 0) { Fail ('git pull --ff-only failed (exit code {0}); inspect the clone by hand' -f $code) 1 }
}

$sha = 'unknown'
if ($git) {
  $r = Invoke-Capture $git ($gitSafe + @('-C', $RepoDir, 'rev-parse', '--short=12', 'HEAD'))
  $line = Get-FirstLine $r.Out
  if ($r.Code -eq 0 -and $line) { $sha = $line }
}
Write-Host ('  repository commit: {0}' -f $sha)

# ---------------------------------------------------------------------------------
# 3. Configure and build
# ---------------------------------------------------------------------------------

Write-Step 'configure and build (Release, static CRT)'
if ($Clean -and (Test-Path -LiteralPath $BuildDir)) {
  if ((Split-Path -Path $BuildDir -Leaf) -notlike 'build-*') {
    Fail ('refusing to delete {0}: the folder name must start with build-' -f $BuildDir) 1
  }
  Write-Host '  removing the old build folder'
  Remove-Item -LiteralPath $BuildDir -Recurse -Force
}

$stopwatch = [System.Diagnostics.Stopwatch]::StartNew()
$code = Invoke-Native $cmake @('-S', $srcDir, '-B', $BuildDir, '-G', $Generator, '-A', 'x64', ('-DPROBE_GIT_SHA=' + $sha))
if ($code -ne 0) {
  Fail ('cmake configure failed (exit code {0}). If the build folder was made with another generator, run again with -Clean.' -f $code) 2
}
$code = Invoke-Native $cmake @('--build', $BuildDir, '--config', 'Release', '--parallel')
if ($code -ne 0) { Fail ('cmake build failed (exit code {0})' -f $code) 2 }

$exe = Join-Path $BuildDir 'Release\hevc-probe.exe'
if (-not (Test-Path -LiteralPath $exe)) { Fail ('the build finished but {0} is missing' -f $exe) 2 }
Write-Host ('  built {0} in {1:N0} s' -f $exe, $stopwatch.Elapsed.TotalSeconds)

# ---------------------------------------------------------------------------------
# 4. Run: HEVC, then the H.264 control
# ---------------------------------------------------------------------------------

if (-not (Test-Path -LiteralPath $ReportDir)) { $null = New-Item -ItemType Directory -Path $ReportDir -Force }
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$exeDir = Split-Path -Path $exe -Parent
$saved = @()
$verdicts = @()

# One probe run. The probe writes the same JSON to stdout and to a file next to the
# exe; the file is used, stdout is discarded. Its progress lines go to stderr and stay
# visible. Returns @{ Json; File; Code } and stops the script when no report was written.
function Invoke-Probe([string]$Codec, [string[]]$ExtraArgs, [string]$Tag) {
  $name = 'probe-' + $Codec + $Tag + '.json'
  $inBuild = Join-Path $exeDir $name
  if (Test-Path -LiteralPath $inBuild) { Remove-Item -LiteralPath $inBuild -Force }
  $code = Invoke-Native $exe (@('--codec', $Codec, '--out', $name) + $ExtraArgs) -DiscardOutput
  if (-not (Test-Path -LiteralPath $inBuild)) {
    Fail ('the {0} probe run{1} wrote no report (exit code {2}); it may have crashed or hit its time limit' -f $Codec, $Tag, $code) 3
  }
  if ($code -ne 0) { Write-Host ('  note: the probe exited with code {0} but wrote a report' -f $code) -ForegroundColor Yellow }
  $dest = Join-Path $ReportDir ('hevc-probe-{0}{1}-{2}.json' -f $Codec, $Tag, $stamp)
  Copy-Item -LiteralPath $inBuild -Destination $dest -Force
  $json = Get-Content -LiteralPath $dest -Raw -Encoding UTF8 | ConvertFrom-Json
  return @{ Json = $json; File = $dest; Code = $code }
}

function Get-SummaryValue($Json, [string]$Codec, [string]$Name) {
  $key = $Codec + '_' + $Name
  $prop = $Json.summary.PSObject.Properties[$key]
  if ($prop) { return [string]$prop.Value }
  return $null
}

foreach ($codec in @('hevc', 'h264')) {
  Write-Step ('probe, codec {0}' -f $codec)
  $run = Invoke-Probe $codec @() ''
  $saved += $run.File
  Write-Host ('  report: {0}' -f $run.File)
  Write-Host '  summary (ok / failed / not_attempted; a not attempted result is not a failure):'
  ($run.Json.summary | ConvertTo-Json -Depth 4) -split "`r?`n" | ForEach-Object { Write-Host ('    ' + $_) }

  # A 1080p encode that fails after a 720p encode that worked may be caused by state
  # left over from the 720p attempts in the same process, or may be independent of it.
  # Run 1080p alone in a fresh process to tell the two apart.
  $s720 = Get-SummaryValue $run.Json $codec 'hardware_encode_720p_status'
  $s1080 = Get-SummaryValue $run.Json $codec 'hardware_encode_1080p_status'
  if ($s720 -eq 'ok' -and $s1080 -ne 'ok') {
    Write-Step ('probe, codec {0}: 1080p alone in a fresh process (the full run did not encode 1080p in hardware)' -f $codec)
    $iso = Invoke-Probe $codec @('--resolutions', '1080') '-1080only'
    $saved += $iso.File
    $isoStatus = Get-SummaryValue $iso.Json $codec 'hardware_encode_1080p_status'
    if ($isoStatus -eq 'ok') {
      $text = 'ORDER-DEPENDENT: 1080p hardware encode works in a fresh process but not after the 720p attempts of the same process (state left over by an earlier attempt)'
    } else {
      $text = ('NOT order-dependent: 1080p hardware encode is {0} in a fresh process too (a resolution or driver limit, not leftover state)' -f $isoStatus)
    }
    Write-Host ('  verdict: ' + $text) -ForegroundColor Yellow
    $verdicts += [ordered]@{ codec = $codec; full_run_1080p = $s1080; fresh_process_1080p = $isoStatus; verdict = $text }
  }
}

if ($verdicts.Count -gt 0) {
  $vfile = Join-Path $ReportDir ('hevc-probe-1080-verdict-{0}.json' -f $stamp)
  [System.IO.File]::WriteAllText($vfile, (($verdicts | ConvertTo-Json -Depth 4)), (New-Object System.Text.UTF8Encoding($false)))
  $saved += $vfile
}

Write-Host ''
Write-Host 'RESULT: the probe runs wrote their reports' -ForegroundColor Green
foreach ($s in $saved) { Write-Host ('  ' + $s) }
exit 0
