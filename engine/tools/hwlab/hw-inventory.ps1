<#
.SYNOPSIS
  Writes a hardware inventory of this machine as JSON, without personal data.

.DESCRIPTION
  Part of the hwlab kit (engine/tools/hwlab). Windows PowerShell 5.1 compatible, no
  administrator rights needed, nothing is installed or changed.

  Reports: OS caption/version/build, CPU, RAM, GPUs (name, driver, memory, PNP vendor
  id only), audio render and capture endpoints (friendly names, default flags), the
  Bluetooth radio and the paired audio devices, cameras, and the HEVC Video Extensions
  (registered for the current user - the value that counts -, provisioned in the system
  image, registered for any user; the last two need elevation and are null without it;
  every registered package with HEVC in its name, known or not; and the media codecs that
  those packages declare in their manifest).

  Never read or written: serial numbers, machine name, user name, MAC or IP addresses,
  paths. Text that comes from the system (device friendly names) is scrubbed for the
  account name, the machine name, MAC and IP patterns and possessive owner names, and the
  finished JSON is checked once more before it is written.

.PARAMETER OutDir
  Folder for inventory-<date>.json. Default C:\hwlab\reports.
#>
[CmdletBinding()]
param(
  [string]$OutDir = 'C:\hwlab\reports'
)

Set-StrictMode -Version 1.0
$ErrorActionPreference = 'Stop'

$script:Warnings = New-Object System.Collections.Generic.List[string]

# ---------------------------------------------------------------------------------
# Scrubbing of text that comes from the system (shared with the other hwlab scripts)
# ---------------------------------------------------------------------------------

. (Join-Path $PSScriptRoot 'hwlab-common.ps1')
Initialize-Scrubbing

function Invoke-Section([string]$Name, [scriptblock]$Body) {
  try {
    & $Body
  } catch {
    $script:Warnings.Add("section '$Name' failed ($($_.Exception.GetType().Name))")
    return $null
  }
}

# ---------------------------------------------------------------------------------
# Operating system, CPU, RAM
# ---------------------------------------------------------------------------------

function Get-OsInfo {
  $os = Get-CimInstance -ClassName Win32_OperatingSystem
  $cv = Get-ItemProperty -Path 'HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion'
  $display = $null
  if ($cv.PSObject.Properties['DisplayVersion']) { $display = [string]$cv.DisplayVersion }
  elseif ($cv.PSObject.Properties['ReleaseId']) { $display = [string]$cv.ReleaseId }
  $ubr = $null
  if ($cv.PSObject.Properties['UBR']) { $ubr = [int]$cv.UBR }
  # OSArchitecture is localized; the processor architecture code is not.
  $arch = 'other'
  switch ([int](@(Get-CimInstance -ClassName Win32_Processor)[0].Architecture)) {
    9  { $arch = 'x64' }
    12 { $arch = 'arm64' }
    0  { $arch = 'x86' }
  }
  return [ordered]@{
    caption         = ([string]$os.Caption).Trim()
    version         = [string]$os.Version
    build           = [int]$os.BuildNumber
    build_revision  = $ubr
    display_version = $display
    architecture    = $arch
  }
}

function Get-CpuInfo {
  $cpus = @(Get-CimInstance -ClassName Win32_Processor)
  $cores = 0
  $threads = 0
  foreach ($c in $cpus) { $cores += [int]$c.NumberOfCores; $threads += [int]$c.NumberOfLogicalProcessors }
  return [ordered]@{
    name    = (@($cpus | ForEach-Object { ([string]$_.Name) -replace '\s+', ' ' }) -join ' + ').Trim()
    sockets = $cpus.Count
    cores   = $cores
    threads = $threads
  }
}

function Get-RamGb {
  $cs = Get-CimInstance -ClassName Win32_ComputerSystem
  return [math]::Round(([double]$cs.TotalPhysicalMemory) / 1GB, 1)
}

# ---------------------------------------------------------------------------------
# GPUs
# ---------------------------------------------------------------------------------

function Get-VendorName([string]$VendorId) {
  switch ($VendorId) {
    '10DE' { return 'NVIDIA' }
    '1002' { return 'AMD' }
    '1022' { return 'AMD' }
    '8086' { return 'Intel' }
    '1414' { return 'Microsoft' }
    '5143' { return 'Qualcomm' }
    default { return $null }
  }
}

# Dedicated video memory from the display class key. WMI AdapterRAM is a 32 bit value
# and tops out at 4 GiB, which is wrong for a modern GPU.
function Get-RegistryVramMap {
  $map = @{}
  $classKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\Class\{4d36e968-e325-11ce-bfc1-08002be10318}'
  foreach ($k in @(Get-ChildItem -Path $classKey -ErrorAction SilentlyContinue)) {
    if ($k.PSChildName -notmatch '^\d{4}$') { continue }
    try {
      $p = Get-ItemProperty -Path $k.PSPath -ErrorAction Stop
      if (-not $p.PSObject.Properties['DriverDesc']) { continue }
      if (-not $p.PSObject.Properties['HardwareInformation.qwMemorySize']) { continue }
      $raw = $p.'HardwareInformation.qwMemorySize'
      if ($raw -is [byte[]]) {
        $bytes = New-Object byte[] 8
        [Array]::Copy($raw, $bytes, [Math]::Min(8, $raw.Length))
        $raw = [BitConverter]::ToInt64($bytes, 0)
      }
      $map[[string]$p.DriverDesc] = [double]$raw
    } catch { }
  }
  return $map
}

function Get-GpuInfo {
  $vram = Get-RegistryVramMap
  $list = @()
  foreach ($g in @(Get-CimInstance -ClassName Win32_VideoController)) {
    $vendorId = $null
    if ([string]$g.PNPDeviceID -match 'VEN_([0-9A-Fa-f]{4})') { $vendorId = $Matches[1].ToUpper() }
    $adapterGb = $null
    if ($g.AdapterRAM -ne $null -and [double]$g.AdapterRAM -gt 0) { $adapterGb = [math]::Round(([double]$g.AdapterRAM) / 1GB, 2) }
    $dedicatedGb = $null
    if ($vram.ContainsKey([string]$g.Name)) { $dedicatedGb = [math]::Round($vram[[string]$g.Name] / 1GB, 2) }
    $list += [ordered]@{
      name                  = Protect-Text ([string]$g.Name)
      driver_version        = [string]$g.DriverVersion
      adapter_ram_gb_wmi    = $adapterGb
      dedicated_vram_gb     = $dedicatedGb
      pnp_vendor_id         = $vendorId
      vendor                = (Get-VendorName $vendorId)
    }
  }
  return ,$list
}

# ---------------------------------------------------------------------------------
# Audio endpoints through Core Audio (MMDevice API)
# ---------------------------------------------------------------------------------

$script:AudioCode = @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class HwLabAudioDevice
{
    public string Name;
    public bool IsDefault;
    public bool IsDefaultCommunications;
}

public static class HwLabAudio
{
    [StructLayout(LayoutKind.Sequential)]
    struct PROPERTYKEY { public Guid fmtid; public uint pid; }

    [StructLayout(LayoutKind.Sequential)]
    struct PROPVARIANT { public ushort vt; public ushort r1; public ushort r2; public ushort r3; public IntPtr p; public IntPtr p2; }

    [ComImport, Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    class MMDeviceEnumeratorCom { }

    [ComImport, Guid("A95664D2-9614-4F35-A746-DE8DB63617E6"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceEnumerator
    {
        [PreserveSig] int EnumAudioEndpoints(int dataFlow, uint stateMask, out IMMDeviceCollection devices);
        [PreserveSig] int GetDefaultAudioEndpoint(int dataFlow, int role, out IMMDevice device);
        [PreserveSig] int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id, out IMMDevice device);
        [PreserveSig] int RegisterEndpointNotificationCallback(IntPtr client);
        [PreserveSig] int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport, Guid("0BD7A1BE-7A1A-44DB-8397-CC5392387B5E"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDeviceCollection
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int Item(uint index, out IMMDevice device);
    }

    [ComImport, Guid("D666063F-1587-4E43-81F1-B948E807363F"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IMMDevice
    {
        [PreserveSig] int Activate(ref Guid iid, uint clsCtx, IntPtr activationParams, [MarshalAs(UnmanagedType.IUnknown)] out object iface);
        [PreserveSig] int OpenPropertyStore(uint stgmAccess, out IPropertyStore props);
        [PreserveSig] int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig] int GetState(out uint state);
    }

    [ComImport, Guid("886D8EEB-8CF2-4446-8D02-CDBA1DBDCF99"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    interface IPropertyStore
    {
        [PreserveSig] int GetCount(out uint count);
        [PreserveSig] int GetAt(uint index, out PROPERTYKEY key);
        [PreserveSig] int GetValue(ref PROPERTYKEY key, out PROPVARIANT value);
        [PreserveSig] int SetValue(ref PROPERTYKEY key, ref PROPVARIANT value);
        [PreserveSig] int Commit();
    }

    [DllImport("ole32.dll")]
    static extern int PropVariantClear(ref PROPVARIANT pv);

    static string IdOf(IMMDevice dev)
    {
        string id;
        if (dev.GetId(out id) != 0) return null;
        return id;
    }

    static string DefaultId(IMMDeviceEnumerator en, int flow, int role)
    {
        IMMDevice dev;
        if (en.GetDefaultAudioEndpoint(flow, role, out dev) != 0 || dev == null) return null;
        string id = IdOf(dev);
        Marshal.ReleaseComObject(dev);
        return id;
    }

    static string FriendlyName(IMMDevice dev)
    {
        IPropertyStore store;
        if (dev.OpenPropertyStore(0, out store) != 0 || store == null) return null;
        try
        {
            PROPERTYKEY key = new PROPERTYKEY();
            key.fmtid = new Guid("A45C254E-DF1C-4EFD-8020-67D146A850E0");
            key.pid = 14;
            PROPVARIANT pv;
            if (store.GetValue(ref key, out pv) != 0) return null;
            string name = null;
            if (pv.vt == 31 && pv.p != IntPtr.Zero) name = Marshal.PtrToStringUni(pv.p);
            PropVariantClear(ref pv);
            return name;
        }
        finally { Marshal.ReleaseComObject(store); }
    }

    // dataFlow: 0 = render, 1 = capture. Only active endpoints.
    public static HwLabAudioDevice[] List(int dataFlow)
    {
        var result = new List<HwLabAudioDevice>();
        IMMDeviceEnumerator en = (IMMDeviceEnumerator)(new MMDeviceEnumeratorCom());
        try
        {
            string defConsole = DefaultId(en, dataFlow, 0);
            string defComm = DefaultId(en, dataFlow, 2);
            IMMDeviceCollection col;
            if (en.EnumAudioEndpoints(dataFlow, 1, out col) != 0 || col == null) return result.ToArray();
            uint count;
            col.GetCount(out count);
            for (uint i = 0; i < count; i++)
            {
                IMMDevice dev;
                if (col.Item(i, out dev) != 0 || dev == null) continue;
                string id = IdOf(dev);
                var d = new HwLabAudioDevice();
                d.Name = FriendlyName(dev);
                d.IsDefault = id != null && id == defConsole;
                d.IsDefaultCommunications = id != null && id == defComm;
                result.Add(d);
                Marshal.ReleaseComObject(dev);
            }
            Marshal.ReleaseComObject(col);
        }
        finally { Marshal.ReleaseComObject(en); }
        return result.ToArray();
    }
}
'@

function Get-AudioInfo {
  $coreAudio = $true
  try {
    if (-not ([System.Management.Automation.PSTypeName]'HwLabAudio').Type) {
      Add-Type -TypeDefinition $script:AudioCode -Language CSharp
    }
  } catch {
    $coreAudio = $false
    $script:Warnings.Add("core audio interop unavailable ($($_.Exception.GetType().Name)), falling back to PnP endpoints")
  }

  if ($coreAudio) {
    $render = @()
    foreach ($d in [HwLabAudio]::List(0)) {
      $render += [ordered]@{
        name                       = Protect-Text ([string]$d.Name)
        default                    = [bool]$d.IsDefault
        default_communications     = [bool]$d.IsDefaultCommunications
      }
    }
    $capture = @()
    foreach ($d in [HwLabAudio]::List(1)) {
      $capture += [ordered]@{
        name                       = Protect-Text ([string]$d.Name)
        default                    = [bool]$d.IsDefault
        default_communications     = [bool]$d.IsDefaultCommunications
      }
    }
    return [ordered]@{ source = 'core_audio'; render = $render; capture = $capture }
  }

  # Fallback: endpoints from the PnP tree. Render endpoints have {0.0.0. in the
  # instance id and capture endpoints {0.0.1. ; the id itself is never reported.
  $render = @()
  $capture = @()
  foreach ($e in @(Get-PnpDevice -Class AudioEndpoint -PresentOnly -ErrorAction Stop)) {
    $item = [ordered]@{ name = Protect-Text ([string]$e.FriendlyName); default = $null; default_communications = $null }
    if ([string]$e.InstanceId -match '\{0\.0\.1\.') { $capture += $item } else { $render += $item }
  }
  return [ordered]@{ source = 'pnp'; render = $render; capture = $capture }
}

# ---------------------------------------------------------------------------------
# Bluetooth
# ---------------------------------------------------------------------------------

# State of the radio as the Windows Bluetooth switch sees it (On, Off, Disabled).
# Windows Runtime API, best effort: null when it cannot be read.
function Get-BluetoothRadioState {
  try {
    Add-Type -AssemblyName System.Runtime.WindowsRuntime
    $null = [Windows.Devices.Radios.Radio,Windows.System.Devices,ContentType=WindowsRuntime]
    $asTask = [System.WindowsRuntimeSystemExtensions].GetMethods() | Where-Object {
      $_.Name -eq 'AsTask' -and $_.GetParameters().Count -eq 1 -and $_.GetParameters()[0].ParameterType.Name -eq 'IAsyncOperation`1'
    } | Select-Object -First 1
    $listType = [System.Collections.Generic.IReadOnlyList[Windows.Devices.Radios.Radio]]
    $task = $asTask.MakeGenericMethod($listType).Invoke($null, @([Windows.Devices.Radios.Radio]::GetRadiosAsync()))
    if (-not $task.Wait(10000)) { return $null }
    foreach ($r in $task.Result) {
      if ([string]$r.Kind -eq 'Bluetooth') { return [string]$r.State }
    }
    return 'NoRadio'
  } catch {
    return $null
  }
}

function Get-BluetoothInfo {
  $all = @(Get-PnpDevice -Class Bluetooth -ErrorAction SilentlyContinue)
  # The radio is the adapter itself: its instance id is not under the BTH* enumerators.
  $radios = @($all | Where-Object { $_.InstanceId -notmatch '^BTH' -and $_.Status -ne 'Unknown' })
  $radioNames = @($radios | ForEach-Object { Protect-Text ([string]$_.FriendlyName) })
  $radioEnabled = $false
  foreach ($r in $radios) { if ([string]$r.Status -eq 'OK') { $radioEnabled = $true } }

  # Paired devices: BTHENUM\DEV_<address> (classic) and BTHLE\DEV_<address> (LE). The
  # address is only used here to match the profile service nodes and is never reported.
  $audioPattern = '^BTHENUM\\\{0000(110[abcde]|111[ef]|1108)-'
  $paired = @()
  $otherCount = 0
  foreach ($d in ($all | Where-Object { $_.InstanceId -match '^BTH(ENUM|LE)\\DEV_([0-9A-Fa-f]{12})\\' })) {
    $null = ([string]$d.InstanceId) -match '^BTH(ENUM|LE)\\DEV_([0-9A-Fa-f]{12})\\'
    $addr = $Matches[2]
    $isAudio = $false
    foreach ($s in $all) {
      if ($s.InstanceId -match $audioPattern -and $s.InstanceId -match $addr) { $isAudio = $true; break }
    }
    if (-not $isAudio) { $otherCount++; continue }
    $connected = $null
    try {
      $prop = Get-PnpDeviceProperty -InstanceId $d.InstanceId -KeyName '{83DA6326-97A6-4088-9453-A1923F573B29} 15' -ErrorAction Stop
      if ($prop -and $prop.Data -ne $null) { $connected = [bool]$prop.Data }
    } catch { }
    $paired += [ordered]@{
      name      = Protect-Text ([string]$d.FriendlyName)
      connected = $connected
    }
  }

  return [ordered]@{
    radio_present        = ($radios.Count -gt 0)
    radio_device_enabled = $radioEnabled
    radio_state          = (Get-BluetoothRadioState)
    radio_names          = $radioNames
    paired_audio_devices = $paired
    paired_other_count   = $otherCount
    paired_audio_note    = 'classic profiles (A2DP, HFP, AVRCP) are recognized as audio; LE Audio devices are counted under paired_other_count'
  }
}

# ---------------------------------------------------------------------------------
# Cameras, HEVC Video Extensions
# ---------------------------------------------------------------------------------

function Get-CameraInfo {
  $names = New-Object System.Collections.Generic.List[string]
  foreach ($d in @(Get-PnpDevice -Class Camera -PresentOnly -ErrorAction SilentlyContinue)) {
    $names.Add((Protect-Text ([string]$d.FriendlyName)))
  }
  # Older drivers register webcams in the Image class (next to scanners).
  foreach ($d in @(Get-PnpDevice -Class Image -PresentOnly -ErrorAction SilentlyContinue)) {
    if ([string]$d.FriendlyName -match '(?i)cam|video|capture') { $names.Add((Protect-Text ([string]$d.FriendlyName))) }
  }
  return ,@($names | Select-Object -Unique)
}

# One definition shared with setup-hwlab.ps1 (see hwlab-common.ps1): "installed" means
# registered for the current user, which is what the application process sees. The
# provisioned (system image) and registered-for-any-user facts need elevation and are
# reported next to it under their own names; null means "could not be read".
function Get-HevcExtensionInfo {
  return (Get-HevcExtensionStatus)
}

# What the registered packages with HEVC in their name declare to Media Foundation. A registered
# package does not have to declare a video decoder for HEVC, and Media Foundation only enumerates
# what a package declares (a windows.mediaCodec extension in its manifest: MediaCodec elements with
# a Category of videoDecoder or videoEncoder and the media subtypes they take or produce; per a
# third-party article, not per Microsoft documentation, see engine/tools/hevc-probe/README.md).

# Reads the MediaCodec declarations of one package manifest (an XmlDocument). Pure: CI runs it on
# fixture manifests. Duplicates (a package lists the same codec once per architecture) are folded.
function Get-MediaCodecsFromManifest($Manifest) {
  $hevc = '43564548-0000-0010-8000-00aa00389b71'     # MFVideoFormat_HEVC
  $hevcEs = '53564548-0000-0010-8000-00aa00389b71'   # MFVideoFormat_HEVC_ES
  $nodes = @($Manifest.SelectNodes("//*[local-name()='Extension'][@Category='windows.mediaCodec']/*[local-name()='MediaCodec']"))
  $seen = @{}
  $codecs = @()
  $dec = $false
  $enc = $false
  foreach ($n in $nodes) {
    $attr = @{}
    foreach ($a in $n.Attributes) { $attr[$a.LocalName] = [string]$a.Value }
    $cat = [string]$attr['Category']
    $key = ('{0}|{1}|{2}' -f $attr['DisplayName'], $cat, $attr['ActivatableClassId'])
    if ($seen.ContainsKey($key)) { continue }
    $seen[$key] = $true
    $inHevc = $false
    foreach ($t in @($n.SelectNodes(".//*[local-name()='InputType']"))) {
      $sub = ([string]$t.GetAttribute('SubType')).Trim('{', '}').ToLowerInvariant()
      if ($sub -eq $hevc -or $sub -eq $hevcEs) { $inHevc = $true }
    }
    $outHevc = $false
    foreach ($t in @($n.SelectNodes(".//*[local-name()='OutputType']"))) {
      $sub = ([string]$t.GetAttribute('SubType')).Trim('{', '}').ToLowerInvariant()
      if ($sub -eq $hevc -or $sub -eq $hevcEs) { $outHevc = $true }
    }
    if ($cat -eq 'videoDecoder' -and $inHevc) { $dec = $true }
    if ($cat -eq 'videoEncoder' -and $outHevc) { $enc = $true }
    $codecs += [ordered]@{
      display_name         = $attr['DisplayName']
      category             = $cat
      activatable_class_id = $attr['ActivatableClassId']
      takes_hevc_input     = $inHevc
      produces_hevc_output = $outHevc
    }
  }
  return [ordered]@{
    media_codecs                = @($codecs)
    declares_hevc_video_decoder = $dec
    declares_hevc_video_encoder = $enc
  }
}

# The manifest of each registered package whose name matches the pattern, for the current user
# (reading it needs no elevation on the machines tried so far). Only names, categories and
# subtype flags are reported, no paths. $null in a field means "could not be read", never "no".
function Get-HevcPackageMediaCodecs([string]$NamePattern = '*HEVC*') {
  $out = @()
  foreach ($p in @(Get-AppxPackage -Name $NamePattern -ErrorAction Stop)) {
    $entry = [ordered]@{
      name                         = [string]$p.Name
      version                      = [string]$p.Version
      family                       = [string]$p.PackageFamilyName
      status                       = [string]$p.Status
      manifest_read                = $false
      error_type                   = $null
      media_codecs                 = @()
      declares_hevc_video_decoder  = $null
      declares_hevc_video_encoder  = $null
    }
    try {
      $xml = Get-AppxPackageManifest -Package $p.PackageFullName -ErrorAction Stop
      $r = Get-MediaCodecsFromManifest $xml
      $entry.manifest_read = $true
      $entry.media_codecs = @($r.media_codecs)
      $entry.declares_hevc_video_decoder = $r.declares_hevc_video_decoder
      $entry.declares_hevc_video_encoder = $r.declares_hevc_video_encoder
    } catch {
      $entry.error_type = $_.Exception.GetType().Name
    }
    $out += $entry
  }
  return ,@($out)
}

# ---------------------------------------------------------------------------------
# Assemble, check, write
# ---------------------------------------------------------------------------------

Write-Host 'hw-inventory: collecting...'

$osInfo   = Invoke-Section 'os'        { Get-OsInfo }
$cpuInfo  = Invoke-Section 'cpu'       { Get-CpuInfo }
$ramGb    = Invoke-Section 'ram'       { Get-RamGb }
$gpus     = Invoke-Section 'gpu'       { Get-GpuInfo }
$audio    = Invoke-Section 'audio'     { Get-AudioInfo }
$bluetooth = Invoke-Section 'bluetooth' { Get-BluetoothInfo }
$cameras  = Invoke-Section 'cameras'   { Get-CameraInfo }
$hevcExt  = Invoke-Section 'hevc_ext'  { Get-HevcExtensionInfo }
$hevcPkgCodecs = Invoke-Section 'hevc_pkg_codecs' { Get-HevcPackageMediaCodecs }

# An empty list must stay an empty JSON array (a sub-expression would turn it into nothing).
$hevcPkgOut = $null
if ($null -ne $hevcPkgCodecs) { $hevcPkgOut = @($hevcPkgCodecs) }

$report = [ordered]@{
  tool                  = [ordered]@{ name = 'hwlab-inventory'; report_version = 1 }
  generated_utc         = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
  os                    = $osInfo
  cpu                   = $cpuInfo
  ram_gb                = $ramGb
  gpus                  = @($gpus)
  audio                 = $audio
  bluetooth             = $bluetooth
  cameras               = @($cameras)
  hevc_video_extensions = $hevcExt
  hevc_package_media_codecs = $hevcPkgOut
  warnings              = @($script:Warnings)
}

$json = $report | ConvertTo-Json -Depth 8

# Last line of defence: nothing identifying may be in the text that is written.
$protected = Protect-Json $json
$json = $protected.Text
$leaks = $protected.Leaks
if ($leaks -gt 0) {
  Write-Warning "identifying text was found in the report and replaced by placeholders ($leaks pattern(s)); read the report before sharing it."
}

if (-not (Test-Path -LiteralPath $OutDir)) { $null = New-Item -ItemType Directory -Path $OutDir -Force }
$outFile = Join-Path -Path $OutDir -ChildPath ('inventory-' + (Get-Date -Format 'yyyy-MM-dd') + '.json')
[System.IO.File]::WriteAllText($outFile, $json, (New-Object System.Text.UTF8Encoding($false)))

# Short summary for the console (and for the session that relays it).
Write-Host ''
Write-Host 'hw-inventory summary'
if ($osInfo)   { Write-Host ('  OS        : {0} (version {1}, build {2})' -f $osInfo.caption, $osInfo.version, $osInfo.build) }
if ($cpuInfo)  { Write-Host ('  CPU       : {0} ({1} cores, {2} threads)' -f $cpuInfo.name, $cpuInfo.cores, $cpuInfo.threads) }
if ($ramGb)    { Write-Host ('  RAM       : {0} GB' -f $ramGb) }
foreach ($g in @($gpus)) { Write-Host ('  GPU       : {0} (driver {1}, vendor id {2})' -f $g.name, $g.driver_version, $g.pnp_vendor_id) }
if ($audio)    { Write-Host ('  Audio     : {0} render, {1} capture endpoint(s), source {2}' -f @($audio.render).Count, @($audio.capture).Count, $audio.source) }
if ($bluetooth) { Write-Host ('  Bluetooth : radio present {0}, enabled {1}, state {2}, paired audio devices {3}' -f $bluetooth.radio_present, $bluetooth.radio_device_enabled, $bluetooth.radio_state, @($bluetooth.paired_audio_devices).Count) }
Write-Host ('  Cameras   : {0}' -f @($cameras).Count)
if ($hevcExt)  {
  function Format-Tri($v) { if ($null -eq $v) { return 'unknown' } elseif ($v) { return 'yes' } else { return 'no' } }
  Write-Host ('  HEVC Video Extensions: registered for the current user {0}; provisioned in the system image {1}; registered for any user {2}' -f (Format-Tri $hevcExt.registered_current_user), (Format-Tri $hevcExt.provisioned_system_image), (Format-Tri $hevcExt.registered_any_user))
  foreach ($np in @($hevcExt.hevc_named_packages_current_user)) {
    if ($np) { Write-Host ('    package with HEVC in its name: {0} {1} (family {2}, known family {3})' -f $np.name, $np.version, $np.family, (Format-Tri $np.in_known_family_list)) }
  }
}
foreach ($pc in @($hevcPkgCodecs)) {
  if ($pc) { Write-Host ('  package {0}: manifest read {1}; declares an HEVC video decoder {2}, an HEVC video encoder {3}' -f $pc.name, $pc.manifest_read, $pc.declares_hevc_video_decoder, $pc.declares_hevc_video_encoder) }
}
foreach ($w in $script:Warnings) { Write-Host ('  warning   : {0}' -f $w) }
Write-Host ('  report    : {0}' -f $outFile)
