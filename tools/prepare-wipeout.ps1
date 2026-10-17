$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$run=Join-Path $root 'pc-vr-tests/Wipeout'
$source=Join-Path $root 'build/win-x64/shadps4.exe'
$exe=Join-Path $run 'shadps4.exe'
if(!(Test-Path -LiteralPath $source)) { throw 'Build the Wipeout player first.' }
if(Get-Process -Name shadps4 -ErrorAction SilentlyContinue | Where-Object Path -EQ $exe) { throw 'Close this Wipeout player before rebuilding.' }
foreach($id in 1000..1003) {
    foreach($folder in @('savedata','trophy','inputs')) {
        New-Item -ItemType Directory -Force -Path (Join-Path $run "user/home/$id/$folder") | Out-Null
    }
}
foreach($name in @('config.json','users.json')) {
    $destination=Join-Path $run "user/$name"
    if(!(Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath (Join-Path $root "config/$name") -Destination $destination }
}
if(Test-Path -LiteralPath $exe) {
    $backup=Join-Path $root ('build/backups/'+[guid]::NewGuid().ToString())
    New-Item -ItemType Directory -Force -Path $backup | Out-Null
    Copy-Item -LiteralPath $exe -Destination $backup
}
Copy-Item -LiteralPath $source -Destination $exe -Force
$features=Get-Content (Join-Path $root 'config/features.json') -Raw | ConvertFrom-Json
$features | Add-Member -NotePropertyName sha256 -NotePropertyValue (Get-FileHash $exe).Hash -Force
$features | ConvertTo-Json | Set-Content (Join-Path $run 'pcvr-features.json') -Encoding utf8
& (Join-Path $PSScriptRoot 'build-pcvr-pkg-extract.ps1')
Write-Host 'Wipeout is ready to test. Add your PKG to games and open Play WipEout VR.bat.'
