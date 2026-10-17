#!/usr/bin/env node

import { createHash } from "node:crypto";
import { existsSync, readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import {
  ARM64_MOV_X8_CLONE3,
  ARM64_MOV_X8_FACCESSAT2,
  countArm64SyscallSites,
} from "../scripts/glibc-android-seccomp.mjs";

const EXPECTED_COMPONENTS = [
  { name: "bachata-xserver", url: "https://github.com/JICA98/bachata-xserver.git", revision: "80e65f396e20efe819e499be225b94c445bfda34", license: "LGPL-2.1" },
  { name: "libadrenotools", url: "https://github.com/JICA98/libadrenotools.git", revision: "7db3328e8d5e4762bcdf91c0279d32a52223899e", license: "BSD-2-Clause" },
  { name: "glibc-packages", url: "https://github.com/termux-pacman/glibc-packages.git", revision: "26d89ba7a1f856b99f0d437bef54f558b2485075", license: "mixed" },
  { name: "mesa", url: "https://gitlab.freedesktop.org/mesa/mesa.git", revision: "6984e91b5fe1d1c204e54954a4282fcdc0c44b78", license: "MIT" },
  {
    name: "fex",
    url: "https://github.com/FEX-Emu/FEX.git",
    revision: "f2b679f6028ce1c38875233aecfcf5d3f8ebecec",
    license: "MIT",
  },
  {
    name: "vortek-client",
    url: "https://github.com/JICA98/vortek.git",
    revision: "9325b6060fc1c690234e102fcbbb1e0283b8892e",
    license: "LGPL-2.1",
    sourceDestination: "runtime/sources/vortek-client",
    buildOutput: "host/lib/libvulkan_vortek.so",
  },
  {
    name: "vortek-server",
    url: "https://github.com/JICA98/vortek.git",
    revision: "df8183df5c2b024f116a5f796a9e4147aa696cd0",
    license: "LGPL-2.1",
    sourceDestination: "runtime/sources/vortek-server",
    sourcedFrom: "bachata-server branch",
  },
];
const FEX_REVISION = EXPECTED_COMPONENTS.find(({ name }) => name === "fex").revision;
const EXPECTED_INPUTS = [
  { name: "glibc-2.43+r22+g8362e8ce10b2-2-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/glibc-2.43+r22+g8362e8ce10b2-2-aarch64.pkg.tar.xz", sha256: "8fac217e98c6e4342326726b2640ac254e8c82032f06f30bfa13ebbcc4fcb25b" },
  { name: "cacert-2025-02-25.pem", url: "https://curl.se/ca/cacert-2025-02-25.pem", sha256: "50a6277ec69113f00c5fd45f09e8b97a4b3e32daa35d3a95ab30137a55386cef" },
  { name: "libxss-1.2.5-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/libxss-1.2.5-1-aarch64.pkg.tar.xz", sha256: "1ebc34a29420166cb25040bfbff5d6ff732eff92de9cf07fcd92835e7d68bb9c" },
  { name: "libxkbcommon-1.13.2-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/libxkbcommon-1.13.2-1-aarch64.pkg.tar.xz", sha256: "76b922d87d0af0011072b156464041ec5674f24ff1c78b7e85132fda72c9a7e7" },
  { name: "dbus-1.16.2-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/dbus-1.16.2-1-aarch64.pkg.tar.xz", sha256: "1aa0bc2be4fa083ec0cb678f05223f55b0c8e498351ad781dc4b7a0987b62bcc" },
  { name: "systemd-libs-261-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/systemd-libs-261-1-aarch64.pkg.tar.xz", sha256: "7e7c4c8c169caa36716c67868a1430cf0d346a7653588d4286f91621913ec452" },
  { name: "vulkan-icd-loader-1.4.350.1-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/vulkan-icd-loader-1.4.350.1-1-aarch64.pkg.tar.xz", sha256: "31142fb87d8c76233e35afc715afa17bcf89e4fe544dbb4d59c0b8e950640c3d" },
  { name: "libstdc++-16.1.1+r12+g301eb08fa2c5-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/libstdc%2B%2B-16.1.1%2Br12%2Bg301eb08fa2c5-1-aarch64.pkg.tar.xz", sha256: "19832a38b2c4820695d28289f1c4f371955586d39fb893d8cbc0d8dbb09a4383" },
  { name: "zlib-1:1.3.2-3-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/core/zlib-1%3A1.3.2-3-aarch64.pkg.tar.xz", sha256: "7e31b465e09e8e61375578b8e26ed883906e126e0c0ef1c75b7ccccfa95a58c4" },
  { name: "libdrm-2.4.134-1-aarch64.pkg.tar.xz", url: "https://de3.mirror.archlinuxarm.org/aarch64/extra/libdrm-2.4.134-1-aarch64.pkg.tar.xz", sha256: "d4173d7adc60d32d389e44e976447962c31bc23ff9a2153539bd8ad766e307b8" }
];
const EXPECTED_GLIBC_SYSVSHM_PATCHES = [
  { file: "android_sysvshm.c", sha256: "3698e8e9cc8e00790f60ec16fbf88c88a78da933740e003677a6c37e509c71c2" },
  { file: "android_sysvshm.h", sha256: "031fa071ca44d1191dc22b4adbf351191bb292d718af0afd2caa9291aabf6550" },
  { file: "shmat.c", sha256: "e6b80913003e80ef4f900322398b682a76c1806dbd2f2da2a8f58a0759897f66" },
  { file: "shmctl.c", sha256: "fe72ee45e0c4cd6215f109c358bdbbfbeef22a6922ea0f91758226a671bc7d4b" },
  { file: "shmdt.c", sha256: "4e4362296e2e572e36e48617dce286639b3d4c2998afc7b8737a201b1e91069e" },
  { file: "shmget.c", sha256: "e59d1b5574e05cd6683202a6e0f61addea2eccca61ebad66682703a9cd477299" },
];
const REQUIRED_RUNTIME_PATHS = [
  "bin/probes/vortek_probe_aarch64",
  "etc/ssl/certs/ca-certificates.crt",
  "host/fexcore-smoke",
  "host/fexcore-guest-harness",
  "host/shadps4-arm64-fex",
  "host/ld-linux-aarch64.so.1",
  "host/libvulkan.so.1",
  "host/lib/libvulkan_vortek.so",
  "host/vulkan/icd.d/vortek.json",
  "usr/share/bachata/vortek/LICENSE",
  "usr/share/bachata/vortek/SOURCE.txt",
  "usr/share/bachata/guest-runtime.json",
  "usr/share/bachata/guest-runtime.txt",
  "host/libc.so.6",
  "host/libdl.so.2",
  "host/libgcc_s.so.1",
  "host/libm.so.6",
  "host/libpthread.so.0",
  "host/libresolv.so.2",
  "host/libX11.so",
  "host/libX11.so.6",
  "host/libX11-xcb.so.1",
  "host/libXcursor.so.1",
  "host/libXext.so.6",
  "host/libXfixes.so.3",
  "host/libXi.so.6",
  "host/libXrandr.so.2",
  "host/libXss.so.1",
  "host/libxkbcommon.so.0",
  "host/libdbus-1.so.3",
  "host/libsystemd.so.0",
  "host/libXrender.so.1",
  "host/libXau.so.6",
  "host/libXdmcp.so.6",
  "host/libxcb.so",
  "host/libxcb.so.1",
  "host/libcap.so.2",
  "usr/share/bachata/shadps4-arm64-fex-needed.txt",
];

function fail(message) {
  throw new Error(message);
}

function sha256(bytes) {
  return createHash("sha256").update(bytes).digest("hex");
}

function countArm64SetRobustListCalls(bytes) {
  let count = 0;
  for (let offset = 0; offset + 4 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) !== 0xd2800c68) continue;
    for (let syscallOffset = offset + 4; syscallOffset <= offset + 32; syscallOffset += 4) {
      if (bytes.readUInt32LE(syscallOffset) === 0xd4000001) {
        count++;
        break;
      }
    }
  }
  return count;
}

function parseStoredZip(bytes) {
  const eocdSignature = 0x06054b50;
  let eocd = -1;
  for (let offset = bytes.length - 22; offset >= Math.max(0, bytes.length - 65_557); offset--) {
    if (bytes.readUInt32LE(offset) === eocdSignature) {
      eocd = offset;
      break;
    }
  }
  if (eocd < 0) fail("ZIP end record missing");
  const entryCount = bytes.readUInt16LE(eocd + 10);
  const centralOffset = bytes.readUInt32LE(eocd + 16);
  const entries = [];
  let offset = centralOffset;
  for (let index = 0; index < entryCount; index++) {
    if (bytes.readUInt32LE(offset) !== 0x02014b50) fail("Invalid ZIP central directory");
    const method = bytes.readUInt16LE(offset + 10);
    const time = bytes.readUInt16LE(offset + 12);
    const date = bytes.readUInt16LE(offset + 14);
    const compressedSize = bytes.readUInt32LE(offset + 20);
    const size = bytes.readUInt32LE(offset + 24);
    const nameLength = bytes.readUInt16LE(offset + 28);
    const extraLength = bytes.readUInt16LE(offset + 30);
    const commentLength = bytes.readUInt16LE(offset + 32);
    const localOffset = bytes.readUInt32LE(offset + 42);
    const path = bytes.subarray(offset + 46, offset + 46 + nameLength).toString("utf8");
    if (method !== 0 || compressedSize !== size) fail(`ZIP entry must be stored: ${path}`);
    if (time !== 0 || date !== 0) fail(`ZIP timestamp is not zero: ${path}`);
    if (bytes.readUInt32LE(localOffset) !== 0x04034b50) fail(`Invalid local header: ${path}`);
    const localNameLength = bytes.readUInt16LE(localOffset + 26);
    const localExtraLength = bytes.readUInt16LE(localOffset + 28);
    const dataOffset = localOffset + 30 + localNameLength + localExtraLength;
    entries.push({ path, bytes: bytes.subarray(dataOffset, dataOffset + size) });
    offset += 46 + nameLength + extraLength + commentLength;
  }
  return entries;
}

const scriptDir = dirname(fileURLToPath(import.meta.url));
const projectRoot = resolve(scriptDir, "../..");
const locksOnly = process.argv[2] === "--locks-only";
const lockPath = resolve(locksOnly ? resolve(projectRoot, "runtime/locks/components.lock.json") : (process.argv[2] ?? resolve(projectRoot, "runtime/locks/components.lock.json")));
const inputLockPath = resolve(projectRoot, "runtime/locks/runtime-inputs.lock.json");
const zipPath = resolve(process.argv[3] ?? resolve(projectRoot, "android/BachataS4/app/src/main/assets/runtime/runtime.zip"));
const manifestPath = resolve(process.argv[4] ?? resolve(projectRoot, "android/BachataS4/app/src/main/assets/runtime/manifest.json"));
const xServerSourcePath = resolve(projectRoot, "externals/bachata-xserver/java/org/bachatas4/xserver/xserver/XServer.java");
const nativeHostLoaderPath = resolve(projectRoot, "android/BachataS4/core/runtime/src/main/jniLibs/arm64-v8a/libbachata_host_loader.so");
const playStoreRuntimeDir = resolve(projectRoot, "android/BachataS4/app/src/playstore/assets/runtime");

if (existsSync(playStoreRuntimeDir)) {
  fail("Play Store flavor must use the generated main runtime assets, not a stale flavor override");
}

const lock = JSON.parse(readFileSync(lockPath, "utf8"));
if (readFileSync(xServerSourcePath, "utf8").includes("GLXExtension")) {
  fail("Bachata X server enables GLX without the gladiorenderer native library");
}
if (lock.schemaVersion !== 1) fail("Lock schemaVersion must be 1");
if (JSON.stringify(lock.components) !== JSON.stringify(EXPECTED_COMPONENTS)) fail("Locked components differ from approved upstreams");
for (const component of lock.components) {
  if (!/^[0-9a-f]{40}$/.test(component.revision)) fail(`Invalid revision: ${component.name}`);
  if (!/^https:\/\//.test(component.url)) fail(`Invalid URL: ${component.name}`);
  if (!component.license) fail(`Missing license: ${component.name}`);
}
const inputLock = JSON.parse(readFileSync(inputLockPath, "utf8"));
if (inputLock.schemaVersion !== 1) fail("Input lock schemaVersion must be 1");
if (JSON.stringify(inputLock.inputs) !== JSON.stringify(EXPECTED_INPUTS)) fail("Runtime inputs differ from approved artifacts");
{
  const shmPatches = inputLock.glibcSysVshmPatches;
  if (!shmPatches || !Array.isArray(shmPatches.files)) fail("Input lock missing glibcSysVshmPatches.files");
  for (const expected of EXPECTED_GLIBC_SYSVSHM_PATCHES) {
    const entry = shmPatches.files.find(({ path }) => path === `runtime/patches/glibc-android-sysvshm/${expected.file}`);
    if (!entry) fail(`glibcSysVshmPatches missing ${expected.file}`);
    if (entry.sha256 !== expected.sha256) fail(`glibc sysvshm patch hash differs from approved revision: ${expected.file}`);
    const actual = resolve(projectRoot, `runtime/patches/glibc-android-sysvshm/${expected.file}`);
    if (!existsSync(actual)) fail(`Missing in-repo glibc patch: ${expected.file}`);
    if (sha256(readFileSync(actual)) !== expected.sha256) fail(`In-repo glibc patch hash mismatch: ${expected.file}`);
  }
}
if (locksOnly) {
  console.log(`runtime locks verified: components=${lock.components.length} inputs=${inputLock.inputs.length}`);
  process.exit(0);
}

const manifest = JSON.parse(readFileSync(manifestPath, "utf8"));
if (manifest.schemaVersion !== 2 || manifest.protocolVersion !== 1 || manifest.distribution !== "debian") {
  fail("Invalid Debian runtime manifest schema/protocol");
}
if (!manifest.runtimeVersion) fail("Missing runtimeVersion");
const componentProvenance = lock.components.map(({ name, revision }) => ({ name, revision }));
if (JSON.stringify(manifest.components) !== JSON.stringify(componentProvenance)) fail("Manifest component provenance mismatch");
const zipEntries = parseStoredZip(readFileSync(zipPath));
const paths = zipEntries.map((entry) => entry.path);
if (new Set(paths).size !== paths.length) fail("Duplicate ZIP paths");
if (JSON.stringify(paths) !== JSON.stringify([...paths].sort())) fail("ZIP paths are not lexical");
for (const required of REQUIRED_RUNTIME_PATHS) {
  if (!paths.includes(required)) fail(`Required runtime file missing: ${required}`);
}
if (!Array.isArray(manifest.files) || manifest.files.length !== zipEntries.length) fail("Manifest file count mismatch");
for (let index = 0; index < zipEntries.length; index++) {
  const entry = zipEntries[index];
  const declared = manifest.files[index];
  if (declared.path !== entry.path) fail(`Manifest order/path mismatch: ${entry.path}`);
  if (declared.size !== entry.bytes.length) fail(`Manifest size mismatch: ${entry.path}`);
  if (declared.sha256 !== sha256(entry.bytes)) fail(`Manifest SHA-256 mismatch: ${entry.path}`);
}
const hostLoader = zipEntries.find((entry) => entry.path === "host/ld-linux-aarch64.so.1").bytes;
const hostLibc = zipEntries.find((entry) => entry.path === "host/libc.so.6").bytes;
const hostFexcoreSmoke = zipEntries.find((entry) => entry.path === "host/fexcore-smoke").bytes;
const hostFexcoreGuestHarness = zipEntries.find((entry) => entry.path === "host/fexcore-guest-harness").bytes;
const hostFexShadPs4 = zipEntries.find((entry) => entry.path === "host/shadps4-arm64-fex").bytes;
if (countArm64SetRobustListCalls(hostLoader) !== 0 || countArm64SetRobustListCalls(hostLibc) !== 0) {
  fail("Host glibc retains set_robust_list calls blocked by Android app seccomp");
}
if (countArm64SyscallSites(hostLibc, ARM64_MOV_X8_CLONE3) !== 0) fail("Host glibc retains clone3 calls blocked by Android app seccomp");
if (countArm64SyscallSites(hostLibc, ARM64_MOV_X8_FACCESSAT2) !== 0) fail("Host glibc retains faccessat2 calls blocked by Android app seccomp");
if (hostFexcoreSmoke.length < 20 || hostFexcoreSmoke[0] !== 0x7f || hostFexcoreSmoke.subarray(1, 4).toString() !== "ELF") {
  fail("FEXCore smoke runner is not ELF");
}
if (hostFexcoreSmoke.readUInt16LE(18) !== 183) fail("FEXCore smoke runner is not AArch64 ELF");
for (const marker of [FEX_REVISION, "gpr=ok", "stack=ok", "fp=ok"]) {
  if (!hostFexcoreSmoke.includes(Buffer.from(marker))) fail(`FEXCore smoke runner lacks ${marker}`);
}
if (hostFexcoreGuestHarness.length < 20 || hostFexcoreGuestHarness[0] !== 0x7f || hostFexcoreGuestHarness.subarray(1, 4).toString() !== "ELF") {
  fail("FEXCore guest harness is not ELF");
}
if (hostFexcoreGuestHarness.readUInt16LE(18) !== 183) fail("FEXCore guest harness is not AArch64 ELF");
if (hostFexShadPs4.length < 20 || hostFexShadPs4[0] !== 0x7f ||
    hostFexShadPs4.subarray(1, 4).toString() !== "ELF" || hostFexShadPs4.readUInt16LE(18) !== 183) {
  fail("FEX shadPS4 is not AArch64 ELF");
}
{
  const guestMetaEntry = zipEntries.find((entry) => entry.path === "usr/share/bachata/guest-runtime.json");
  if (!guestMetaEntry) fail("Missing source-built guest runtime metadata");
  const guestMeta = JSON.parse(guestMetaEntry.bytes.toString("utf8"));
  const guestSha = sha256(hostFexShadPs4);
  if (guestMeta.variant !== "built" || guestMeta.revision !== "workspace"
      || guestMeta.label !== "workspace-build" || guestMeta.sha256 !== guestSha) {
    fail("Source-built guest runtime metadata does not match packaged guest");
  }
  console.log(
    `GUEST_RUNTIME_BUILD variant=${guestMeta.variant} sha256=${guestMeta.sha256} revision=${guestMeta.revision}`,
  );
}
for (const marker of [
  FEX_REVISION,
  "FEXCORE_GUEST_ENGINE_OK",
  "bridge=ok",
  "teardown=ok",
  "FEXCORE_GUEST_CPU_OK",
  "caller_mapping=ok",
  "thread_lifetime=ok",
  "thread_isolation=ok",
  "overlap_rejected=ok",
]) {
  if (!hostFexcoreGuestHarness.includes(Buffer.from(marker))) fail(`FEXCore guest harness lacks ${marker}`);
}
if (sha256(readFileSync(nativeHostLoaderPath)) !== sha256(hostLoader)) fail("Native host loader differs from runtime host loader");
// Task 4: Validate that none of the four binaries contains clean-build checkout path
const checkPaths = [projectRoot];
if (process.env.REPRO_CHECKOUT_PATH) checkPaths.push(process.env.REPRO_CHECKOUT_PATH);
const fourBinaries = [
  { path: "host/shadps4-arm64-fex", bytes: hostFexShadPs4 },
  { path: "host/fexcore-guest-harness", bytes: hostFexcoreGuestHarness },
  { path: "host/fexcore-smoke", bytes: hostFexcoreSmoke },
];
for (const b of fourBinaries) {
  for (const cPath of checkPaths) {
    if (b.bytes.includes(Buffer.from(cPath))) {
      fail(`Binary ${b.path} contains checkout path: ${cPath}`);
    }
  }
}

console.log(`runtime verified: ${zipEntries.length} files, sha256=${sha256(readFileSync(zipPath))}`);
