# PCVR only. The default uses the active OpenXR runtime and real controls.
[CmdletBinding()]
param(
    [Parameter(Mandatory)][ValidateSet('Wipeout')][string]$Title,
    [switch]$Headless,
    [switch]$Menu,
    [switch]$ShowSettings,
    [ValidatePattern('^[a-zA-Z0-9_-]*$')][string]$Variant = '',
    [string]$InputScriptFile = '',
    [string]$InputInboxFile = '',
    [switch]$PoseOnly,
    [switch]$IgnoreUpdate,
    [switch]$RepairGame,
    [switch]$WipeoutHandshakeTrial,
    [switch]$WipeoutStackTrial,
    [switch]$WipeoutImageSyncTrial,
    [switch]$WipeoutNoHmdOrderControl,
    [switch]$FixedResolution,
    [switch]$EnhancedEdition,
    [switch]$TraceVr,
    [switch]$TraceSync,
    [switch]$ThumperNoLabelControl,
    [switch]$ThumperNoLayerControl,
    [switch]$FarpointNoSbsControl,
    [ValidateSet(0,960,1536,1920,2160,2400,2688,3072)][int]$FarpointEyeWidth = 0,
    [switch]$WorldsMultilayerTrial,
    [switch]$WorldsFrameLabelTrial,
    [switch]$RushNoHtileControl,
    [switch]$RushNoBufferDrainControl,
    [switch]$RushNoBufferGrowthControl,
    [switch]$PoseSweep,
    [switch]$RaceInput,
    [switch]$StereoAudit,
    [switch]$EyeLabels,
    [ValidateSet('psvr','headset')][string]$FovOf = 'headset',
    [ValidateSet(100,125,150,200)][int]$ResolutionScale = 100,
    [ValidateSet('right','left')][string]$PadHand = 'right',
    [ValidateRange(0,100)][int]$Sharpen = 0,
    [ValidateSet('auto','gamepad','aim')][string]$Controllers = 'auto',
    [ValidateRange(1,100000)][int]$StereoAuditFrame = 600,
    [ValidateRange(0,1800)][int]$Seconds = 0,
    [ValidatePattern('^[a-zA-Z0-9_-]+$')][string]$Label = 'pcvr',
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$WipeoutHandshakeTrial=$true
$WipeoutStackTrial=$true
$WipeoutImageSyncTrial=$true
# Use the active OpenXR headset's FOV for every PCVR title and entry point.
# An explicit -FovOf psvr remains available for controlled diagnostic comparisons.
# Without a live runtime the core can fall back to cached optics or PSVR optics;
# a headless dry run verifies this selection, not physical stereo correctness.
if ($Variant -and (!$Headless -or $Menu -or $ShowSettings)) { throw 'Development variants require Headless without a menu.' }
if ($InputScriptFile -and (!$Headless -or !$Variant)) { throw 'Research input requires a Headless variant.' }
if ($InputScriptFile) { $InputScriptFile = (Resolve-Path -LiteralPath $InputScriptFile).Path }
if ($InputInboxFile -and (!$Headless -or !$Variant)) { throw 'Live research commands require a Headless variant.' }
if ($InputInboxFile) { $InputInboxFile = [IO.Path]::GetFullPath($InputInboxFile) }
if ($RushNoBufferDrainControl -and (!$Headless -or $Variant -ne 'research' -or $Title -ne 'RushOfBlood')) { throw 'The buffer retirement control requires RushOfBlood Headless research.' }
if ($RushNoBufferGrowthControl -and (!$Headless -or $Variant -ne 'research' -or $Title -ne 'RushOfBlood')) { throw 'The buffer growth control requires RushOfBlood Headless research.' }
$runtimeName = if ($Variant) { "$Title-$Variant" } else { $Title }
$run = Join-Path $root "pc-vr-tests\$runtimeName"
$exe = Join-Path $run 'shadps4.exe'
$games = @{
    Wipeout = 'games\Wipeout-CUSA05670\uroot\eboot.bin'
    Statik = 'games\Statik-CUSA06929\uroot\eboot.bin'
    BloodTruth = 'games\BloodTruth-CUSA11098\uroot\eboot.bin'
    Thumper = 'games\Thumper-CUSA03559\uroot\eboot.bin'
    RushOfBlood = 'games\RushOfBlood-CUSA02350\uroot\eboot.bin'
    Farpoint = 'games\Farpoint-CUSA04508\uroot\eboot.bin'
    VRWorlds = 'games\VRWorlds-CUSA01690\uroot\eboot.bin'
}
$game = Join-Path $root $games[$Title]
if (!(Test-Path -LiteralPath $exe)) { throw "Missing file: $exe" }
if (!$DryRun) {
    if (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'shadps4*' }) {
        throw 'Close shadPS4 before preparing or starting another game.'
    }
    . (Join-Path $PSScriptRoot 'pcvr-packages.ps1')
    try {
        $gameReady=Initialize-PcvrGame -Root $root -Game $game -IgnoreUpdate:$IgnoreUpdate -Repair:$RepairGame -NonInteractive:$Headless
        if (!$gameReady) { return }
    } catch {
        if ($Headless) { throw }
        Add-Type -AssemblyName System.Windows.Forms
        [void][Windows.Forms.MessageBox]::Show($_.Exception.Message,'Game preparation stopped','OK','Warning')
        Write-Host 'Preparation stopped. No game was started.'
        return
    }
}
foreach ($path in @($exe, $game)) {
    if (!(Test-Path -LiteralPath $path)) { throw "Missing file: $path" }
}
if (($Menu -or $ShowSettings) -and !$Headless -and !$DryRun) {
    . (Join-Path $PSScriptRoot 'pcvr-settings.ps1')
    $capabilities = Get-PcvrCapabilities $Title $run $game
    $preferences = Read-PcvrSettings $run $capabilities
    if ($PSBoundParameters.ContainsKey('EnhancedEdition')) { $preferences.EnhancedEdition=[bool]$EnhancedEdition }
    Add-Type -AssemblyName System.Windows.Forms
    $shift = ([Windows.Forms.Control]::ModifierKeys -band [Windows.Forms.Keys]::Shift) -ne 0
    if ($preferences.ShowMenu -or $ShowSettings -or $shift) {
        $preferences = Show-PcvrSettings $capabilities $preferences
        if ($null -eq $preferences) { return }
        Save-PcvrSettings $run $preferences $capabilities
    }
    $ResolutionScale = [int]$preferences.ResolutionScale
    $Sharpen = [int]$preferences.Sharpen
    $Controllers = $preferences.Controllers
    $FixedResolution = [bool]$preferences.FixedResolution
    $PadHand = $preferences.PadHand
    $EnhancedEdition = [bool]$preferences.EnhancedEdition
}
if (!$Menu -and !$ShowSettings -and !$PSBoundParameters.ContainsKey('PadHand')) {
    $savedSettings = Join-Path $run 'pcvr-settings.json'
    if (Test-Path -LiteralPath $savedSettings) {
        $savedHand = (Get-Content -LiteralPath $savedSettings -Raw | ConvertFrom-Json).PadHand
        if ($savedHand -in @('right','left')) { $PadHand=$savedHand }
    }
}
if ($ResolutionScale -gt 100 -and $Title -notin @('Thumper','Statik','Wipeout','RushOfBlood')) { throw 'Higher resolution is not supported for this title yet.' }
if ($Title -eq 'RushOfBlood' -and $ResolutionScale -gt 100) {
    . (Join-Path $PSScriptRoot 'pcvr-settings.ps1')
    if ((Get-PcvrCapabilities $Title $run $game).MaxScale -lt $ResolutionScale) {
        throw 'This Rush player or game version does not support the selected scale. Install the complete updated build or select 100%.'
    }
}
if ($Title -eq 'Thumper' -and $ResolutionScale -gt 150) { throw 'Thumper supports up to 150%. The 200% allocation test crashed; this scale is not enabled.' }
if ($IgnoreUpdate -and $Title -eq 'Thumper' -and $ResolutionScale -gt 100) { throw 'Thumper resolution scaling requires version 1.06.' }
if ($PoseOnly -and !$Headless) { throw 'PoseOnly requires Headless.' }
if ($Controllers -eq 'aim' -and $Title -ne 'Farpoint') { throw 'Aim mode is available only for Farpoint.' }
if ($FarpointEyeWidth -gt 0 -and $Title -ne 'Farpoint') { throw 'Farpoint eye width applies only to Farpoint.' }
if ($Controllers -eq 'aim' -and !$Variant) {
    . (Join-Path $PSScriptRoot 'pcvr-settings.ps1')
    if (!(Get-PcvrCapabilities $Title $run $game).Aim) { throw 'This build does not support Aim yet. Select Automatic or update the Farpoint player.' }
}
if ($RaceInput -and (!$Headless -or $Title -ne 'Wipeout' -or $PoseOnly -or $PoseSweep)) { throw 'RaceInput requires Wipeout Headless without PoseOnly/PoseSweep.' }
if ($PoseSweep -and (!$Headless -or $PoseOnly)) { throw 'PoseSweep requires Headless without PoseOnly.' }
if ($WipeoutHandshakeTrial -and $Title -ne 'Wipeout') { throw 'HandshakeTrial requires Wipeout.' }
if ($WipeoutStackTrial -and $Title -ne 'Wipeout') { throw 'StackTrial requires Wipeout.' }
if ($WipeoutImageSyncTrial -and $Title -ne 'Wipeout') { throw 'ImageSyncTrial requires Wipeout.' }
if ($WipeoutNoHmdOrderControl -and (!$Headless -or !$Variant -or $Title -ne 'Wipeout')) { throw 'The HMD ordering control requires Wipeout Headless with a variant.' }
if ($FixedResolution -and $Title -ne 'Wipeout') { throw 'Fixed resolution is supported only for Wipeout.' }
if ($EnhancedEdition -and $Title -ne 'Statik') { throw 'Enhanced edition is supported only for Statik.' }
if ($FixedResolution -and $IgnoreUpdate) { throw 'Wipeout fixed resolution requires version 1.07.' }
if (($ThumperNoLabelControl -or $ThumperNoLayerControl) -and (!$Headless -or $Title -ne 'Thumper')) { throw 'Thumper diagnostic controls require Thumper Headless.' }
if ($FarpointNoSbsControl -and (!$Headless -or !$Variant -or $Title -ne 'Farpoint')) { throw 'The Farpoint stereo control requires Farpoint Headless with a variant.' }
if ($WorldsMultilayerTrial -and (!$Headless -or !$Variant -or $Title -ne 'VRWorlds')) { throw 'The Worlds multilayer trial requires VRWorlds Headless with a variant.' }
if ($WorldsFrameLabelTrial -and !$WorldsMultilayerTrial) { throw 'The Worlds completion-label trial requires WorldsMultilayerTrial.' }
if ($RushNoHtileControl -and (!$Headless -or !$Variant -or $Title -ne 'RushOfBlood')) { throw 'The Rush HTILE control requires RushOfBlood Headless with a variant.' }
$scriptName = if ($RaceInput) { 'wipeout-race-input.txt' } elseif ($PoseSweep) { 'psvr-pose-sweep.txt' } elseif ($PoseOnly) { 'psvr-pose-only.txt' } elseif ($Title -eq 'BloodTruth' -or ($Title -eq 'RushOfBlood' -and $Controllers -ne 'gamepad')) {
    'psvr-two-moves.txt'
} else { 'psvr-pad-input.txt' }
$settings = @{
    SHADPS4_ENABLE_IPC = 'false'
    SHADPS4_VR = '1'
    SHADPS4_OPENXR = $(if ($Headless) { '0' } else { '1' })
    SHADPS4_VR_DEMO = '0'
    SHADPS4_VR_FOV_OF = $FovOf
    SHADPS4_VR_FOV = '100'
    # The per-title vr.json owns the fallback anchor selected in the menu.
    SHADPS4_VR_PAD_ANCHOR = $null
    SHADPS4_HMD_RESOLUTION_SCALE = $null
    SHADPS4_WIPEOUT_FIXED_RESOLUTION = $(if ($FixedResolution) { '1' } else { $null })
    SHADPS4_VR_SHARPEN = ($Sharpen / 100.0).ToString([Globalization.CultureInfo]::InvariantCulture)
    SHADPS4_THUMPER_RESOLUTION_SCALE = $(if ($Title -eq 'Thumper') { [string]$ResolutionScale } else { $null })
    SHADPS4_STATIK_RESOLUTION_SCALE = $(if ($Title -eq 'Statik') { [string]$ResolutionScale } else { $null })
    SHADPS4_WIPEOUT_RESOLUTION_SCALE = $(if ($Title -eq 'Wipeout') { [string]$ResolutionScale } else { $null })
    SHADPS4_STEREO_EYE_LABELS = $(if ($EyeLabels) { '1' } else { $null })
    SHADPS4_XR_HANDS = '0'
    SHADPS4_XR_PAD_HAND = $PadHand
    SHADPS4_XR_CONTROLLERS = $(if ($Controllers -eq 'gamepad' -and $Title -ne 'BloodTruth') { '0' } else { '1' })
    # Physical gamepads keep independent right-stick and real touchpad inputs.
    # The OpenXR controller-to-touchpad mapping is separate from this fallback.
    SHADPS4_STICK_TOUCHPAD = '0'
    SHADPS4_XR_RIGHT_STICK_DPAD = $(if ($Title -in @('Wipeout','Thumper')) { '1' } else { $null })
    SHADPS4_XR_WAIT = $(if ($Headless) { '0' } else { '30' })
    SHADPS4_XR_PAUSE = $(if ($Headless) { '0' } else { '1' })
    SHADPS4_INPUT_SCRIPT = $(if ($InputScriptFile) { $InputScriptFile } elseif ($Headless) { Join-Path $PSScriptRoot "tests\$scriptName" } else { $null })
    SHADPS4_INPUT_INBOX = $(if ($InputInboxFile) { $InputInboxFile } else { $null })
    SHADPS4_EMULATE_MOVE = $(if ($Title -eq 'BloodTruth' -or ($Title -eq 'RushOfBlood' -and $Controllers -ne 'gamepad')) { '1' } else { $null })
    SHADPS4_EMULATE_AIM = $(if ($Title -eq 'Farpoint' -and $Controllers -eq 'aim') { '1' } else { $null })
    SHADPS4_RUSH_RESOLUTION_SCALE = $(if ($Title -eq 'RushOfBlood' -and $ResolutionScale -gt 100) { [string]$ResolutionScale } else { $null })
    SHADPS4_TEST_REPROJECTION_FLIPS = '1'
    SHADPS4_TEST_HMD_FRAME_LABEL = $(if (($Title -eq 'Thumper' -and !$ThumperNoLabelControl) -or $Title -eq 'RushOfBlood' -or $WorldsFrameLabelTrial) { '1' } else { $null })
    SHADPS4_TEST_HMD_MULTILAYER = $(if ($Title -eq 'Farpoint' -or $WorldsMultilayerTrial) { '1' } else { $null })
    SHADPS4_TEST_FARPOINT_STACK = $(if ($Title -eq 'Farpoint') { '1' } else { $null })
    SHADPS4_TEST_TESS_LAYER = $(if ($Title -eq 'Thumper' -and !$ThumperNoLayerControl) { '1' } else { $null })
    SHADPS4_TEST_HMD_BASE_LAYER = $(if ($Title -eq 'Statik') { '1' } else { $null })
    SHADPS4_TEST_RUSH_HTILE_STENCIL = $(if ($Title -eq 'RushOfBlood' -and !$RushNoHtileControl) { '1' } else { $null })
    SHADPS4_TEST_RUSH_COMPOSITOR_MASK = $(if ($Title -eq 'RushOfBlood') { '1' } else { $null })
    SHADPS4_TEST_BUFFER_RETIRE_BUDGET_MB = $(if ($Title -eq 'RushOfBlood' -and !$RushNoBufferDrainControl) { '512' } else { $null })
    SHADPS4_TEST_RUSH_BUFFER_GROWTH = $(if ($Title -eq 'RushOfBlood' -and !$RushNoBufferGrowthControl) { '1' } else { $null })
    SHADPS4_TEST_HMD_SBS = $(if ($Title -in @('Statik','RushOfBlood') -or ($Title -eq 'Farpoint' -and !$FarpointNoSbsControl)) { '1' } else { $null })
    # Preserve guest left/right order. FOV selection does not exchange the eyes;
    # headset correctness for the experimental titles still requires verification.
    SHADPS4_VR_SWAP_EYES = $null
    SHADPS4_TEST_NULL_UNMAPPED_BUFFERS = $(if ($Title -eq 'Statik') { '1' } else { $null })
    SHADPS4_TEST_VR_SERVICE_DIALOG = $(if ($Title -eq 'Statik') { '1' } else { $null })
    SHADPS4_TEST_WIPEOUT_STACK = $(if ($WipeoutStackTrial) { '1' } else { $null })
    SHADPS4_TEST_WIPEOUT_HANDSHAKE = $(if ($WipeoutHandshakeTrial) { '1' } else { $null })
    # Copy complete eye images between graphics submissions, not mid-command-buffer.
    SHADPS4_TEST_WIPEOUT_HMD_ORDER = $(if ($Title -eq 'Wipeout' -and !$WipeoutNoHmdOrderControl) { '1' } else { $null })
    SHADPS4_TEST_READCONST_IMAGE_SYNC = $(if ($WipeoutImageSyncTrial) { '1' } else { $null })
    SHADPS4_TRACE_HMD_LAYOUT = $(if ($TraceVr -or !$Headless) { '1' } else { $null })
    SHADPS4_STEREO_AUDIT = $(if ($StereoAudit) { '1' } else { $null })
    SHADPS4_STEREO_AUDIT_FRAME = $(if ($StereoAudit) { [string]$StereoAuditFrame } else { $null })
    SHADPS4_TRACE_VR_EVENTS = $null
    SHADPS4_TRACE_SYNC = $(if ($TraceSync) { '1' } else { $null })
}
# Research source/readback overrides must never leak into a normal player launch.
if (!$Variant) {
    $settings.SHADPS4_TEST_RUSH_COPY_CPU_SOURCE=$null
    $settings.SHADPS4_TEST_RUSH_COPY_READBACK=$null
    $settings.SHADPS4_TEST_RUSH_COPY_IMAGES=$null
    $settings.SHADPS4_TEST_RUSH_MIN_LOD=$null
    $settings.SHADPS4_TEST_RUSH_RESOLUTION_SCALE=$null
    $settings.SHADPS4_TEST_RUSH_RESOLUTION_ALIGN=$null
    $settings.SHADPS4_TEST_RUSH_POOL_SCALE=$null
    $settings.SHADPS4_TEST_NEO_TILING=$null
}
if ($DryRun) {
    [pscustomobject]@{Executable=$exe; Game=$game; WorkingDirectory=$run; Environment=$settings; IgnoreUpdate=[bool]$IgnoreUpdate; EnhancedEdition=[bool]$EnhancedEdition}
    return
}
if (Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'shadps4*' }) {
    throw 'A shadPS4 emulator is already running. Close it before starting this test.'
}
if ($Title -eq 'RushOfBlood' -and !$Variant) {
    . (Join-Path $PSScriptRoot 'pcvr-settings.ps1')
    if ((Get-PcvrCapabilities $Title $run $game).Width -ne 1344) {
        throw 'The Rush player and its capability file do not match. Extract the complete release together.'
    }
    # Reapply the release profile when users restore an older user folder with saves.
    $configPath=Join-Path $run 'user\config.json'
    $config=Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if (!$config.General.neo_mode) {
        $config.General.neo_mode=$true
        $config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $configPath -Encoding UTF8
    }
    Write-Host "Rush: PS4 Pro mode, rendering scale $ResolutionScale%. Experimental; full campaign and headset performance need testing."
}
Write-Host "$Title - experimental PCVR build. See docs/KNOWN-ISSUES.md for known issues."
if ($Title -eq 'Wipeout' -and $ResolutionScale -gt 100) {
    if (!$Variant) {
        $memoryFeatures = Get-Content -LiteralPath (Join-Path $run 'pcvr-features.json') -Raw | ConvertFrom-Json
        if ($memoryFeatures.resolutionMemoryBudget -ne $true -or
            $memoryFeatures.sha256 -ne (Get-FileHash -LiteralPath $exe).Hash) {
            throw 'Higher WipEout resolution requires the updated memory-budget player. Install the complete release, or select 100%.'
        }
    }
    # Match WipeoutResolution::RequiredExtraMemoryMB. The player also expands
    # the game's fixed heap; extra emulator memory alone does not fix loading.
    $requiredMemory = switch ($ResolutionScale) { 125 { 512 } 150 { 1024 } 200 { 2048 } }
    $configPath = Join-Path $run 'user/config.json'
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if ([int]$config.General.extra_dmem_in_mbytes -lt $requiredMemory) {
        $config.General | Add-Member -Force -NotePropertyName extra_dmem_in_mbytes -NotePropertyValue $requiredMemory
        $config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $configPath -Encoding UTF8
    }
    Write-Host "WipEout: $ResolutionScale% rendering, with $requiredMemory MiB of additional game-memory budget."
}
if ($Title -eq 'Farpoint') {
    . (Join-Path $PSScriptRoot 'pcvr-farpoint-profile.ps1')
    $extraMemory = Set-PcvrFarpointProfile -Game $game -EyeWidth $FarpointEyeWidth -IgnoreUpdate:$IgnoreUpdate
    $configPath = Join-Path $run 'user/config.json'
    $config = Get-Content -LiteralPath $configPath -Raw | ConvertFrom-Json
    if ([int]$config.General.extra_dmem_in_mbytes -lt $extraMemory) {
        $config.General.extra_dmem_in_mbytes = $extraMemory
        $config | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $configPath -Encoding UTF8
    }
}
if ($Title -eq 'Statik') {
    . (Join-Path $PSScriptRoot 'statik-enhanced.ps1')
    Set-StatikEnhancedEdition -Game $game -Enabled ([bool]$EnhancedEdition) -IgnoreUpdate:$IgnoreUpdate | Write-Host
    Write-Host 'Use a PC DualShock/DualSense or VR controllers; automatic mode follows the controls you use.'
    Write-Host "VR controllers: the $PadHand hand moves the virtual pad. Updated player: both sticks analog; hold Menu for left D-pad / right touchpad. Frame keeps native controls."
    Write-Host 'VR recenter: press both stick clicks without Menu, holding the controllers comfortably ahead; the view and box reset together.'
    Write-Host 'PC gamepad: hold Options/Start for 1 second to recenter view and virtual pad.'
    $features = Get-Content -LiteralPath (Join-Path $run 'pcvr-features.json') -Raw | ConvertFrom-Json
    if ($features.completeDualShock -eq $true -and $features.sha256 -eq (Get-FileHash -LiteralPath $exe).Hash) {
        . (Join-Path $PSScriptRoot 'statik-controls-guide.ps1')
        try { Install-StatikControlsGuide -Game $game | Write-Host }
        catch { Write-Warning "PCVR controls image could not be installed; leaving existing game files unchanged: $($_.Exception.Message)" }
    }
}
$saved = @{}
try {
    foreach ($key in $settings.Keys) {
        $saved[$key] = [Environment]::GetEnvironmentVariable($key, 'Process')
        if ($null -eq $settings[$key]) { Remove-Item "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key, $settings[$key], 'Process') }
    }
    $log = Join-Path $run 'user\log\shad_log.txt'
    if (Test-Path -LiteralPath $log) {
        Copy-Item -LiteralPath $log -Destination (Join-Path $run ('previous-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff') + '.log'))
    }
    if ($StereoAudit) {
        # Preserve previous evidence, but never mix a new headless capture with
        # headset.png left over from an earlier OpenXR run.
        $auditDirectory = Join-Path $run 'user\stereo_audit'
        $previousAudit = Join-Path $auditDirectory ('previous\' + (Get-Date -Format 'yyyyMMdd-HHmmssfff'))
        foreach ($fileName in @('guest.png','guest-left.png','guest-right.png','composed.png','headset.png','headset-simulated.png')) {
            $auditFile = Join-Path $auditDirectory $fileName
            if (Test-Path -LiteralPath $auditFile) {
                New-Item -ItemType Directory -Path $previousAudit -Force | Out-Null
                Move-Item -LiteralPath $auditFile -Destination (Join-Path $previousAudit $fileName)
            }
        }
        # Periodic captures also belong to one run. Otherwise old high-numbered
        # frames can be mistaken for new evidence after a slower or failed run.
        if (Test-Path -LiteralPath $auditDirectory) {
            $auditRootFull=[IO.Path]::GetFullPath($auditDirectory).TrimEnd('\')+'\'
            foreach ($frameDir in (Get-ChildItem -LiteralPath $auditDirectory -Directory -Filter 'frame-*')) {
                $sourceFull=[IO.Path]::GetFullPath($frameDir.FullName)
                $destFull=[IO.Path]::GetFullPath((Join-Path $previousAudit $frameDir.Name))
                if (!$sourceFull.StartsWith($auditRootFull,[StringComparison]::OrdinalIgnoreCase) -or
                    !$destFull.StartsWith($auditRootFull,[StringComparison]::OrdinalIgnoreCase) -or
                    ($frameDir.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
                    throw 'Unexpected stereo audit path.'
                }
                New-Item -ItemType Directory -Path $previousAudit -Force | Out-Null
                Move-Item -LiteralPath $sourceFull -Destination $destFull
            }
        }
    }
    $arguments = @('-g', ('"{0}"' -f $game))
    if ($IgnoreUpdate) { $arguments += '--ignore-game-patch' }
    try {
        . (Join-Path $PSScriptRoot 'pcvr-diagnostics.ps1')
        Write-PcvrLaunchDiagnostics -Run $run -Title $Title -Exe $exe -Settings $settings -Headless ([bool]$Headless) -IgnoreUpdate ([bool]$IgnoreUpdate) -EnhancedEdition ([bool]$EnhancedEdition)
    } catch {
        Write-Warning "Launch diagnostics could not be written: $($_.Exception.Message)"
    }
    $windowStyle = if ($Headless) { 'Hidden' } else { 'Normal' }
    $process = Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $run -WindowStyle $windowStyle -PassThru -RedirectStandardOutput (Join-Path $run "$Label-console.txt") -RedirectStandardError (Join-Path $run "$Label-errors.txt")
    # Cache the handle before a short-lived crash exits, so ExitCode is retained.
    $null=$process.Handle
    Write-Output "PID=$($process.Id) Headless=$Headless Title=$Title"
    if ($Seconds -eq 0) { return }
    $timedOut = !$process.WaitForExit($Seconds * 1000)
    if ($timedOut) { Stop-Process -Id $process.Id; $process.WaitForExit() }
    if (Test-Path -LiteralPath $log) { Copy-Item -LiteralPath $log -Destination (Join-Path $run "$Label.log") }
    [pscustomobject]@{Title=$Title; TimedOut=$timedOut; ExitCode=$process.ExitCode; Seconds=$Seconds} |
        ConvertTo-Json | Set-Content -LiteralPath (Join-Path $run "$Label-result.json") -Encoding UTF8
    Write-Output "Test finished. Time limit reached=$timedOut ExitCode=$($process.ExitCode). Logs: $run"
} finally {
    foreach ($key in $saved.Keys) {
        if ($null -eq $saved[$key]) { Remove-Item "Env:$key" -ErrorAction SilentlyContinue }
        else { [Environment]::SetEnvironmentVariable($key, $saved[$key], 'Process') }
    }
}
