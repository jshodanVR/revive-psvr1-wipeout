[CmdletBinding()]
param([ValidateRange(1,128)][int]$Jobs=4,[switch]$ConfigureOnly)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if(!$ConfigureOnly) {
    foreach($tool in @('PkgTool.exe','LibOrbisPkg.dll')) {
        if(!(Test-Path -LiteralPath (Join-Path $root "pc-vr/pkgtool/$tool"))) {
            throw "Missing package-import prerequisite: $tool. Follow the PkgTool setup in BUILD.md."
        }
    }
}
& (Join-Path $PSScriptRoot 'build-pc-vr.ps1') -Jobs $Jobs -ConfigureOnly:$ConfigureOnly
if($ConfigureOnly) { return }
& (Join-Path $PSScriptRoot 'prepare-wipeout.ps1')
