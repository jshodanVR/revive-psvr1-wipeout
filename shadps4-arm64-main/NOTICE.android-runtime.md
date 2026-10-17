# Android Runtime Third-Party Notice

## Bachata X Server (embedded X11/ALSA/SHM server)

- Upstream: https://github.com/JICA98/bachata-xserver.git
- Revision: `80e65f396e20efe819e499be225b94c445bfda34`
- License: LGPL-2.1 (`LICENSES/BachataXServer-LGPL-2.1.txt`)
- Consumed as: Git submodule `externals/bachata-xserver`; Java sources compiled into the
  app from `externals/bachata-xserver/java`, native helpers built as
  `libbachata_xserver.so` from `externals/bachata-xserver/cpp/xserver`.
- Origin/provenance: seeded byte-exact from the Bachata S4 runtime fork of
  brunodev85/winlator-app at `72ec347c9ced676e206fbc3762b9d567852cb3e3` including the
  Bachata S4 fixes of that era (abstract X11 sockets, keymap query, GPU image unlock,
  SYNC_FD wait handling), then renamed to the `org.bachatas4.xserver` namespace; see the
  submodule's `UPSTREAM.md`.

## libadrenotools

- Upstream: https://github.com/JICA98/libadrenotools.git (vendored copy of bylaws/libadrenotools)
- Revision: `7db3328e8d5e4762bcdf91c0279d32a52223899e`
- License: BSD-2-Clause (`LICENSE` in the submodule)
- Used by the custom Vulkan driver installer (`bachata_custom_vulkan`).

## Vortek

- Client upstream: https://github.com/JICA98/vortek.git (Bachata S4 runtime fork of brunodev85/vortek)
- Client revision: `9325b6060fc1c690234e102fcbbb1e0283b8892e`
- Client license: LGPL-2.1
- Client source destination: `runtime/sources/vortek-client`
- Client is built from source into the managed runtime (`host/lib/libvulkan_vortek.so`); no prebuilt asset is redistributed.
- Server upstream: https://github.com/JICA98/vortek.git, branch `bachata-server`
- Server revision: `df8183df5c2b024f116a5f796a9e4147aa696cd0`
- Server source location: `runtime/sources/vortek-server` (re-homed; originally from the
  winlator-app tree at `72ec347c9ced676e206fbc3762b9d567852cb3e3`, see branch `UPSTREAM.md`)
- Server is also LGPL-2.1 and is built from source.
- Protocol headers `request_codes.h` and `vortek_serializer.h` are verified byte-identical between the pinned client and server via `runtime/scripts/vendor-vortek.sh`.
- Shared Bachata handshake definitions live in `runtime/vortek-protocol/bachata_vortek_protocol.h`.
- Android server: `libbachata_vortek_server.so` under `android/BachataS4/core/runtime/src/main/cpp/vortek/`,
  ring/ashmem helpers from the pinned bachata-xserver submodule, host Vulkan via `dlopen("libvulkan.so")`.

## Runtime Components

- shadPS4 backend: GPL-2.0-or-later; corresponding source is this repository, including Bachata runtime changes.
- GNU glibc: LGPL-2.1-or-later. Unmodified locked packages are listed in `runtime/locks/runtime-inputs.lock.json`; package-time Android seccomp compatibility edits are reproducible in `runtime/scripts/package-runtime.mjs`. The Android SysV-shm compatibility patches are kept under `runtime/patches/glibc-android-sysvshm/` with pinned SHA-256 values.
- Mesa/Turnip and Vulkan loader: MIT-family licenses; revisions and package hashes are recorded in runtime locks.
- SDL2/SDL3, X11 libraries, libudev, libuuid, libstdc++, libgcc, zlib, libdrm, and CA certificates: redistributed under their respective upstream licenses with exact package hashes in runtime locks.

For GPL/LGPL components, corresponding source and build scripts are provided in this repository. A written source offer is available with distributed binaries for at least three years where required by the applicable license.
