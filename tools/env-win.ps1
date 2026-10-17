# Dot-source from PowerShell. Uses an installed Visual Studio C++ SDK and portable LLVM.
$ErrorActionPreference = 'Stop'
$astroRoot = Split-Path -Parent $PSScriptRoot
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw 'Install Visual Studio with Desktop development with C++, CMake and a Windows SDK.'
}
$astroVS = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $astroVS) { throw 'Visual Studio C++ tools were not found.' }
& (Join-Path $astroVS 'Common7\Tools\Launch-VsDevShell.ps1') -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$astroPaths = @(
    (Join-Path $astroRoot 'tools\llvm\bin'),
    (Join-Path $astroVS 'VC\Tools\Llvm\x64\bin'),
    (Join-Path $astroVS 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
    (Join-Path $astroVS 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja')
)
$env:PATH = ($astroPaths -join ';') + ';' + $env:PATH
foreach ($astroCommand in @('clang-cl.exe', 'lld-link.exe', 'cmake.exe', 'ninja.exe', 'rc.exe')) {
    if (-not (Get-Command $astroCommand -ErrorAction SilentlyContinue)) {
        throw "Missing $astroCommand. LLVM 21 or later is required; portable LLVM may be placed in tools/llvm."
    }
}
