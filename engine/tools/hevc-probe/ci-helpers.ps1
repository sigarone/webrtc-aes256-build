# Helpers for the checks of .github/workflows/hevc-probe.yml (PowerShell 7, dot-sourced by two
# steps of the build job). Not part of the probe and not shipped with it.

# Checks the d3d11va adapter entries of a report: every one is ok, failed or not_attempted with
# what that status needs, never crashed or timed out, names the adapter it used, and an ok one
# carries a coherent set of checks (the six profiles, listed or not as the profile list says, an
# ok profile only when it is listed, has its native output format, a 1080p configuration and a
# decoder object that was made).
function Test-VaAdapters($va) {
  foreach ($a in @($va.adapters)) {
    if ($a.status -notin @('ok', 'failed', 'not_attempted')) { throw "d3d11va adapter status: $($a.status)" }
    if ($a.status -eq 'failed' -and (-not $a.failed_stage -or -not $a.hresult)) { throw 'a failed d3d11va adapter names no stage' }
    if ($a.status -eq 'failed' -and $a.failed_stage -in @('crash', 'timeout')) { throw "d3d11va crashed or timed out on $($a.adapter.description)" }
    if ($a.status -eq 'not_attempted' -and (-not $a.reason -or -not $a.reason_code)) { throw 'a not attempted d3d11va adapter gives no reason' }
    if (-not $a.adapter.description) { throw 'a d3d11va entry does not name the adapter it used' }
    Write-Host ("d3d11va adapter '{0}' (software {1}): {2} {3} {4}" -f $a.adapter.description, $a.adapter.software_adapter, $a.status, $a.failed_stage, $a.reason)
    if ($a.status -ne 'ok') { continue }
    if (@($a.checked).Count -ne 6) { throw "d3d11va adapter $($a.adapter.description) did not check the six profiles" }
    if ($a.profile_count -ne @($a.profiles).Count) { throw 'profile_count differs from the listed profiles' }
    foreach ($c in @($a.checked)) {
      if ($c.status -notin @('ok', 'failed')) { throw "d3d11va profile status: $($c.status)" }
      if ($c.status -eq 'failed' -and (-not $c.failed_stage -or -not $c.reason)) { throw "a failed profile $($c.profile) names no stage or reason" }
      $listed = @($a.profiles | Where-Object { $_.guid -eq $c.guid }).Count -gt 0
      if ($listed -ne $c.listed) { throw "profile $($c.profile): listed does not match the profile list" }
      if ($c.status -eq 'ok') {
        if (-not $c.listed -or $c.decoder_object.status -ne 'ok') { throw "profile $($c.profile) is ok without being listed and created" }
        if (-not $c.output_formats.($c.native_output_format).supported) { throw "profile $($c.profile) is ok without its native output format" }
        if ($c.decoder_config_count.'1920x1080'.count -lt 1) { throw "profile $($c.profile) is ok without a 1080p configuration" }
      }
      if (-not $c.listed -and $c.status -ne 'failed') { throw "profile $($c.profile) is not listed and not reported failed" }
    }
  }
}
