#Requires -Version 5.1
<#
.SYNOPSIS
  Real-audio bench of the desktop media engine (hwlab kit, engine/tools/hwlab). No
  administrator rights needed.

.DESCRIPTION
  Two engine processes call each other over loopback with the strict transport and the
  frame keys of the engine call test. The first captures from a real microphone, the
  second plays to real speakers (or Bluetooth earbuds) for -Seconds seconds. The result
  is a JSON report in <ReportDir>\audio-bench-<stamp>.json: capture and playout levels
  (dBFS), concealed samples (the closest the statistics get to glitches), a latency
  estimate, the DTLS/SRTP verdict and an overall verdict. A Bluetooth endpoint that only
  offers the hands-free profile (16 kHz mono) is noted as an observation.

  NOTHING IS RECORDED OR PLAYED unless -Real is given. Run -Real only when someone is
  at the machine to speak. The other modes never open a microphone or speakers:

    -Build      Configure and build qmedia_audio_bench, qaudion-media-hw and
                qaudion-media-ci into <WorkDir>\<BuildName> from the verified download in
                <WorkDir>\webrtc-fetch (nothing is downloaded).
    -List       List the capture and render devices of the Windows audio device module.
    -SelfCheck  -List plus a session and certificate round trip. No stream is opened.
    -Dry        The whole call with file devices (the CI engine) instead of the sound
                card: proves the bench logic, the strict transport and the report.
    -Real       The real test (microphone and speakers).

  Start it in the background and follow the log: it prints a line per second.
  Device names are scrubbed (account name, machine name, possessive owner names such as
  "<owner>'s", MAC and IP patterns) with the helpers of hwlab-common.ps1 before the
  report is written. Check the report for names anyway before it leaves the machine.

  Exit code: 0 done (a "transport-ok-no-signal" verdict is also 0: nobody spoke),
  1 test failed, 2 bad usage or toolchain problem, 3 no device matches a name part.

.PARAMETER Capture
  Part of the capture device name (case-insensitive). Empty: the system default.

.PARAMETER Render
  Part of the render device name (case-insensitive). Empty: the system default.

.PARAMETER Seconds
  Length of the measured window (3 to 600, default 10).

.EXAMPLE
  .\audio-bench.ps1 -Real -Seconds 10
.EXAMPLE
  .\audio-bench.ps1 -Real -Capture 'Headset' -Render 'Headphones' -Seconds 15
#>
[CmdletBinding()]
param(
  [string]$Capture = '',
  [string]$Render = '',
  [int]$Seconds = 10,
  [switch]$Real,
  [switch]$Dry,
  [switch]$List,
  [switch]$SelfCheck,
  [switch]$Build,
  [string]$WorkDir = 'C:\hwlab\work',
  [string]$BuildName = 'build-audiobench',
  [string]$ReportDir = 'C:\hwlab\reports',
  [int]$BuildTimeoutMinutes = 90,
  [int]$RunTimeoutMinutes = 20
)

Set-StrictMode -Version 1.0
$ErrorActionPreference = 'Stop'

. (Join-Path $PSScriptRoot 'hwlab-common.ps1')
Initialize-Scrubbing

$engineDir = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$BuildDir = Join-Path $WorkDir $BuildName
$FetchDir = Join-Path $WorkDir 'webrtc-fetch'
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$RunDir = Join-Path $WorkDir 'audiobench-run'
$bench = Join-Path $BuildDir 'qmedia_audio_bench.exe'

function Write-Log([string]$Message) {
  Write-Host ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), (Protect-Text $Message))
}

function Update-SessionPath {
  $have = @($env:Path -split ';' | Where-Object { $_ })
  foreach ($scope in 'Machine', 'User') {
    $v = [Environment]::GetEnvironmentVariable('Path', $scope)
    if ($v) { foreach ($d in @($v -split ';' | Where-Object { $_ })) { if ($have -notcontains $d) { $have += $d } } }
  }
  $env:Path = ($have -join ';')
}

function Import-VcVars {
  $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (-not (Test-Path -LiteralPath $vswhere)) { throw 'vswhere was not found' }
  $path = (& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath | Select-Object -First 1)
  if (-not $path) { throw 'no Visual Studio with the MSVC x64 tools was found' }
  $vcvars = Join-Path $path 'VC\Auxiliary\Build\vcvars64.bat'
  if (-not (Test-Path -LiteralPath $vcvars)) { throw 'vcvars64.bat was not found' }
  $lines = & cmd.exe /d /c ('"' + $vcvars + '" >nul 2>&1 && set')
  foreach ($l in $lines) {
    $i = $l.IndexOf('=')
    if ($i -gt 0) { Set-Item -LiteralPath ('Env:' + $l.Substring(0, $i)) -Value $l.Substring($i + 1) }
  }
}

function Invoke-Logged([string]$Exe, [string[]]$ArgList, [string]$LogPath, [int]$Minutes) {
  $outFile = $LogPath
  $errFile = $LogPath + '.err'
  $quoted = ($ArgList | ForEach-Object { if ($_ -match '[\s"]') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $p = Start-Process -FilePath $Exe -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
  $null = $p.Handle   # keeps the exit code readable after the process ends (Windows PowerShell 5.1)
  if (-not $p.WaitForExit($Minutes * 60 * 1000)) {
    try { $p.Kill() } catch { }
    throw "timed out after $Minutes minutes: $LogPath"
  }
  $p.WaitForExit()
  return [int]$p.ExitCode
}

# ---------------------------------------------------------------------------------
# Build (nothing is downloaded)
# ---------------------------------------------------------------------------------

function Invoke-Build {
  Update-SessionPath
  $tc = Join-Path $FetchDir 'toolchain.cmake'
  if (-not (Test-Path -LiteralPath $tc)) { throw 'the verified download is missing in the fetch folder; run engine-local-build.ps1 first' }
  Import-VcVars
  $cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
  $null = (Get-Command ninja.exe -ErrorAction Stop)
  New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
  $slash = { param($p) $p -replace '\\', '/' }
  Write-Log 'configure'
  $rc = Invoke-Logged $cmake @('-S', (& $slash $engineDir), '-B', (& $slash $BuildDir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    ('-DCMAKE_TOOLCHAIN_FILE=' + (& $slash $tc)), '-DQMEDIA_WITH_WEBRTC=ON', '-DQMEDIA_BUILD_TESTS=ON', '-DCMAKE_CXX_FLAGS=') (Join-Path $RunDir 'configure.log') 15
  if ($rc -ne 0) { throw "configure failed (exit $rc), see $RunDir\configure.log" }
  Write-Log 'build'
  $rc = Invoke-Logged $cmake @('--build', (& $slash $BuildDir), '--target', 'qaudion-media', 'qaudion-media-ci', 'qaudion-media-hw', 'qmedia_call_test', 'qmedia_audio_bench') (Join-Path $RunDir 'build.log') $BuildTimeoutMinutes
  if ($rc -ne 0) { throw "build failed (exit $rc), see $RunDir\build.log" }
  Write-Log 'build done'
}

# ---------------------------------------------------------------------------------
# Run the driver
# ---------------------------------------------------------------------------------

function Get-ScrubbedReport([string]$Path) {
  $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
  # Device names: every string value under these keys goes through the text scrubber (possessives too).
  $eval = [System.Text.RegularExpressions.MatchEvaluator] {
    param($m)
    '"' + $m.Groups[1].Value + '":"' + (Protect-Text $m.Groups[2].Value) + '"'
  }
  $text = [regex]::Replace($text, '"(name|capture_used|render_used)":"((?:[^"\\]|\\.)*)"', $eval)
  $r = Protect-Json $text
  $null = $r.Text | ConvertFrom-Json   # must still be valid JSON
  return $r
}

function Invoke-Driver([string[]]$DriverArgs, [string]$Tag) {
  if (-not (Test-Path -LiteralPath $bench)) { throw "the bench is not built: $bench (run with -Build)" }
  New-Item -ItemType Directory -Force -Path $RunDir | Out-Null
  $log = Join-Path $RunDir ("audio-bench-$Tag-$stamp.log")
  $errFile = $log + '.err'
  $quoted = ($DriverArgs | ForEach-Object { if ($_ -match '[\s"]' -or $_ -eq '') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ } }) -join ' '
  $p = Start-Process -FilePath $bench -ArgumentList $quoted -NoNewWindow -PassThru -RedirectStandardOutput $log -RedirectStandardError $errFile
  $null = $p.Handle   # keeps the exit code readable after the process ends (Windows PowerShell 5.1)
  $pos = 0
  $deadline = (Get-Date).AddMinutes($RunTimeoutMinutes)
  while (-not $p.HasExited) {
    Start-Sleep -Milliseconds 500
    if (Test-Path -LiteralPath $log) {
      $lines = @(Get-Content -LiteralPath $log -Encoding UTF8)
      for (; $pos -lt $lines.Count; $pos++) { Write-Log $lines[$pos] }
    }
    if ((Get-Date) -gt $deadline) { try { $p.Kill() } catch { }; throw 'the bench ran too long and was stopped' }
  }
  $p.WaitForExit()
  if (Test-Path -LiteralPath $log) {
    $lines = @(Get-Content -LiteralPath $log -Encoding UTF8)
    for (; $pos -lt $lines.Count; $pos++) { Write-Log $lines[$pos] }
  }
  return [int]$p.ExitCode
}

try {
  $modes = @($Real, $Dry, $List, $SelfCheck, $Build) | Where-Object { $_ }
  if (@($modes).Count -eq 0) {
    Write-Host 'Choose a mode: -Build, -List, -SelfCheck, -Dry or -Real (only -Real records and plays). See Get-Help.'
    exit 2
  }
  if ($Seconds -lt 3 -or $Seconds -gt 600) { throw '-Seconds must be between 3 and 600' }
  New-Item -ItemType Directory -Force -Path $RunDir | Out-Null

  if ($Build) { Invoke-Build }

  if ($List -or $SelfCheck) {
    $flag = '--list'
    if ($SelfCheck) { $flag = '--selfcheck' }
    $rc = Invoke-Driver @($flag, '--work', $RunDir) 'list'
    if ($rc -ne 0) { exit 1 }
  }

  if ($Dry -or $Real) {
    New-Item -ItemType Directory -Force -Path $ReportDir | Out-Null
    $tmpJson = Join-Path $RunDir ("report-$stamp.json")
    $mode = '--dry'
    $name = 'dry'
    if ($Real) { $mode = '--real'; $name = 'real' }
    $dargs = @($mode, '--work', $RunDir, '--seconds', [string]$Seconds, '--out', $tmpJson)
    if ($Real) { $dargs += @('--capture', $Capture, '--render', $Render) }
    if ($Real) { Write-Log 'REAL test: the microphone and the speakers are opened now. Speak for the whole run.' }
    $rc = Invoke-Driver $dargs $name
    if (Test-Path -LiteralPath $tmpJson) {
      $final = Get-ScrubbedReport $tmpJson
      $dest = Join-Path $ReportDir ("audio-bench-$stamp.json")
      if ($Dry) { $dest = Join-Path $ReportDir ("audio-bench-dry-$stamp.json") }
      [System.IO.File]::WriteAllText($dest, $final.Text, (New-Object System.Text.UTF8Encoding($false)))
      Remove-Item -LiteralPath $tmpJson -Force
      Write-Log ("report written: $dest (scrubbed rules that changed text: " + $final.Leaks + ')')
    } else {
      Write-Log 'no report was written'
    }
    exit $rc
  }
  exit 0
} catch {
  Write-Host ('audio-bench: ' + (Protect-Text $_.Exception.Message))
  exit 2
}
