[CmdletBinding()]
param([string]$Root=(Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference='Stop'
$Root=[IO.Path]::GetFullPath($Root)
$output=Join-Path $Root 'output'
New-Item -ItemType Directory -Path $output -Force | Out-Null
$stamp=Get-Date -Format 'yyyyMMdd-HHmmssfff'
$stage=Join-Path $output "PCVR-support-$stamp"
New-Item -ItemType Directory -Path $stage | Out-Null
$copied=@()
$errors=@()
# Explicit allowlist. Never collect game, save, shader, account or cache contents.
foreach ($relative in @('pc-vr-tests/Wipeout')) {
    $run=Join-Path $Root $relative
    if (!(Test-Path -LiteralPath $run)) { continue }
    $files=@('user\log\shad_log.txt','user\log\pcvr-launch.json','pcvr-settings.json',
             'pcvr-features.json','user\vr_headset_fov.json','pcvr-console.txt','pcvr-errors.txt')
    # Include the latest previous emulator log to survive one accidental relaunch.
    $previous=Get-ChildItem -LiteralPath $run -Filter 'previous-*.log' -File |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 1
    if ($previous) { $files += $previous.Name }
    foreach ($file in $files) {
        $source=Join-Path $run $file
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        $destination=Join-Path $stage (Join-Path $relative $file)
        try {
            New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
            $copied += (Join-Path $relative $file)
        } catch { $errors += "$relative/$file : $($_.Exception.Message)" }
    }
}
$games=Join-Path $Root 'games'
if (Test-Path -LiteralPath $games) {
    foreach ($dir in (Get-ChildItem -LiteralPath $games -Directory -Filter '.pkg-import-*' -Force |
        Where-Object { !($_.Attributes -band [IO.FileAttributes]::ReparsePoint) } |
        Sort-Object LastWriteTimeUtc -Descending | Select-Object -First 10)) {
        $source=Join-Path $dir.FullName 'package-metadata.jsonl'
        if (!(Test-Path -LiteralPath $source -PathType Leaf)) { continue }
        if ((Get-Item -LiteralPath $source).Attributes -band [IO.FileAttributes]::ReparsePoint) { continue }
        $relative='package-import\'+$dir.Name+'.jsonl'
        try {
            $destination=Join-Path $stage $relative
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $destination) | Out-Null
            Copy-Item -LiteralPath $source -Destination $destination
            $copied+=$relative
        } catch { $errors+="$relative : $($_.Exception.Message)" }
    }
}
if (Test-Path -LiteralPath (Join-Path $Root 'build-info.json')) {
    Copy-Item -LiteralPath (Join-Path $Root 'build-info.json') -Destination $stage
    $copied += 'build-info.json'
}
@{
    CollectedUtc=[DateTime]::UtcNow.ToString('o'); Files=$copied; CopyErrors=$errors
    Note='Local diagnostic collection only. Files may contain local paths/device names. No upload performed. Latest live logs can be incomplete while a game is running.'
} | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'collection.json') -Encoding UTF8
@'
PCVR SUPPORT LOGS
Please describe the title, where the problem starts, your headset, controllers,
OpenXR runtime and whether a PC gamepad was connected. Note whether the issue
affects left eye, right eye, both eyes, tracking, buttons, graphics or smoothness.
Logs record runtime/session/FOV, tracking and controller profiles.
Importer diagnostics record package metadata and exact rejection reasons.
No game packages, game files, saves, account settings or shader dumps are included.
Nothing is uploaded automatically. Close the game and collect again for complete
final logs; a crash does not need to be reproduced just to collect existing logs.
'@ | Set-Content -LiteralPath (Join-Path $stage 'README.txt') -Encoding UTF8
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip="$stage.zip"
[IO.Compression.ZipFile]::CreateFromDirectory($stage,$zip)
Write-Output "Support archive created: $zip"
if ($errors.Count) { Write-Warning 'Some logs could not be copied. Details are in collection.json.' }
