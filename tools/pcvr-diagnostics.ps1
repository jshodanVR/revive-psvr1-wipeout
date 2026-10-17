# Read-only launch evidence for any Windows PCVR OpenXR headset.
# Never enumerate the complete environment, accounts, saves or game contents.
function Write-PcvrLaunchDiagnostics {
    param([string]$Run, [string]$Title, [string]$Exe, [hashtable]$Settings,
          [bool]$Headless, [bool]$IgnoreUpdate, [bool]$EnhancedEdition=$false)
    $directory=Join-Path $Run 'user\log'
    New-Item -ItemType Directory -Force -Path $directory | Out-Null
    $runtimeCandidates=@()
    $override=[Environment]::GetEnvironmentVariable('XR_RUNTIME_JSON','Process')
    if ($override) { $runtimeCandidates += [ordered]@{Source='XR_RUNTIME_JSON override'; Manifest=$override} }
    foreach ($key in @('HKLM:\SOFTWARE\Khronos\OpenXR\1','HKCU:\SOFTWARE\Khronos\OpenXR\1')) {
        $item=Get-ItemProperty -LiteralPath $key -Name ActiveRuntime -ErrorAction SilentlyContinue
        if ($item) { $runtimeCandidates += [ordered]@{Source=$key; Manifest=$item.ActiveRuntime} }
    }
    foreach ($candidate in $runtimeCandidates) {
        $candidate['Exists']=Test-Path -LiteralPath $candidate.Manifest -PathType Leaf
        if ($candidate.Exists) {
            try {
                $manifest=Get-Content -LiteralPath $candidate.Manifest -Raw | ConvertFrom-Json
                $candidate['Library']=$manifest.runtime.library_path
                $candidate['Name']=$manifest.runtime.name
            } catch { $candidate['ReadError']=$_.Exception.Message }
        }
    }
    $sha=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash
    # These are the retained players that emit profile suggestions/selection.
    # Other binaries still log runtime/headset/session/tracking and gamepad use.
    $profileLogging=$sha -in @(
        '922D9B2F5761F523F078C287BA2570A12601E896C274A24E8D65903A25369129',
        '560255C48B619FE6399A09F82F1B40469BB8BD82E55977C835DFB802AD50CAC7')
    try {
        $features=Get-Content -LiteralPath (Join-Path $Run 'pcvr-features.json') -Raw -ErrorAction Stop | ConvertFrom-Json
        if ($features.sha256 -eq $sha -and $features.activeProfileLogging -eq $true) { $profileLogging=$true }
    } catch { }
    $gpu=@()
    try { $gpu=@(Get-CimInstance Win32_VideoController -ErrorAction Stop | Select-Object Name,DriverVersion,VideoProcessor) }
    catch { $gpu=@([pscustomobject]@{ReadError=$_.Exception.Message}) }
    $cachedFov=$null
    $cache=Join-Path $Run 'user\vr_headset_fov.json'
    if (Test-Path -LiteralPath $cache) {
        try { $cachedFov=Get-Content -LiteralPath $cache -Raw | ConvertFrom-Json } catch { }
    }
    $data=[ordered]@{
        SchemaVersion=1; StartedUtc=[DateTime]::UtcNow.ToString('o'); Title=$Title
        ExecutableSha256=$sha; Windows=[Environment]::OSVersion.VersionString
        Is64BitProcess=[Environment]::Is64BitProcess; PowerShell=$PSVersionTable.PSVersion.ToString()
        GPU=$gpu; Headless=$Headless; IgnoreUpdate=$IgnoreUpdate; EnhancedEdition=$EnhancedEdition
        RuntimeCandidates=$runtimeCandidates
        RuntimeSelectionNote='Registry/override candidates only; shad_log.txt records the runtime actually opened.'
        ApiLayers=[Environment]::GetEnvironmentVariable('XR_ENABLE_API_LAYERS','Process')
        ApiLayerPath=[Environment]::GetEnvironmentVariable('XR_API_LAYER_PATH','Process')
        EffectiveLauncherSettings=$Settings; PreviousHeadsetFov=$cachedFov
        ActiveControllerProfileLogging=$profileLogging
        ControllerNote=$(if ($profileLogging) {
            'See shad_log.txt for suggested/active interaction profiles, session focus, tracking and input. A suggested binding is not proof of an active controller.'
        } else {
            'This retained player has no active-profile log. Missing profile lines mean unavailable evidence, not disconnected controllers. Farpoint/Wipeout have profile logging, but runtime selection can differ by application.'
        })
        Status='Ready to start; process success is not yet established.'
    }
    foreach ($name in @('pcvr-settings.json','pcvr-features.json')) {
        $path=Join-Path $Run $name
        if (Test-Path -LiteralPath $path) {
            try { $data[$name]=Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } catch { }
        }
    }
    $json=$data | ConvertTo-Json -Depth 10
    $json | Set-Content -LiteralPath (Join-Path $directory 'pcvr-launch.json') -Encoding UTF8
    $json | Set-Content -LiteralPath (Join-Path $directory ('pcvr-launch-'+(Get-Date -Format 'yyyyMMdd-HHmmssfff')+'.json')) -Encoding UTF8
}
