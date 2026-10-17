# Build Wipeout for Windows PCVR

Requirements: Windows x64, Git for Windows, Visual Studio 2022 with Desktop
development with C++, a Windows SDK, CMake, Ninja, and LLVM 21 or newer
(`clang-cl` and `lld-link`). Select the LLVM tools in Visual Studio Installer,
or place portable LLVM in `tools/llvm`. The build script discovers Visual Studio.

From the repository root in PowerShell:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools/build-wipeout.ps1 -Jobs 4
```

This configures a new build in `build/win-x64`, compiles the player and prepares
`pc-vr-tests/Wipeout`. Existing user settings and saves are preserved. No game is
started. The output uses this repository's sources, not another local checkout.
The generic source folder retains its inherited name `shadps4-arm64-main`, but
this build targets Windows x64 with OpenXR and disables the updater and Discord.

Dependencies are vendored source snapshots; no submodule initialization is needed.
Their origin/revision inventory is in `docs/dependencies.json`. Some dependencies
may require their own upstream build-time downloads; any such request must be
documented and pinned before a public release.

The package importer uses PkgTool/LibOrbisPkg 0.2.231. During initial preparation,
obtain the official v0.2 archive from https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2
and place `PkgTool.exe` and `LibOrbisPkg.dll` in `pc-vr/pkgtool`.
The local prepared checkout already has these prerequisites. The build compiles
our read-only extraction helper from `tools/pcvr-pkg-extract.cs` with the Windows
.NET Framework compiler. These tools keep their original license notices.

No game files are required to compile. To run, add your own Wipeout package to
`games` and open `Play WipEout VR.bat`. User verification comes before packaging.

Do not copy `build`, runtime `user` folders, game packages, caches or logs into Git.
Do not publish a ZIP until its build and headset checks are recorded.
