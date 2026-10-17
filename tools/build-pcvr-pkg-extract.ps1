# Build with the Windows .NET Framework compiler; no SDK download is required.
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
$compiler=Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
$folder=Join-Path $root 'pc-vr\pkgtool'
if (!(Test-Path -LiteralPath $compiler)) { throw 'The .NET Framework 4 compiler is missing.' }
& $compiler /nologo /target:exe /platform:x64 /optimize+ ('/out:'+(Join-Path $folder 'PcvrPkgExtract.exe')) ('/reference:'+(Join-Path $folder 'LibOrbisPkg.dll')) (Join-Path $PSScriptRoot 'pcvr-pkg-extract.cs')
if ($LASTEXITCODE -ne 0) { throw 'Read-only extractor build failed.' }
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'pkgtool-runtime.config') -Destination (Join-Path $folder 'PcvrPkgExtract.exe.config')
