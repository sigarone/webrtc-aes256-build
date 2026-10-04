#Requires -Version 5.1
<#
.SYNOPSIS
  One-time setup of the hardware lab PC (hwlab kit, engine/tools/hwlab).

.DESCRIPTION
  Run once from an ELEVATED Windows PowerShell 5.1 (or newer). Idempotent: whatever is
  already present is skipped, so a second run is safe.

    1. Checks the machine: Windows 10 22H2 or Windows 11, x64, at least 8 GB RAM,
       at least 40 GB free on the system drive.
    2. Installs with winget (exact ids, silent): Git.Git, Python.Python.3.12,
       Kitware.CMake, Ninja-build.Ninja and Microsoft.VisualStudio.2022.BuildTools
       with the C++ workload. Every id is verified with `winget show` first.
       The HEVC Video Extensions are NOT installed: the probe must see the machine
       as it is. They are only reported: registered for the current user (the value
       that counts), provisioned in the system image, registered for any user.
    3. Creates C:\hwlab with work and reports subfolders.
    4. Clones https://github.com/sigarone/webrtc-aes256-build into
       C:\hwlab\work\webrtc-aes256-build, or updates it (fast-forward only). The clone
       is for reading: its push URL is disabled and no credential helper is used.
    5. Prints a summary and writes it to C:\hwlab\reports\setup-last.json (the step log
       is C:\hwlab\reports\setup-last.log), so that a non-elevated session can read the
       result of an elevated run.

  No telemetry, no download from anywhere except winget and this repository.

  Exit code: 0 ready, 1 a check failed or not elevated (nothing installed), 2 an
  install or the repository step failed.

.PARAMETER Branch
  Branch of the repository to keep checked out. Default main.
#>
[CmdletBinding()]
param(
  [string]$Branch = 'main'
)

Set-StrictMode -Version 1.0
$ErrorActionPreference = 'Stop'

$RepoUrl       = 'https://github.com/sigarone/webrtc-aes256-build'
$LabRoot       = 'C:\hwlab'
$WorkDir       = Join-Path $LabRoot 'work'
$ReportsDir    = Join-Path $LabRoot 'reports'
$RepoDir       = Join-Path $WorkDir 'webrtc-aes256-build'
$script:LogFile     = Join-Path $ReportsDir 'setup-last.log'
$SummaryFile   = Join-Path $ReportsDir 'setup-last.json'

$MinBuild      = 19045          # Windows 10 22H2; Windows 11 builds are higher
$MinRamGiB     = 7.5            # an "8 GB" machine reports a little less than 8 GiB
$MinFreeGiB    = 40
$VsCppComponent = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
$VsOverride    = '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended'

# The five packages of the kit and nothing else. The HEVC Video Extensions are
# deliberately absent.
$Packages = @(
  [pscustomobject]@{ Id = 'Git.Git';                                  Label = 'Git';                    Override = $null }
  [pscustomobject]@{ Id = 'Python.Python.3.12';                       Label = 'Python 3.12';            Override = $null }
  [pscustomobject]@{ Id = 'Kitware.CMake';                            Label = 'CMake';                  Override = $null }
  [pscustomobject]@{ Id = 'Ninja-build.Ninja';                        Label = 'Ninja';                  Override = $null }
  [pscustomobject]@{ Id = 'Microsoft.VisualStudio.2022.BuildTools';   Label = 'VS 2022 Build Tools';    Override = $VsOverride }
)

$script:ExitCode       = 0
$script:RebootRequired = $false
$script:LogReady       = $false
$script:LogLines       = New-Object System.Collections.Generic.List[string]
$script:WingetPath     = $null

# ---------------------------------------------------------------------------------
# Logging (no user name or machine name ends up in the log)
# ---------------------------------------------------------------------------------

function Protect-Log([string]$Text) {
  $s = $Text
  foreach ($v in @($env:USERNAME, $env:COMPUTERNAME)) {
    if ($v -and $v.Length -ge 2) {
      $s = [regex]::Replace($s, '(?<![\p{L}\p{N}])' + [regex]::Escape($v) + '(?![\p{L}\p{N}])', '<redacted>', 'IgnoreCase')
    }
  }
  return $s
}

function Write-Log([string]$Message, [string]$Level = 'INFO') {
  $line = ('[{0}] {1,-5} {2}' -f (Get-Date -Format 'HH:mm:ss'), $Level, (Protect-Log $Message))
  switch ($Level) {
    'ERROR' { Write-Host $line -ForegroundColor Red }
    'WARN'  { Write-Host $line -ForegroundColor Yellow }
    'OK'    { Write-Host $line -ForegroundColor Green }
    default { Write-Host $line }
  }
  $script:LogLines.Add($line)
  if ($script:LogReady) {
    try { [System.IO.File]::AppendAllText($script:LogFile, $line + "`r`n") } catch { }
  }
}

function Stop-Lab([string]$Message, [int]$Code) {
  $script:ExitCode = $Code
  throw $Message
}

# ---------------------------------------------------------------------------------
# Process helpers
# ---------------------------------------------------------------------------------

# Runs a console program, shows its output, returns its exit code. Output text is
# never parsed (winget is localized); only exit codes are used.
function Invoke-Visible([string]$Exe, [string[]]$ArgList) {
  $old = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  try {
    & $Exe @ArgList | Out-Host
    return [int]$LASTEXITCODE
  } catch {
    return -1
  } finally {
    $ErrorActionPreference = $old
  }
}

# Runs a program without a window, captures stdout, returns @{ Code; Out }.
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
  try {
    $p = [System.Diagnostics.Process]::Start($psi)
  } catch {
    return @{ Code = -1; Out = '' }
  }
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
  foreach ($extra in @("$env:ProgramFiles\Git\cmd", "$env:ProgramFiles\CMake\bin", "$env:ProgramFiles\WinGet\Links")) {
    if (Test-Path -LiteralPath $extra) { $fresh += $extra }
  }
  foreach ($d in $fresh) {
    if ($have -notcontains $d) { $have += $d }
  }
  $env:Path = ($have -join ';')
}

function Find-Tool([string]$Name, [string[]]$Fallbacks) {
  $c = Get-Command $Name -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
  if ($c) { return $c.Source }
  foreach ($pattern in $Fallbacks) {
    $hit = Get-Item -Path $pattern -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hit) { return $hit.FullName }
  }
  return $null
}

# ---------------------------------------------------------------------------------
# Detection of what is installed
# ---------------------------------------------------------------------------------

function Get-VsWhere {
  $p = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
  if (Test-Path -LiteralPath $p) { return $p }
  return $null
}

function Get-FirstLine([string]$Text) {
  foreach ($l in ($Text -split "`r?`n")) { if ($l.Trim()) { return $l.Trim() } }
  return $null
}

# Installation path of a Visual Studio 2022 (17.x) that has the MSVC x64 tools.
function Get-VsCppInstallPath {
  $vw = Get-VsWhere
  if (-not $vw) { return $null }
  $r = Invoke-Capture $vw @('-products', '*', '-version', '[17.0,18.0)', '-requires', $VsCppComponent, '-property', 'installationPath')
  if ($r.Code -ne 0) { return $null }
  return (Get-FirstLine $r.Out)
}

# Installation path of a VS 2022 Build Tools instance, with or without the C++ tools.
function Get-BuildToolsInstallPath {
  $vw = Get-VsWhere
  if (-not $vw) { return $null }
  $r = Invoke-Capture $vw @('-products', 'Microsoft.VisualStudio.Product.BuildTools', '-version', '[17.0,18.0)', '-property', 'installationPath')
  if ($r.Code -ne 0) { return $null }
  return (Get-FirstLine $r.Out)
}

function Get-WindowsSdkFound {
  $kits = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\Include'
  if (-not (Test-Path -LiteralPath $kits)) { return $false }
  $hit = Get-ChildItem -Path $kits -Directory -ErrorAction SilentlyContinue |
    Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'um\mfapi.h') } | Select-Object -First 1
  return [bool]$hit
}

# Returns the version string of an installed package, or $null when it is missing.
function Get-InstalledVersion([string]$Id) {
  switch ($Id) {
    'Git.Git' {
      $g = Find-Tool 'git.exe' @("$env:ProgramFiles\Git\cmd\git.exe")
      if (-not $g) { return $null }
      $r = Invoke-Capture $g @('--version')
      $v = Get-FirstLine $r.Out
      if ($v) { return ($v -replace '^git version\s+', '') }
      return 'unknown'
    }
    'Python.Python.3.12' {
      $py = Find-Tool 'py.exe' @("$env:SystemRoot\py.exe", "$env:LOCALAPPDATA\Programs\Python\Launcher\py.exe")
      if ($py) {
        $r = Invoke-Capture $py @('-3.12', '--version')
        if ($r.Code -eq 0) { return ((Get-FirstLine $r.Out) -replace '^Python\s+', '') }
      }
      $exe = Find-Tool 'python3.12.exe' @("$env:ProgramFiles\Python312\python.exe", "$env:LOCALAPPDATA\Programs\Python\Python312\python.exe")
      if ($exe) {
        $r = Invoke-Capture $exe @('--version')
        if ($r.Code -eq 0) { return ((Get-FirstLine $r.Out) -replace '^Python\s+', '') }
      }
      return $null
    }
    'Kitware.CMake' {
      $c = Find-Tool 'cmake.exe' @("$env:ProgramFiles\CMake\bin\cmake.exe")
      if (-not $c) { return $null }
      $r = Invoke-Capture $c @('--version')
      $v = Get-FirstLine $r.Out
      if ($v) { return ($v -replace '^cmake version\s+', '') }
      return 'unknown'
    }
    'Ninja-build.Ninja' {
      $n = Find-Tool 'ninja.exe' @("$env:ProgramFiles\WinGet\Links\ninja.exe", "$env:LOCALAPPDATA\Microsoft\WinGet\Links\ninja.exe", "$env:ProgramFiles\WinGet\Packages\Ninja-build.Ninja_*\ninja.exe", "$env:LOCALAPPDATA\Microsoft\WinGet\Packages\Ninja-build.Ninja_*\ninja.exe")
      if (-not $n) { return $null }
      $r = Invoke-Capture $n @('--version')
      $v = Get-FirstLine $r.Out
      if ($v) { return $v }
      return 'unknown'
    }
    'Microsoft.VisualStudio.2022.BuildTools' {
      $vw = Get-VsWhere
      $path = Get-VsCppInstallPath
      if (-not $path) { return $null }
      $r = Invoke-Capture $vw @('-products', '*', '-version', '[17.0,18.0)', '-requires', $VsCppComponent, '-property', 'installationVersion')
      $v = Get-FirstLine $r.Out
      if ($v) { return $v }
      return 'unknown'
    }
  }
  return $null
}

# ---------------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------------

function Test-Elevated {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

function Test-Machine {
  $checks = @()

  $os = Get-CimInstance -ClassName Win32_OperatingSystem
  $build = [int]$os.BuildNumber
  $checks += [pscustomobject]@{
    name = 'windows'; ok = ($build -ge $MinBuild)
    detail = ('build {0}; Windows 10 22H2 (build {1}) or Windows 11 is required' -f $build, $MinBuild)
  }

  $arch = [int](@(Get-CimInstance -ClassName Win32_Processor)[0].Architecture)
  $checks += [pscustomobject]@{
    name = 'x64'; ok = ($arch -eq 9)
    detail = $(if ($arch -eq 9) { 'x64' } else { 'processor architecture code ' + $arch + '; x64 is required' })
  }

  $ram = [double](Get-CimInstance -ClassName Win32_ComputerSystem).TotalPhysicalMemory / 1GB
  $checks += [pscustomobject]@{
    name = 'ram'; ok = ($ram -ge $MinRamGiB)
    detail = ('{0:N1} GiB installed; 8 GB or more is required' -f $ram)
  }

  $drive = $env:SystemDrive
  $disk = Get-CimInstance -ClassName Win32_LogicalDisk -Filter ("DeviceID='{0}'" -f $drive)
  $free = 0.0
  if ($disk) { $free = [double]$disk.FreeSpace / 1GB }
  $checks += [pscustomobject]@{
    name = 'disk'; ok = ($free -ge $MinFreeGiB)
    detail = ('{0:N0} GiB free on {1}; {2} GiB or more is required' -f $free, $drive, $MinFreeGiB)
  }

  return $checks
}

function Install-LabPackage($Pkg) {
  # Build Tools without the C++ workload: extend the existing instance instead of
  # asking winget to install a second one.
  if ($Pkg.Id -eq 'Microsoft.VisualStudio.2022.BuildTools') {
    $existing = Get-BuildToolsInstallPath
    if ($existing) {
      $setup = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\setup.exe'
      if (-not (Test-Path -LiteralPath $setup)) { return -1 }
      Write-Log 'VS 2022 Build Tools is present without the C++ workload: adding it with the Visual Studio installer'
      $argLine = 'modify --installPath "{0}" --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --quiet --norestart --wait' -f $existing
      $proc = Start-Process -FilePath $setup -ArgumentList $argLine -Wait -PassThru
      return [int]$proc.ExitCode
    }
  }
  $argList = @('install', '--id', $Pkg.Id, '--exact', '--source', 'winget', '--silent',
    '--accept-package-agreements', '--accept-source-agreements')
  if ($Pkg.Override) { $argList += @('--override', $Pkg.Override) }
  return (Invoke-Visible $script:WingetPath $argList)
}

function Sync-Repository {
  $git = Find-Tool 'git.exe' @("$env:ProgramFiles\Git\cmd\git.exe")
  if (-not $git) { Stop-Lab 'git is not available, cannot clone the repository' 2 }

  # Never wait for a credential prompt: the repository is public and the lab holds no
  # credentials. A failing fetch must fail, not hang.
  $env:GIT_TERMINAL_PROMPT = '0'
  $safe = $RepoDir -replace '\\', '/'

  if (-not (Test-Path -LiteralPath (Join-Path $RepoDir '.git'))) {
    if ((Test-Path -LiteralPath $RepoDir) -and @(Get-ChildItem -LiteralPath $RepoDir -Force -ErrorAction SilentlyContinue).Count -gt 0) {
      Stop-Lab ("{0} exists, is not empty and is not a git clone; move it away and run again" -f $RepoDir) 2
    }
    Write-Log ('cloning {0}' -f $RepoUrl)
    $code = Invoke-Visible $git @('-c', 'credential.helper=', '-c', 'core.longpaths=true', 'clone', '--branch', $Branch, $RepoUrl, $RepoDir)
    if ($code -ne 0) { Stop-Lab ('git clone failed with exit code {0}' -f $code) 2 }
  } else {
    $r = Invoke-Capture $git @('-c', ('safe.directory=' + $safe), '-C', $RepoDir, 'remote', 'get-url', 'origin')
    $origin = (Get-FirstLine $r.Out)
    if (-not $origin -or (($origin -replace '\.git$', '').TrimEnd('/') -ine $RepoUrl)) {
      Stop-Lab ('the clone in {0} does not point at {1}; move it away and run again' -f $RepoDir, $RepoUrl) 2
    }
    $r = Invoke-Capture $git @('-c', ('safe.directory=' + $safe), '-C', $RepoDir, 'status', '--porcelain')
    if ($r.Code -ne 0) { Stop-Lab 'git status failed in the existing clone' 2 }
    if (Get-FirstLine $r.Out) {
      Stop-Lab 'the existing clone has local changes; the lab uses it read-only. Inspect it, then remove or fix it by hand' 2
    }
    Write-Log 'updating the existing clone (fast-forward only)'
    $code = Invoke-Visible $git @('-c', 'credential.helper=', '-c', ('safe.directory=' + $safe), '-C', $RepoDir, 'fetch', '--prune', 'origin')
    if ($code -ne 0) { Stop-Lab ('git fetch failed with exit code {0}' -f $code) 2 }
    $r = Invoke-Capture $git @('-c', ('safe.directory=' + $safe), '-C', $RepoDir, 'rev-parse', '--abbrev-ref', 'HEAD')
    if ((Get-FirstLine $r.Out) -ne $Branch) {
      $code = Invoke-Visible $git @('-c', ('safe.directory=' + $safe), '-C', $RepoDir, 'checkout', $Branch)
      if ($code -ne 0) { Stop-Lab ('git checkout {0} failed with exit code {1}' -f $Branch, $code) 2 }
    }
    $code = Invoke-Visible $git @('-c', ('safe.directory=' + $safe), '-C', $RepoDir, 'merge', '--ff-only', ('origin/' + $Branch))
    if ($code -ne 0) { Stop-Lab ('the clone cannot be fast-forwarded to origin/{0} (it has diverged); fix it by hand' -f $Branch) 2 }
  }

  # Folders created by an elevated process are owned by the Administrators group, and
  # git refuses to work in them from a non-elevated shell ("dubious ownership").
  $r = Invoke-Capture $git @('config', '--global', '--get-all', 'safe.directory')
  if (($r.Out -split "`r?`n") -notcontains $safe) {
    $null = Invoke-Capture $git @('config', '--global', '--add', 'safe.directory', $safe)
  }
  # Read-only use: a push has nowhere to go, and no credential helper is consulted.
  $null = Invoke-Capture $git @('-C', $RepoDir, 'remote', 'set-url', '--push', 'origin', 'no_push')
  $null = Invoke-Capture $git @('-C', $RepoDir, 'config', 'credential.helper', '')
  $null = Invoke-Capture $git @('-C', $RepoDir, 'config', 'core.longpaths', 'true')

  $r = Invoke-Capture $git @('-C', $RepoDir, 'rev-parse', '--short=12', 'HEAD')
  return (Get-FirstLine $r.Out)
}

# ---------------------------------------------------------------------------------
# The HEVC Video Extensions (the same text as in hwlab-common.ps1; CI compares the two)
# ---------------------------------------------------------------------------------

#region hevc-extensions
# One definition of "the HEVC Video Extensions are installed". The text of this region is
# identical in hwlab-common.ps1 and setup-hwlab.ps1 (CI compares them); change both.
#
# The packages are registered per user, and an application process sees only the packages
# registered for the user it runs as. So three facts are reported, each with its own name:
#
#   registered_current_user   Get-AppxPackage for the current user. This is the value the
#                             summaries use ("installed"): it is what the application sees.
#   registered_any_user       Get-AppxPackage -AllUsers. Needs elevation, otherwise $null.
#                             A package registered for another account does not help the
#                             current user (an elevated "all users" query once said true
#                             while the probe, running as the current user, saw nothing).
#   provisioned_system_image  Get-AppxProvisionedPackage -Online. Needs elevation, otherwise
#                             $null. Provisioned means: part of the system image and
#                             installed for users when they first sign in, not for accounts
#                             that already exist.
#
# $null always means "could not be read", never "no".
#
# Besides the known families, any package registered for the current user whose name matches
# *HEVC* is listed (hevc_named_packages_current_user), with a flag that says whether its family
# is in the list below, so that a new variant of the extension is seen the day it appears, and
# is not reported as "not installed" because nobody added its name yet. The families are:
#   Microsoft.HEVCVideoExtension_8wekyb3d8bbwe              HEVC Video Extensions
#   Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe             HEVC Video Extensions from Device Manufacturer
#   Microsoft.HEVCVideoExtensionFirstParty_8wekyb3d8bbwe    seen on the lab PC (Windows build 26300),
#                                                           not described in any public documentation
#                                                           found when it was added
# The probe (engine/tools/hevc-probe) checks the same three families; CI compares the names.
function Get-HevcExtensionStatus {
  $families = @('Microsoft.HEVCVideoExtension_8wekyb3d8bbwe', 'Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe', 'Microsoft.HEVCVideoExtensionFirstParty_8wekyb3d8bbwe')
  $names = @($families | ForEach-Object { $_.Substring(0, $_.IndexOf('_')) })
  $elevated = $false
  try {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $elevated = ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
  } catch { }

  $current = $null
  $currentList = @()
  try {
    $found = @()
    foreach ($n in $names) {
      $found += @(Get-AppxPackage -Name $n -ErrorAction Stop | Where-Object { $families -contains $_.PackageFamilyName })
    }
    $current = ($found.Count -gt 0)
    foreach ($p in $found) { $currentList += ('{0} {1}' -f $p.Name, $p.Version) }
  } catch { $current = $null }

  $named = $null
  $namedNotListed = $null
  try {
    $named = @(Get-AppxPackage -Name '*HEVC*' -ErrorAction Stop | ForEach-Object {
        [ordered]@{
          name                 = [string]$_.Name
          version              = [string]$_.Version
          family               = [string]$_.PackageFamilyName
          status               = [string]$_.Status
          in_known_family_list = ($families -contains $_.PackageFamilyName)
        }
      })
    $namedNotListed = @($named | Where-Object { -not $_.in_known_family_list }).Count
  } catch { $named = $null; $namedNotListed = $null }

  $anyUser = $null
  if ($elevated) {
    try {
      $all = @()
      foreach ($n in $names) {
        $all += @(Get-AppxPackage -AllUsers -Name $n -ErrorAction Stop | Where-Object { $families -contains $_.PackageFamilyName })
      }
      $anyUser = ($all.Count -gt 0)
    } catch { $anyUser = $null }
  }

  $prov = $null
  if ($elevated) {
    try {
      $pp = @(Get-AppxProvisionedPackage -Online -ErrorAction Stop | Where-Object {
          $pub = ([string]$_.PackageName).Substring(([string]$_.PackageName).LastIndexOf('_') + 1)
          $families -contains ('{0}_{1}' -f $_.DisplayName, $pub)
        })
      $prov = ($pp.Count -gt 0)
    } catch { $prov = $null }
  }

  return [ordered]@{
    family_names                     = $families
    summary_basis                    = 'registered_current_user'
    installed                        = $current
    registered_current_user          = $current
    registered_current_user_packages = @($currentList)
    hevc_named_packages_current_user = $named
    hevc_named_packages_not_in_family_list = $namedNotListed
    registered_any_user              = $anyUser
    provisioned_system_image         = $prov
    elevated                         = $elevated
  }
}
#endregion hevc-extensions

# ---------------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------------

$checks   = @()
$pkgState = @()
$repoSha  = $null
$hevcExtensions = $null
$sdkFound = $null

try {
  Write-Host ''
  Write-Host 'hwlab setup'
  Write-Host ''

  if (-not (Test-Elevated)) {
    Stop-Lab 'this script must run in an elevated PowerShell (Run as administrator); nothing was changed' 1
  }

  # Folders first, so that the log has somewhere to go.
  foreach ($d in @($LabRoot, $WorkDir, $ReportsDir)) {
    if (-not (Test-Path -LiteralPath $d)) { $null = New-Item -ItemType Directory -Path $d -Force }
  }
  [System.IO.File]::WriteAllText($script:LogFile, '')
  $script:LogReady = $true
  foreach ($l in $script:LogLines) { [System.IO.File]::AppendAllText($script:LogFile, $l + "`r`n") }

  # 1. Checks ------------------------------------------------------------------
  Write-Log 'step 1/5: checking the machine'
  $checks = @(Test-Machine)
  foreach ($c in $checks) {
    if ($c.ok) { Write-Log ('check {0}: ok - {1}' -f $c.name, $c.detail) 'OK' }
    else { Write-Log ('check {0}: FAILED - {1}' -f $c.name, $c.detail) 'ERROR' }
  }
  if (@($checks | Where-Object { -not $_.ok }).Count -gt 0) {
    Stop-Lab 'the machine does not meet the requirements; nothing was installed' 1
  }

  # 2. Packages ----------------------------------------------------------------
  Write-Log 'step 2/5: packages'
  Update-SessionPath
  $missing = @()
  foreach ($p in $Packages) {
    $ver = Get-InstalledVersion $p.Id
    if ($ver) {
      Write-Log ('{0}: already present ({1}), skipped' -f $p.Label, $ver) 'OK'
      $pkgState += [pscustomobject]@{ id = $p.Id; status = 'present'; version = $ver }
    } else {
      $missing += $p
    }
  }

  if ($missing.Count -gt 0) {
    $script:WingetPath = Find-Tool 'winget.exe' @("$env:LOCALAPPDATA\Microsoft\WindowsApps\winget.exe")
    if (-not $script:WingetPath) {
      Stop-Lab 'winget was not found. Update "App Installer" from the Microsoft Store, open a new elevated PowerShell and run this script again' 2
    }

    # Verify every id that is about to be installed before installing anything.
    foreach ($p in $missing) {
      $r = Invoke-Capture $script:WingetPath @('show', '--id', $p.Id, '--exact', '--source', 'winget', '--accept-source-agreements') 180
      if ($r.Code -ne 0) {
        Stop-Lab ('winget show could not resolve the package id {0} (exit code {1}): check the network and the winget source, or the id has changed. Nothing was installed' -f $p.Id, $r.Code) 2
      }
      Write-Log ('winget show: {0} is a valid id' -f $p.Id)
    }

    foreach ($p in $missing) {
      Write-Log ('installing {0} ({1}); this can take several minutes' -f $p.Label, $p.Id)
      $code = Install-LabPackage $p
      # 3010 = restart required (installer); the other two are winget's own restart codes.
      $rebootCodes = @(3010, -1978334967, -1978334966)
      if ($rebootCodes -contains $code) { $script:RebootRequired = $true }
      Update-SessionPath
      $ver = Get-InstalledVersion $p.Id
      if ($ver) {
        Write-Log ('{0}: installed ({1}), installer exit code 0x{2:X8}' -f $p.Label, $ver, $code) 'OK'
        $pkgState += [pscustomobject]@{ id = $p.Id; status = 'installed'; version = $ver }
      } else {
        Write-Log ('{0}: NOT detected after the install, installer exit code 0x{1:X8}' -f $p.Label, $code) 'ERROR'
        $pkgState += [pscustomobject]@{ id = $p.Id; status = 'failed'; version = $null }
      }
    }
  }

  $sdkFound = Get-WindowsSdkFound
  if ($sdkFound) { Write-Log 'Windows SDK: found' 'OK' } else { Write-Log 'Windows SDK: not found (a restart may be needed after the Build Tools install)' 'WARN' }

  # 3. Folders (created above) -------------------------------------------------
  Write-Log ('step 3/5: folders {0}, {1}, {2}' -f $LabRoot, $WorkDir, $ReportsDir)

  # 4. Repository --------------------------------------------------------------
  if (@($pkgState | Where-Object { $_.status -eq 'failed' }).Count -gt 0) {
    Write-Log 'step 4/5: skipped (an install failed)' 'WARN'
  } else {
    Write-Log 'step 4/5: repository'
    $repoSha = Sync-Repository
    Write-Log ('repository ready at commit {0}' -f $repoSha) 'OK'
  }

  # 5. HEVC Video Extensions: report only, never install -----------------------
  # Same definition as hw-inventory.ps1: "installed" means registered for the current
  # user, which is what the application process sees. Provisioned (system image) and
  # registered for any user are reported next to it, with their own names.
  try {
    $hevcExtensions = Get-HevcExtensionStatus
  } catch { $hevcExtensions = $null }

  if (@($pkgState | Where-Object { $_.status -eq 'failed' }).Count -gt 0) {
    $script:ExitCode = 2
  }
} catch {
  Write-Log ('stopped: {0}' -f $_.Exception.Message) 'ERROR'
  if ($script:ExitCode -eq 0) { $script:ExitCode = 2 }
}

# ---------------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------------

if ($script:LogReady) {
  $ready = ($script:ExitCode -eq 0)
  $summary = [ordered]@{
    tool                         = [ordered]@{ name = 'hwlab-setup'; report_version = 1 }
    generated_utc                = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    ready                        = $ready
    exit_code                    = $script:ExitCode
    reboot_required              = $script:RebootRequired
    checks                       = @($checks | ForEach-Object { [ordered]@{ name = $_.name; ok = [bool]$_.ok; detail = $_.detail } })
    packages                     = @($pkgState | ForEach-Object { [ordered]@{ id = $_.id; status = $_.status; version = $_.version } })
    windows_sdk_found            = $sdkFound
    repository                   = [ordered]@{ url = $RepoUrl; path = $RepoDir; branch = $Branch; commit = $repoSha }
    hevc_video_extensions        = $hevcExtensions
    hevc_video_extensions_note   = 'never installed by this kit, on purpose; installed means registered for the current user (what the application sees)'
  }
  try {
    $json = $summary | ConvertTo-Json -Depth 6
    [System.IO.File]::WriteAllText($SummaryFile, (Protect-Log $json), (New-Object System.Text.UTF8Encoding($false)))
  } catch { }

  Write-Host ''
  Write-Host '----- hwlab setup summary -----'
  foreach ($c in $checks) { Write-Host ('  check   {0,-8} {1}' -f $c.name, $(if ($c.ok) { 'ok' } else { 'FAILED' })) }
  foreach ($s in $pkgState) { Write-Host ('  package {0,-40} {1} {2}' -f $s.id, $s.status, $s.version) }
  if ($null -ne $sdkFound) { Write-Host ('  Windows SDK found        : {0}' -f $sdkFound) }
  if ($repoSha) { Write-Host ('  repository               : {0} at {1}' -f $RepoDir, $repoSha) }
  if ($null -ne $hevcExtensions) {
    function Format-Tri($v) { if ($null -eq $v) { return 'unknown' } elseif ($v) { return 'yes' } else { return 'no' } }
    Write-Host ('  HEVC Video Extensions    : registered for the current user {0}; provisioned in the system image {1}; registered for any user {2} (not installed by this kit, on purpose)' -f (Format-Tri $hevcExtensions.registered_current_user), (Format-Tri $hevcExtensions.provisioned_system_image), (Format-Tri $hevcExtensions.registered_any_user))
  }
  if ($script:RebootRequired) { Write-Host '  A restart is needed before the Build Tools are fully usable.' -ForegroundColor Yellow }
  if ($ready) { Write-Host '  RESULT: ready' -ForegroundColor Green } else { Write-Host ('  RESULT: not ready (exit code {0}), see {1}' -f $script:ExitCode, $script:LogFile) -ForegroundColor Red }
  Write-Host ('  summary file: {0}' -f $SummaryFile)
}

exit $script:ExitCode
