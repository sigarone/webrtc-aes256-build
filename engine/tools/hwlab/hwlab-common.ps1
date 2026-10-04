<#
.SYNOPSIS
  Helpers shared by the scripts of the hwlab kit (engine/tools/hwlab). Dot-source it.

.DESCRIPTION
  Not meant to be run. Windows PowerShell 5.1 compatible, ASCII only, no side effects
  except the regular expressions and the empty rule list it defines in the scope of the
  script that dot-sources it:

      . (Join-Path $PSScriptRoot 'hwlab-common.ps1')
      Initialize-Scrubbing

  Contents
    - Scrubbing of text that comes from the system (account name, machine name, MAC and
      IP patterns, possessive owner names): Initialize-Scrubbing, Protect-Text,
      Protect-Json. hw-inventory.ps1 and engine-local-build.ps1 use the same rules, so
      a report or a log never carries a name that identifies the lab owner.
    - Test-Elevated.
    - Get-HevcExtensionStatus: the one definition of "the HEVC Video Extensions are
      installed" (the region below is copied word for word into setup-hwlab.ps1, which
      has to work on its own before the repository is cloned; CI compares the copies).
#>

# ---------------------------------------------------------------------------------
# Scrubbing of text that comes from the system
# ---------------------------------------------------------------------------------

$script:ScrubRules = New-Object System.Collections.Generic.List[object]

function Add-ScrubLiteral([string]$Value, [string]$Replacement) {
  if ([string]::IsNullOrWhiteSpace($Value)) { return }
  $v = $Value.Trim()
  if ($v.Length -lt 2) { return }
  $pattern = '(?<![\p{L}\p{N}])' + [regex]::Escape($v) + '(?![\p{L}\p{N}])'
  $script:ScrubRules.Add(@{
      Literal     = $v
      Regex       = (New-Object System.Text.RegularExpressions.Regex($pattern, 'IgnoreCase'))
      Replacement = $Replacement
    })
}

$script:MacRegex = New-Object System.Text.RegularExpressions.Regex('(?i)(?<![0-9a-f])(?:[0-9a-f]{2}[:-]){5}[0-9a-f]{2}(?![0-9a-f])')
$script:MacBareRegex = New-Object System.Text.RegularExpressions.Regex('(?i)(?<![0-9a-f])[0-9a-f]{12}(?![0-9a-f])')
$script:Ipv4Regex = New-Object System.Text.RegularExpressions.Regex('(?<![\d.])(?:25[0-5]|2[0-4]\d|1?\d?\d)(?:\.(?:25[0-5]|2[0-4]\d|1?\d?\d)){3}(?![\d.])')
# A possessive: a name followed by 's, with the ASCII apostrophe or the typographic one
# (U+2019, written as a regex escape that is assembled here to keep this file ASCII).
$script:ApostropheClass = '(?:''|' + '\' + 'u2019)'
$script:PossessiveRegex = New-Object System.Text.RegularExpressions.Regex('(?<![\p{L}\p{N}])[\p{L}][\p{L}\p{N}\-]{1,30}(?=' + $script:ApostropheClass + 's(?![\p{L}\p{N}]))')

# Builds the literal rules: the account name, the machine name, the domain, the profile
# folder name and the full name of the local account. Best effort: the pattern rules
# (MAC, IPv4, possessive) apply even when this cannot read the account.
function Initialize-Scrubbing {
  try {
    Add-ScrubLiteral $env:USERNAME '<user>'
    Add-ScrubLiteral $env:COMPUTERNAME '<machine>'
    Add-ScrubLiteral $env:USERDOMAIN '<machine>'
    if ($env:USERPROFILE) { Add-ScrubLiteral (Split-Path -Path $env:USERPROFILE -Leaf) '<user>' }
    $safeUser = $env:USERNAME -replace "'", "''"
    $acct = Get-CimInstance -ClassName Win32_UserAccount -Filter "LocalAccount=True AND Name='$safeUser'" -ErrorAction Stop
    foreach ($a in @($acct)) {
      if ($a -and $a.FullName) {
        Add-ScrubLiteral $a.FullName '<user>'
        foreach ($w in ($a.FullName -split '\s+')) { if ($w.Length -ge 3) { Add-ScrubLiteral $w '<user>' } }
      }
    }
  } catch {
    # Best effort: the possessive and pattern rules still apply.
  }
}

# Use for names that the system supplies (friendly names, log lines). Not for version
# strings, which look like IPv4 addresses.
function Protect-Text([string]$Text) {
  if ([string]::IsNullOrEmpty($Text)) { return $Text }
  $s = $Text
  foreach ($r in $script:ScrubRules) { $s = $r.Regex.Replace($s, $r.Replacement) }
  $s = $script:MacRegex.Replace($s, '<mac>')
  $s = $script:Ipv4Regex.Replace($s, '<ip>')
  $s = $script:PossessiveRegex.Replace($s, '<owner>')
  return $s.Trim()
}

# Last line of defence for a finished report: the literal rules and the MAC patterns
# are applied to the whole text (version strings are left alone, so no IPv4 rule).
# Returns @{ Text; Leaks } where Leaks is the number of rules that changed something.
function Protect-Json([string]$Json) {
  $leaks = 0
  $out = $Json
  foreach ($r in $script:ScrubRules) {
    $before = $out
    $out = $r.Regex.Replace($out, $r.Replacement)
    if ($out -ne $before) { $leaks++ }
  }
  foreach ($rx in @($script:MacRegex, $script:MacBareRegex)) {
    $before = $out
    $out = $rx.Replace($out, '<mac>')
    if ($out -ne $before) { $leaks++ }
  }
  return @{ Text = $out; Leaks = $leaks }
}

function Test-Elevated {
  $id = [Security.Principal.WindowsIdentity]::GetCurrent()
  return ([Security.Principal.WindowsPrincipal]$id).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

# ---------------------------------------------------------------------------------
# The HEVC Video Extensions
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
function Get-HevcExtensionStatus {
  $families = @('Microsoft.HEVCVideoExtension_8wekyb3d8bbwe', 'Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe')
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
    registered_any_user              = $anyUser
    provisioned_system_image         = $prov
    elevated                         = $elevated
  }
}
#endregion hevc-extensions

# ---------------------------------------------------------------------------------
# Running a console program with a live, scrubbed log
# ---------------------------------------------------------------------------------

# Reads what a file has gained since $Position (the file is being written by another
# process, so it is opened with sharing), and advances $Position.
function Read-NewText([string]$Path, [ref]$Position) {
  if (-not (Test-Path -LiteralPath $Path)) { return '' }
  $fs = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
  try {
    $len = [int]($fs.Length - $Position.Value)
    if ($len -le 0) { return '' }
    $null = $fs.Seek($Position.Value, [System.IO.SeekOrigin]::Begin)
    $buf = New-Object byte[] $len
    $n = $fs.Read($buf, 0, $len)
    $Position.Value = $Position.Value + $n
    return [System.Text.Encoding]::UTF8.GetString($buf, 0, $n)
  } finally {
    $fs.Dispose()
  }
}

# Runs a console program without a window. Its output (stdout and stderr) is scrubbed
# line by line (Protect-Text), appended to $LogFile while the program runs, and echoed
# to the console unless -Quiet. When the time limit passes, the whole process tree is
# killed. Returns Code, Seconds, TimedOut and Tail (the last lines, scrubbed).
function Invoke-LoggedProcess {
  param(
    [Parameter(Mandatory = $true)] [string]$Exe,
    [string]$ArgumentString = '',
    [string]$LogFile = '',
    [int]$TimeoutMinutes = 30,
    [string]$WorkingDirectory = '',
    [int]$TailLines = 20,
    [switch]$Quiet
  )
  $id = [guid]::NewGuid().ToString('N')
  $outFile = Join-Path $env:TEMP ('hwlab-' + $id + '.out')
  $errFile = Join-Path $env:TEMP ('hwlab-' + $id + '.err')
  $tail = New-Object System.Collections.Generic.Queue[string]

  function Emit-Line([string]$Line) {
    $clean = Protect-Text $Line
    if ($LogFile) { [System.IO.File]::AppendAllText($LogFile, ([string]$clean) + "`r`n") }
    if (-not $Quiet) { Write-Host ('  | ' + $clean) }
    $tail.Enqueue([string]$clean)
    while ($tail.Count -gt $TailLines) { [void]$tail.Dequeue() }
  }

  function Write-Lines([string]$Text, [ref]$Pending) {
    $buf = $Pending.Value + $Text
    $parts = @($buf -split "`r?`n")
    $Pending.Value = [string]$parts[$parts.Count - 1]
    for ($i = 0; $i -lt ($parts.Count - 1); $i++) { Emit-Line ([string]$parts[$i]) }
  }

  $startArgs = @{
    FilePath               = $Exe
    NoNewWindow            = $true
    PassThru               = $true
    RedirectStandardOutput = $outFile
    RedirectStandardError  = $errFile
  }
  if ($ArgumentString) { $startArgs['ArgumentList'] = $ArgumentString }
  if ($WorkingDirectory) { $startArgs['WorkingDirectory'] = $WorkingDirectory }

  $sw = [System.Diagnostics.Stopwatch]::StartNew()
  $timedOut = $false
  $code = -1
  $posOut = [long]0
  $posErr = [long]0
  $pendOut = ''
  $pendErr = ''
  try {
    $p = Start-Process @startArgs
    # Reading the handle once makes ExitCode reliable after a redirected, windowless start.
    $null = $p.Handle
    while (-not $p.HasExited) {
      Start-Sleep -Milliseconds 1000
      Write-Lines (Read-NewText $outFile ([ref]$posOut)) ([ref]$pendOut)
      Write-Lines (Read-NewText $errFile ([ref]$posErr)) ([ref]$pendErr)
      if ($sw.Elapsed.TotalMinutes -gt $TimeoutMinutes) {
        $timedOut = $true
        try { & (Join-Path $env:SystemRoot 'System32\taskkill.exe') /PID $p.Id /T /F 2>&1 | Out-Null } catch { }
        break
      }
    }
    $null = $p.WaitForExit(30000)
    Write-Lines (Read-NewText $outFile ([ref]$posOut)) ([ref]$pendOut)
    Write-Lines (Read-NewText $errFile ([ref]$posErr)) ([ref]$pendErr)
    if ($pendOut) { Emit-Line $pendOut }
    if ($pendErr) { Emit-Line $pendErr }
    if ($timedOut) { $code = -2 } else { $code = [int]$p.ExitCode }
  } finally {
    foreach ($f in @($outFile, $errFile)) { if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue } }
  }
  return [pscustomobject]@{
    Code     = $code
    Seconds  = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    TimedOut = $timedOut
    Tail     = @($tail.ToArray())
  }
}
