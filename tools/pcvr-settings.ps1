# Shared Windows PCVR menu. Dot-source from the launcher; no emulator is started here.
function Get-PcvrCapabilities([string]$Title, [string]$Run, [string]$Game) {
    $profiles = @{
        VRWorlds = @{ Name='PlayStation VR Worlds'; Status='Not playable yet: startup stops on unsupported shader resource access. Import and launch are available for diagnostics.'; Width=0; Height=0; Hash=''; Pad=$true }
        Thumper = @{ Name='Thumper'; Status='Positive first headset test: smooth gameplay, no issues reported. Full playthrough not verified.'; Width=1344; Height=1512; Hash='00343FCB76DA1890E6EC1F0BA9E5BB05E2619ED0BD4A61B7A6331C1E5E79E561'; Pad=$true }
        Statik = @{ Name='Statik'; Status='Version 0.1.0 pre-release. Headset smoke test passed; full playthrough not verified. See docs/KNOWN-ISSUES.md.'; Width=1392; Height=1566; Hash='2F38FC89C11EB7F42E083DD0D2745F905AD9034CDD9CEA9C7FACF065DD0AA24E'; Pad=$true }
        Wipeout = @{ Name='WipEout Omega Collection'; Status='Experimental: loading-memory, HD/Fury stack and shader-cache fixes included. Polygon artifacts and occasional black race views remain.'; Width=1344; Height=1512; Hash='3A52E910F6C450CC2F0C85EF994BE5D8C951C326A362BCC4775F29E6FF7F644C'; Pad=$false }
        BloodTruth = @{ Name='Blood & Truth'; Status='Not playable yet: the renderer stops before gameplay.'; Width=0; Height=0; Hash=''; Pad=$false }
        RushOfBlood = @{ Name='Until Dawn: Rush of Blood'; Status='Not playable beyond the tutorial: severe graphics corruption remains. Higher native resolution and border correction included.'; Width=0; Height=0; Hash=''; Pad=$true }
        Farpoint = @{ Name='Farpoint'; Status='Experimental: room and weapon visible. Graphics and campaign progression remain unverified.'; Width=0; Height=0; Hash=''; Pad=$true }
    }
    $p = $profiles[$Title].Clone()
    $p.Title = $Title
    $p.PoseHand = $Title -ne 'BloodTruth'
    $p.CompleteDualShock = $false
    $p.MaxScale = 100
    $p.FixedResolution = $false
    $p.Aim = $false
    try {
        $features = Get-Content -LiteralPath (Join-Path $Run 'pcvr-features.json') -Raw | ConvertFrom-Json
        $p.CompleteDualShock = $Title -eq 'Statik' -and $features.completeDualShock -eq $true -and
            $features.sha256 -eq (Get-FileHash (Join-Path $Run 'shadps4.exe')).Hash
    } catch { }
    if ($Title -eq 'Farpoint') {
        try {
            $marker = Join-Path $Run 'pcvr-features.json'
            if (Test-Path -LiteralPath $marker) {
                $features = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
                $p.Aim = $features.aim -eq $true -and $features.sha256 -eq (Get-FileHash (Join-Path $Run 'shadps4.exe')).Hash
                if ($p.Aim) { $p.Status='Experimental Aim emulation with two VR controllers. Graphics and campaign gameplay still need work.' }
            }
        } catch { $p.Aim = $false }
    }
    $p.ResolutionNote = 'Higher rendering resolution is not available for this title yet.'
    if ($Title -eq 'RushOfBlood') {
        try {
            $features = Get-Content -LiteralPath (Join-Path $Run 'pcvr-features.json') -Raw | ConvertFrom-Json
            if ($features.nativeNeo -eq $true -and $features.compositorMask -eq $true -and
                $features.sha256 -eq (Get-FileHash (Join-Path $Run 'shadps4.exe')).Hash) {
                $p.Width=1344; $p.Height=1512
                $p.ResolutionLabel='PS4 Pro native - 1344 x 1512 pixels per eye'
                $p.ResolutionNote='36% more pixels than the original PS4 mode. Higher custom scales are not supported. This does not fix post-tutorial corruption.'
                if ($features.rushTextureResidency -eq $true -and $features.rushGpuImageCopies -eq $true) {
                    $p.Status='Experimental: streamed texture and memory tracking fixes restore the tutorial and early outdoor range in automated tests. Full campaign and headset performance still need testing.'
                    $p.ResolutionNote='Native PS4 Pro resolution: 36% more pixels than the original PS4 mode. Higher custom scales are still being tested.'
                    if ($features.rushResolution -eq $true -and $features.maxResolutionScale -eq 200 -and
                        (Get-FileHash -LiteralPath $Game).Hash -eq '427D027915DBA4EBA418166EAE35D1C24D1B876957DCAE1428081BDE6F1CDF5C') {
                        $p.MaxScale=200; $p.ResolutionLabel=$null
                        $p.ResolutionNote='Experimental scaling, up to 4 times the pixels. Extra game memory is reserved automatically. Full campaign and headset performance are not verified.'
                    }
                }
            }
        } catch { Write-Warning 'Could not verify Rush native resolution support.' }
    }
    if ($p.Hash) {
        $actualGame = $Game
        $updateGame = Join-Path ((Split-Path -Parent $Game) + '-UPDATE') 'eboot.bin'
        if (Test-Path -LiteralPath $updateGame) { $actualGame = $updateGame }
        $marker = Join-Path $Run 'pcvr-features.json'
        if (Test-Path -LiteralPath $marker) {
            try {
                $features = Get-Content -LiteralPath $marker -Raw | ConvertFrom-Json
                if ($features.resolution -eq $Title -and $features.sha256 -eq (Get-FileHash (Join-Path $Run 'shadps4.exe')).Hash -and
                    (Get-FileHash -LiteralPath $actualGame).Hash -eq $p.Hash) {
                    $p.MaxScale = 150
                    if ($features.maxResolutionScale -eq 200) { $p.MaxScale = 200 }
                    # Older players scale the eyes without growing the fixed game heap.
                    # Unity Square exhausts it at 150%, even with more emulator memory.
                    if ($Title -eq 'Wipeout' -and $features.resolutionMemoryBudget -ne $true) { $p.MaxScale = 100 }
                    $p.FixedResolution = $Title -eq 'Wipeout' -and $features.fixedResolution -eq $true
                }
            } catch { Write-Warning 'Could not verify resolution support.' }
        }
        $p.ResolutionNote = if ($p.MaxScale -gt 100) {
            if ($Title -eq 'Wipeout') { 'Maximum per eye. Extra game memory is reserved automatically. Without fixed resolution, the game may lower resolution during a race.' }
            else { 'Renders more pixels in the game. Requires more GPU power and memory.' }
        } else { 'Original resolution: scaling is not verified for this game and emulator combination.' }
        if ($p.MaxScale -eq 100) { $p.Width=0; $p.Height=0 }
    }
    return $p
}

function Read-PcvrSettings([string]$Run, $Capabilities) {
    $s = Read-PcvrDefaultSettings
    $path = Join-Path $Run 'pcvr-settings.json'
    if (Test-Path -LiteralPath $path) {
        try {
            $saved = Get-Content -LiteralPath $path -Raw | ConvertFrom-Json
            foreach ($key in @($s.Keys)) {
                if ($null -ne $saved.PSObject.Properties[$key]) { $s[$key] = $saved.$key }
            }
            if ($s.ResolutionScale -notin @(100,125,150,200) -or $s.ResolutionScale -gt $Capabilities.MaxScale) { $s.ResolutionScale=100 }
            $s.Sharpen = [Math]::Max(0,[Math]::Min([int]$s.Sharpen,100))
            $s.PadDistance = [Math]::Max(20,[Math]::Min([int]$s.PadDistance,100))
            $s.PadHeight = [Math]::Max(0,[Math]::Min([int]$s.PadHeight,60))
            if ($s.Controllers -notin @('auto','gamepad','aim') -or ($s.Controllers -eq 'aim' -and !$Capabilities.Aim)) { $s.Controllers='auto' }
            if ($s.PadAnchor -notin @('head','seat')) { $s.PadAnchor='head' }
            if ($s.PadHand -notin @('right','left')) { $s.PadHand='right' }
            $s.ShowMenu = [bool]$s.ShowMenu
            $s.FixedResolution = $Capabilities.FixedResolution -and $s.FixedResolution -eq $true
            $s.EnhancedEdition = $Capabilities.Title -eq 'Statik' -and $s.EnhancedEdition -is [bool] -and $s.EnhancedEdition
        } catch { Write-Warning 'Could not read settings; using defaults.'; return Read-PcvrDefaultSettings }
    }
    return $s
}

function Read-PcvrDefaultSettings {
    return @{ ResolutionScale=100; FixedResolution=$false; EnhancedEdition=$false; Sharpen=0; Controllers='auto'; PadDistance=50; PadHeight=17; PadAnchor='head'; PadHand='right'; ShowMenu=$true }
}

function Save-PcvrSettings([string]$Run, $Settings, $Capabilities) {
    # Preserve unrelated VR configuration. The position is only the untracked pad fallback.
    if ($Capabilities.Pad) {
        $path = Join-Path $Run 'user\vr.json'
        $vr = if (Test-Path -LiteralPath $path) { Get-Content -LiteralPath $path -Raw | ConvertFrom-Json } else { [pscustomobject]@{} }
        $vr | Add-Member -Force -NotePropertyName pad_offset -NotePropertyValue @(0.0,(-$Settings.PadHeight/100.0),(-$Settings.PadDistance/100.0))
        $vr | Add-Member -Force -NotePropertyName pad_anchor -NotePropertyValue $Settings.PadAnchor
        $vr | ConvertTo-Json -Depth 20 | Set-Content -LiteralPath $path -Encoding UTF8
    }
    $Settings | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $Run 'pcvr-settings.json') -Encoding UTF8
}

function Show-PcvrSettings($Capabilities, $Settings, [string]$PreviewPath='') {
    Add-Type -AssemblyName System.Windows.Forms
    Add-Type -AssemblyName System.Drawing
    [Windows.Forms.Application]::EnableVisualStyles()
    $form = New-Object Windows.Forms.Form
    $form.Text = $Capabilities.Name + ' - PCVR'
    $form.StartPosition = 'CenterScreen'
    # A launcher started minimized/hidden must still show its interactive dialog.
    if (!$PreviewPath) { $form.Add_Shown({ $this.WindowState='Normal'; $this.Activate() }) }
    $form.AutoScaleMode = 'Dpi'
    $form.ClientSize = New-Object Drawing.Size(620, $(if ($Capabilities.Pad) { 625 } else { 495 }))
    $form.FormBorderStyle = 'FixedDialog'
    $form.MaximizeBox = $false
    $form.Font = New-Object Drawing.Font('Segoe UI',10)
    function Label($text,$y,$height=30) {
        $c = New-Object Windows.Forms.Label
        $c.BackColor=$form.BackColor
        $c.Text=$text; $c.Location=New-Object Drawing.Point(22,$y); $c.Size=New-Object Drawing.Size(576,$height)
        $form.Controls.Add($c); return $c
    }
    $heading=Label ($Capabilities.Name + ' / PC VR') 18 32
    $heading.Font=New-Object Drawing.Font('Segoe UI',15,[Drawing.FontStyle]::Bold)
    $null=Label $Capabilities.Status 58 46
    $null=Label 'Resolution per eye' 112
    $res=New-Object Windows.Forms.ComboBox
    $res.DropDownStyle='DropDownList'; $res.Location=New-Object Drawing.Point(22,143); $res.Size=New-Object Drawing.Size(576,28)
    $scales = @(100,125,150,200 | Where-Object { $_ -le $Capabilities.MaxScale })
    foreach ($scale in $scales) {
        $desc = if ($Capabilities.ResolutionLabel) { $Capabilities.ResolutionLabel } elseif ($Capabilities.Width) {
            $rw=[int]($Capabilities.Width*$scale/100); $rh=[int]($Capabilities.Height*$scale/100)
            if ($Capabilities.Title -eq 'RushOfBlood' -and $scale -gt 100) {
                $rw=[int]([Math]::Ceiling($rw/8.0)*8); $rh=[int]([Math]::Ceiling($rh/8.0)*8)
            }
            '{0}% - about {1} x {2} pixels ({3:0.00} times the pixels)' -f $scale,$rw,$rh,($scale*$scale/10000.0)
        } else { 'Original, controlled by the game' }
        [void]$res.Items.Add($desc)
    }
    $res.SelectedIndex=[Array]::IndexOf($scales,[int]$Settings.ResolutionScale)
    if ($res.SelectedIndex -lt 0) { $res.SelectedIndex=0 }
    $res.Enabled=$Capabilities.MaxScale -gt 100; $form.Controls.Add($res)
    $null=Label $Capabilities.ResolutionNote 180 43
    $null=Label 'Sharpening (final filter; does not increase resolution)' 231
    $sharp=New-Object Windows.Forms.TrackBar
    $sharp.Location=New-Object Drawing.Point(16,260); $sharp.Size=New-Object Drawing.Size(580,45)
    $sharp.Minimum=0; $sharp.Maximum=100; $sharp.TickFrequency=25; $sharp.Value=$Settings.Sharpen
    $form.Controls.Add($sharp)
    $controlsLabel=Label 'Controls' 306
    $controlsLabel.Width=360
    if ($Capabilities.CompleteDualShock) {
        $help=New-Object Windows.Forms.Button
        $help.Text='Controls guide'; $help.Location=New-Object Drawing.Point(458,302); $help.Size=New-Object Drawing.Size(140,29)
        $help.Add_Click({
            [void][Windows.Forms.MessageBox]::Show(@'
STATIK - TOUCH-STYLE CONTROLLERS (PIMAX / QUEST)
Right A = Cross; right B = Square; left X = Circle; left Y = Triangle.
Left/right grip = L1/R1. Left/right trigger = L2/R2.
Both sticks = the two DualShock sticks. Click them separately for L3/R3.
Tap and release left Menu = Options.
Menu alternative: hold left stick click + right A. Release both for Options.
Hold left Menu + left stick = D-pad (including diagonals).
Hold left Menu + right stick = touchpad finger; click right stick = touchpad press.
Release Menu to return to analog sticks. Using the modifier does not open Options.
Both stick clicks WITHOUT Menu = recenter the view and level/centre the box.
Hold the VR controllers comfortably in front of you when recentering.
PC gamepad: hold Options / Start for one second to recenter view and virtual pad.

STEAM FRAME (NATIVE PROFILE)
Use the real D-pad, both sticks/clicks, bumpers, triggers and A/B/X/Y.
Right Menu = Options. Left View = touchpad click; View + right stick = touchpad finger.

INDEX: left A/B replace X/Y; press the left trackpad for Menu/modifier.
Vive wands lack separate stick clicks: this layout cannot provide every DualShock command.
PS/Share are console functions and are not synthesized as game buttons.
Changing the tracked hand changes pose only, not the button layout.
'@,'Statik controls','OK','Information')
        })
        $form.Controls.Add($help); $help.BringToFront()
    }
    $controls=New-Object Windows.Forms.ComboBox
    $controls.DropDownStyle='DropDownList'; $controls.Location=New-Object Drawing.Point(22,335); $controls.Size=New-Object Drawing.Size(576,28)
    if ($Capabilities.Name -eq 'Blood & Truth') {
        [void]$controls.Items.Add('Two VR controllers (emulated PS Move pair)'); $controls.Enabled=$false
    } else {
        [void]$controls.Items.Add('Automatic: use PC gamepad or VR controllers')
        [void]$controls.Items.Add('PC gamepad only')
        if ($Capabilities.Aim) { [void]$controls.Items.Add('Two VR controllers: Aim gun (experimental)') }
    }
    $controls.SelectedIndex=0
    if ($controls.Enabled -and $Settings.Controllers -eq 'gamepad') { $controls.SelectedIndex=1 }
    if ($Capabilities.Aim -and $Settings.Controllers -eq 'aim') { $controls.SelectedIndex=2 }
    $form.Controls.Add($controls)
    $null=Label 'Automatic mode switches to the controls you use.' 373 28
    $bottom=410
    if ($Capabilities.Pad) {
        $null=Label 'Virtual position with an untracked PC gamepad' 410
        $null=Label 'Distance (cm)                        Below eye level (cm)' 441
        $distance=New-Object Windows.Forms.NumericUpDown
        $distance.Location=New-Object Drawing.Point(22,468); $distance.Size=New-Object Drawing.Size(135,28)
        $distance.Minimum=20; $distance.Maximum=100; $distance.Value=$Settings.PadDistance; $form.Controls.Add($distance)
        $height=New-Object Windows.Forms.NumericUpDown
        $height.Location=New-Object Drawing.Point(278,468); $height.Size=New-Object Drawing.Size(135,28)
        $height.Minimum=0; $height.Maximum=60; $height.Value=$Settings.PadHeight; $form.Controls.Add($height)
        $anchor=New-Object Windows.Forms.CheckBox
        $anchor.Text='Virtual controller follows your head'; $anchor.Checked=$Settings.PadAnchor -eq 'head'
        $anchor.Location=New-Object Drawing.Point(22,507); $anchor.Size=New-Object Drawing.Size(560,28); $form.Controls.Add($anchor)
        $null=Label 'PC pad: hold Options / Start for 1 second to recenter the view and virtual pad.' 536 38
        if ($Capabilities.Aim) {
            $updatePadControls = {
                $enabled = $controls.SelectedIndex -ne 2
                $distance.Enabled=$enabled; $height.Enabled=$enabled; $anchor.Enabled=$enabled
            }.GetNewClosure()
            $controls.Add_SelectedIndexChanged($updatePadControls)
            & $updatePadControls
        }
        $bottom=586
        $form.ClientSize=New-Object Drawing.Size($form.ClientSize.Width,($form.ClientSize.Height+38))
    }
    $menu=New-Object Windows.Forms.CheckBox
    $menu.Text='Show at every start'; $menu.Checked=$Settings.ShowMenu
    $menu.Location=New-Object Drawing.Point(22,$bottom); $menu.Size=New-Object Drawing.Size(260,30); $form.Controls.Add($menu)
    $play=New-Object Windows.Forms.Button
    $play.Text='Play'; $play.Location=New-Object Drawing.Point(350,$bottom); $play.Size=New-Object Drawing.Size(118,36)
    $play.DialogResult='OK'; $form.Controls.Add($play); $form.AcceptButton=$play
    $quit=New-Object Windows.Forms.Button
    $quit.Text='Quit'; $quit.Location=New-Object Drawing.Point(480,$bottom); $quit.Size=New-Object Drawing.Size(118,36)
    $quit.DialogResult='Cancel'; $form.Controls.Add($quit); $form.CancelButton=$quit
    $hint=Label 'Hold Shift when starting the BAT to reopen this window.' ($bottom+43) 25
    $hint.Font=New-Object Drawing.Font('Segoe UI',8)
    if ($Capabilities.Title -eq 'Wipeout') {
        $help=New-Object Windows.Forms.Button
        $help.Text='Pause / menu help'; $help.Location=New-Object Drawing.Point(426,302); $help.Size=New-Object Drawing.Size(172,29)
        $help.Add_Click({ [void][Windows.Forms.MessageBox]::Show("Pause in a race: left Menu (Touch/Pimax), right Menu (Steam Frame), or Options/Start (PC gamepad).`n`nOn the updated WipEout player, left stick click + right A also opens pause on non-Frame VR controllers. Use the D-pad/right stick to navigate; Cross confirms and Circle goes back.",'WipEout controls','OK','Information') })
        $form.Controls.Add($help); $help.BringToFront()
    }
    if ($Capabilities.PoseHand) {
        foreach ($control in $form.Controls) { if ($control.Top -ge 410) { $control.Top += 104 } }
        $form.ClientSize=New-Object Drawing.Size($form.ClientSize.Width,($form.ClientSize.Height+104))
        $null=Label 'VR hand that holds the virtual gamepad / Aim gun' 408 28
        $hand=New-Object Windows.Forms.ComboBox
        $hand.DropDownStyle='DropDownList'; $hand.Location=New-Object Drawing.Point(22,438); $hand.Size=New-Object Drawing.Size(576,28)
        [void]$hand.Items.Add('Right hand'); [void]$hand.Items.Add('Left hand')
        $hand.SelectedIndex=if ($Settings.PadHand -eq 'left') { 1 } else { 0 }
        $form.Controls.Add($hand)
        $null=Label 'Changes tracked position and rotation, not buttons. Does not affect a PC gamepad or two independent Move controllers.' 472 40
        $updateHand = { $hand.Enabled = $controls.SelectedIndex -ne 1 -and $Capabilities.Title -ne 'RushOfBlood' }.GetNewClosure()
        $controls.Add_SelectedIndexChanged($updateHand); & $updateHand
    }
    if ($Capabilities.FixedResolution) {
        # Make room only for titles with a verified fixed-resolution implementation.
        foreach ($control in $form.Controls) { if ($control.Top -ge 231) { $control.Top += 74 } }
        $form.ClientSize=New-Object Drawing.Size($form.ClientSize.Width,($form.ClientSize.Height+74))
        $fixed=New-Object Windows.Forms.CheckBox
        $fixed.Text='Keep the selected resolution during races'; $fixed.Checked=$Settings.FixedResolution
        $fixed.Location=New-Object Drawing.Point(22,228); $fixed.Size=New-Object Drawing.Size(576,28)
        $form.Controls.Add($fixed)
        $null=Label 'Disables automatic scaling. Sharper image, but may reduce smoothness. Experimental.' 258 43
    }
    if ($Capabilities.Title -eq 'Statik') {
        foreach ($control in $form.Controls) { if ($control.Top -ge 231) { $control.Top += 74 } }
        $form.ClientSize=New-Object Drawing.Size($form.ClientSize.Width,($form.ClientSize.Height+74))
        $enhanced=New-Object Windows.Forms.CheckBox
        $enhanced.Text='Enhanced edition'; $enhanced.Checked=$Settings.EnhancedEdition -eq $true
        $enhanced.Location=New-Object Drawing.Point(22,228); $enhanced.Size=New-Object Drawing.Size(576,28)
        $form.Controls.Add($enhanced)
        $null=Label 'Improved texture filtering and selected surface materials. Preserves labels, puzzle clues and transparency. Applies on the next launch.' 258 43
    }
    try {
        if ($PreviewPath) {
            $form.ShowInTaskbar=$false; $form.Opacity=0
            $form.Show(); $form.PerformLayout(); [Windows.Forms.Application]::DoEvents()
            $bitmap=New-Object Drawing.Bitmap($form.Width,$form.Height)
            try { $form.DrawToBitmap($bitmap,(New-Object Drawing.Rectangle(0,0,$form.Width,$form.Height))); $bitmap.Save($PreviewPath) } finally { $bitmap.Dispose() }
            $form.Controls | ForEach-Object { [pscustomobject]@{ Text=$_.Text; X=$_.Left; Y=$_.Top; Width=$_.Width; Height=$_.Height; Visible=$_.Visible } } |
                ConvertTo-Json | Set-Content -LiteralPath ($PreviewPath + '.json') -Encoding UTF8
            return $null
        }
        if ($form.ShowDialog() -ne 'OK') { return $null }
        $Settings.ResolutionScale=$scales[$res.SelectedIndex]
        $Settings.FixedResolution=$Capabilities.FixedResolution -and $fixed.Checked
        $Settings.EnhancedEdition=$Capabilities.Title -eq 'Statik' -and $enhanced.Checked
        $Settings.Sharpen=$sharp.Value
        $Settings.Controllers=if ($controls.SelectedIndex -eq 2 -and $Capabilities.Aim) { 'aim' } elseif ($controls.SelectedIndex -eq 1) { 'gamepad' } else { 'auto' }
        $Settings.ShowMenu=$menu.Checked
        if ($Capabilities.PoseHand) { $Settings.PadHand=if ($hand.SelectedIndex -eq 1) { 'left' } else { 'right' } }
        if ($Capabilities.Pad) {
            $Settings.PadDistance=[int]$distance.Value; $Settings.PadHeight=[int]$height.Value
            $Settings.PadAnchor=if ($anchor.Checked) { 'head' } else { 'seat' }
        }
        return $Settings
    } finally { $form.Dispose() }
}
