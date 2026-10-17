# Windows x64/OpenXR build of the independent WipEout source tree.
[CmdletBinding()]
param([switch]$ConfigureOnly,[ValidateRange(1,128)][int]$Jobs=4)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot 'env-win.ps1')
function Invoke-Checked([string]$Program,[string[]]$Arguments) {
    & $Program @Arguments
    if($LASTEXITCODE -ne 0) { throw "$Program failed (exit $LASTEXITCODE)." }
}
$source=Join-Path $root 'shadps4-arm64-main'
$build=Join-Path $root 'build/win-x64'
Invoke-Checked 'cmake.exe' @('-S',$source,'-B',$build,'-G','Ninja',
    '-DCMAKE_BUILD_TYPE=Release','-DCMAKE_C_COMPILER=clang-cl',
    '-DCMAKE_CXX_COMPILER=clang-cl','-DCMAKE_EXPORT_COMPILE_COMMANDS=ON',
    '-DENABLE_OPENXR=ON','-DENABLE_BACHATA_RUNTIME=OFF',
    '-DENABLE_FEX_GUEST_CPU=OFF','-DENABLE_DISCORD_RPC=OFF','-DENABLE_UPDATER=OFF')
if($ConfigureOnly) { return }
Invoke-Checked 'cmake.exe' @('--build',$build,'--target','shadps4','--parallel',"$Jobs")
