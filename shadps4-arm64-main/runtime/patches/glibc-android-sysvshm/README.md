# glibc Android SysV-shm compatibility patches

Source-level patches for the guest glibc so POSIX SysV shared-memory calls
(`shmat`, `shmctl`, `shmdt`, `shmget`) work on Android, which lacks kernel SysV IPC.
These back the X11 MIT-SHM extension served by the embedded Bachata X server.

## Provenance

- Origin: https://github.com/brunodev85/winlator.git at
  `fb66541b93a4eb3ee585a433b4c7b20544d58e40`, directory `glibc_patches/sysdeps/unix/sysv/linux/`
  (MIT license, per upstream repository).
- Files are byte-identical to the origin; SHA-256 values are pinned in
  `runtime/locks/runtime-inputs.lock.json` under `glibcSysVshmPatches` and enforced by
  `runtime/tests/verify-runtime.mjs`.
