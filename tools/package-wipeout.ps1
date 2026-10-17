# Create a clean player ZIP from an recorded and verified build. Never package user data.
[CmdletBinding()]
param([ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+$')][string]$Version='0.1.0')
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$record=Get-Content -LiteralPath (Join-Path $root "docs/releases/v$Version-build.json") -Raw | ConvertFrom-Json
if ($record.version -ne $Version) { throw 'Release record version mismatch.' }
$player=Join-Path $root 'pc-vr-tests/Wipeout/shadps4.exe'
if ((Get-FileHash -LiteralPath $player).Hash -ne $record.playerSHA256) { throw 'The player differs from the recorded release build.' }
$sourceChanges=& git -C $root diff $record.compiledPlayerCommit -- shadps4-arm64-main
if ($LASTEXITCODE -ne 0 -or $sourceChanges) { throw 'Emulator source differs from the recorded player revision.' }
$commit=(& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify the packaging commit.' }
$dirty=& git -C $root status --porcelain
if ($LASTEXITCODE -ne 0 -or $dirty) { throw 'Commit the reviewed packaging inputs before creating a release ZIP.' }
$name="RevivePSVR1-WipEout-v$Version-Windows-PCVR"
$output=Join-Path $root 'output'
$zip=Join-Path $output "$name.zip"
if ((Test-Path -LiteralPath $zip) -or (Test-Path -LiteralPath "$zip.sha256")) { throw 'An existing release archive must not be overwritten.' }
$stage=Join-Path $root ('build/release-staging/'+[guid]::NewGuid().ToString('N'))
$package=Join-Path $stage $name
New-Item -ItemType Directory -Path $package -Force | Out-Null
New-Item -ItemType Directory -Path $output -Force | Out-Null
function Copy-ReleaseFile([string]$Source,[string]$Relative) {
    if (!(Test-Path -LiteralPath $Source -PathType Leaf)) { throw "Missing release input: $Relative" }
    $destination=Join-Path $package $Relative
    New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
    Copy-Item -LiteralPath $Source -Destination $destination
}
$files=@(
    'Play WipEout VR.bat','Collect PCVR Logs.bat','README.md','BUILD.md','CHANGELOG.md',
    'LICENSE','THIRD-PARTY-NOTICES.md','games/README.txt',
    'docs/CONTROLS.md','docs/SAVES.md','docs/KNOWN-ISSUES.md','docs/PROVENANCE.md',
    'docs/PKG-IMPORT-PCVR.txt','docs/PCVR-DIAGNOSTICS.txt','docs/OPENXR-CONTROLLERS-PCVR.txt',
    'docs/dependencies.json','docs/import-provenance.json',
    "docs/releases/v$Version.md","docs/releases/v$Version-build.json",
    'tools/launch-psvr-test.ps1','tools/pcvr-settings.ps1','tools/pcvr-packages.ps1',
    'tools/pcvr-diagnostics.ps1','tools/collect-pcvr-logs.ps1',
    'tools/pcvr-pkg-extract.cs','tools/build-pcvr-pkg-extract.ps1','tools/pkgtool-runtime.config',
    'pc-vr/pkgtool/PkgTool.exe','pc-vr/pkgtool/LibOrbisPkg.dll',
    'pc-vr/pkgtool/PcvrPkgExtract.exe','pc-vr/pkgtool/PkgTool.exe.config',
    'pc-vr/pkgtool/PcvrPkgExtract.exe.config','pc-vr/pkgtool/LICENSE.txt','pc-vr/pkgtool/README.txt',
    'pc-vr-tests/Wipeout/shadps4.exe'
)
foreach ($file in $files) { Copy-ReleaseFile (Join-Path $root $file) $file }
foreach ($file in @('config.json','users.json')) {
    Copy-ReleaseFile (Join-Path $root "config/$file") "pc-vr-tests/Wipeout/user/$file"
}
foreach ($id in 1000..1003) {
    foreach ($folder in @('savedata','trophy','inputs')) {
        New-Item -ItemType Directory -Path (Join-Path $package "pc-vr-tests/Wipeout/user/home/$id/$folder") -Force | Out-Null
    }
}
$features=Get-Content -LiteralPath (Join-Path $root 'config/features.json') -Raw | ConvertFrom-Json
$features | Add-Member -NotePropertyName sha256 -NotePropertyValue $record.playerSHA256 -Force
$features | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $package 'pc-vr-tests/Wipeout/pcvr-features.json') -Encoding utf8
# Preserve original dependency license text without copying dependency binaries or samples.
$tracked=& git -C $root ls-files shadps4-arm64-main
if ($LASTEXITCODE -ne 0) { throw 'Cannot enumerate source licenses.' }
foreach ($file in $tracked) {
    $leaf=Split-Path -Leaf $file
    if ($leaf -match '^(LICENSE|COPYING|COPYRIGHT|NOTICE|AUTHORS)(\..*|-.*)?$') {
        Copy-ReleaseFile (Join-Path $root $file) ('licenses/'+$file)
    }
}
$manifest=[ordered]@{
    product='RevivePSVR1 - Wipeout'; version=$Version; releaseTag="v$Version"
    repository='https://github.com/jshodanVR/revive-psvr1-wipeout'
    packagingCommit=$commit; compiledPlayerCommit=$record.compiledPlayerCommit
    playerSHA256=$record.playerSHA256; headsetSmokeTest=$record.headsetSmokeTest
    fullPlaythroughVerified=$false; defaultResolutionScale=100; enhancedDefault=$false
    files=@()
}
foreach ($file in (Get-ChildItem -LiteralPath $package -Recurse -File | Sort-Object FullName)) {
    $relative=$file.FullName.Substring($package.Length+1).Replace('\','/')
    if ($relative -match '(?i)(\.pkg$|\.pak$|\.sfo$|eboot\.bin$|\.log$|\.pdb$|\.git/|\.ssh/|ed25519|/savedata/|/trophy/|/inputs/)') {
        throw "Forbidden payload in release: $relative"
    }
    if ($relative.StartsWith('games/') -and $relative -ne 'games/README.txt') { throw 'Game payload detected.' }
    $manifest.files += [ordered]@{path=$relative; bytes=$file.Length; sha256=(Get-FileHash -LiteralPath $file.FullName).Hash}
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $package 'build-info.json') -Encoding utf8
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($package,$zip,[IO.Compression.CompressionLevel]::Optimal,$true)
$hash=(Get-FileHash -LiteralPath $zip).Hash.ToLowerInvariant()
"$hash  $name.zip" | Set-Content -LiteralPath "$zip.sha256" -Encoding ascii
[pscustomobject]@{Zip=$zip; SHA256=$hash; Files=$manifest.files.Count; Staging=$package} | ConvertTo-Json
