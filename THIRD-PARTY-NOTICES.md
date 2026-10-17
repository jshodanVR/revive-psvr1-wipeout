# Third-party notices

RevivePSVR1 - WipEout is licensed under GPL-2.0-or-later; see [LICENSE](LICENSE).
Original copyright and SPDX notices remain in the source. Dependencies retain
their own licenses. The Windows release includes these components:

| Component | License | Corresponding source |
| --- | --- | --- |
| Modified emulator, launcher and package helper | GPL-2.0-or-later | This repository, at the release tag; the build manifest also records the exact compiled player revision |
| Libraries linked into the emulator | Their individual licenses | Vendored under `shadps4-arm64-main/externals`; origins and revisions in `docs/dependencies.json` |
| OpenXR loader | Apache-2.0 | Vendored `shadps4-arm64-main/externals/openxr-sdk` |
| FFmpeg core | See its included license notices | [ext-ffmpeg-core](https://github.com/shadps4-emu/ext-ffmpeg-core/tree/94dde08c8a9e4271a93a2a7e4159e9fb05d30c0a), pinned revision 94dde08; see BUILD.md |
| PkgTool and LibOrbisPkg 0.2.231 | LGPL-3.0 | [LibOrbisPkg v0.2](https://github.com/maxton/LibOrbisPkg/releases/tag/v0.2); binaries remain unchanged |

The ZIP includes dependency notices under `licenses` and the package tool's
license under `pc-vr/pkgtool`. Full source is available from this repository's
release tag. The Microsoft Visual C++ runtime is a separate prerequisite and
is not distributed in the ZIP.

## Source attribution

The emulator derives from [shadPS4](https://github.com/shadps4-emu/shadPS4),
[shadps4-arm64](https://github.com/zenithblue-oss/shadps4-arm64), and
[AstroQuest](https://github.com/bigmak94/AstroQuest).
Inherited changes from [evertec82/Farpoint](https://github.com/evertec82/Farpoint)
are recorded at revision `de6d8df1165aac8e09d90febfb4fb6a30384f48c`.
Packed render-target export work credits shadPS4 PR #5279 by ArtemkaVG.
See `docs/PROVENANCE.md` and the source headers for provenance and attribution.

No game package, extracted game, firmware or personal save is distributed.
