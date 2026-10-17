import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

import {
  ARM64_MOV_X0_NEG_ENOSYS,
  ARM64_MOV_X8_CLONE3,
  ARM64_MOV_X8_FACCESSAT2,
  ARM64_SVC0,
  ARM64_SYSCALL_LOOKAHEAD,
  countArm64EnosysStubs,
  countArm64SyscallSites,
  patchArm64SyscallSites,
} from "../scripts/glibc-android-seccomp.mjs";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

const MOV_X20_X2 = 0xaa0203f4;
const STP_X21_X22 = 0xa90b5bf5;
const MOV_X19_X3 = 0xaa0303f3;
const LDR_X6_X4 = 0xf9400086;
const STR_X6_SP = 0xf90047e6;
const MOV_X6_0 = 0xd2800006;
const MOV_X6_X1 = 0xaa0103e6;
const CMN_X0 = 0xb140041f;

function pushU32(out, value) {
  const buf = Buffer.alloc(4);
  buf.writeUInt32LE(value >>> 0, 0);
  out.push(buf);
}

function concat(words) {
  const parts = [];
  for (const word of words) pushU32(parts, word);
  return Buffer.concat(parts);
}

// Debian glibc faccessat2 INLINE_SYSCALL: mov x8,#439, then 7 insns, then svc #0.
// Issue #17: consecutive-only patcher left this live → Android seccomp SIGSYS 159.
function debianFaccessat2Sequence() {
  return concat([
    ARM64_MOV_X8_FACCESSAT2,
    MOV_X20_X2,
    STP_X21_X22,
    MOV_X19_X3,
    LDR_X6_X4,
    STR_X6_SP,
    MOV_X6_0,
    MOV_X6_X1,
    ARM64_SVC0,
    CMN_X0,
  ]);
}

function consecutiveSyscallSequence(movOpcode) {
  return concat([movOpcode, ARM64_SVC0, CMN_X0]);
}

test("countArm64SyscallSites detects Debian glibc faccessat2 with insns between mov and svc", () => {
  const bytes = debianFaccessat2Sequence();
  assert.equal(countArm64SyscallSites(bytes, ARM64_MOV_X8_FACCESSAT2), 1);
  assert.equal(countArm64SyscallSites(bytes, ARM64_MOV_X8_CLONE3), 0);
});

test("countArm64SyscallSites still detects consecutive mov x8 / svc #0", () => {
  const bytes = consecutiveSyscallSequence(ARM64_MOV_X8_CLONE3);
  assert.equal(countArm64SyscallSites(bytes, ARM64_MOV_X8_CLONE3), 1);
});

test("patchArm64SyscallSites rewrites Debian faccessat2 svc to ENOSYS and leaves lookahead window intact", () => {
  const bytes = debianFaccessat2Sequence();
  const patched = patchArm64SyscallSites(bytes, ARM64_MOV_X8_FACCESSAT2);
  assert.equal(patched, 1);
  assert.equal(bytes.readUInt32LE(8 * 4), ARM64_MOV_X0_NEG_ENOSYS);
  assert.equal(bytes.readUInt32LE(0), ARM64_MOV_X8_FACCESSAT2);
  assert.equal(countArm64SyscallSites(bytes, ARM64_MOV_X8_FACCESSAT2), 0);
});

test("patchArm64SyscallSites rewrites consecutive clone3 svc to ENOSYS", () => {
  const bytes = consecutiveSyscallSequence(ARM64_MOV_X8_CLONE3);
  const patched = patchArm64SyscallSites(bytes, ARM64_MOV_X8_CLONE3);
  assert.equal(patched, 1);
  assert.equal(bytes.readUInt32LE(4), ARM64_MOV_X0_NEG_ENOSYS);
  assert.equal(countArm64SyscallSites(bytes, ARM64_MOV_X8_CLONE3), 0);
});

test("already-patched ENOSYS stubs are visible so package-runtime can re-run", () => {
  const bytes = debianFaccessat2Sequence();
  assert.equal(countArm64EnosysStubs(bytes, ARM64_MOV_X8_FACCESSAT2), 0);
  assert.equal(patchArm64SyscallSites(bytes, ARM64_MOV_X8_FACCESSAT2), 1);
  assert.equal(patchArm64SyscallSites(bytes, ARM64_MOV_X8_FACCESSAT2), 0);
  assert.equal(countArm64EnosysStubs(bytes, ARM64_MOV_X8_FACCESSAT2), 1);
  assert.equal(countArm64SyscallSites(bytes, ARM64_MOV_X8_FACCESSAT2), 0);
});

test("syscall lookahead covers the 32-byte Debian faccessat2 gap", () => {
  assert.equal(ARM64_SYSCALL_LOOKAHEAD, 32);
  const svcAt = 8 * 4;
  assert.ok(svcAt <= ARM64_SYSCALL_LOOKAHEAD);
});

test("package-runtime and verify-runtime use the shared lookahead patcher and fail if faccessat2 is unpatched", () => {
  const pack = read("runtime/scripts/package-runtime.mjs");
  const verify = read("runtime/tests/verify-runtime.mjs");
  assert.match(pack, /from "\.\/glibc-android-seccomp\.mjs"/);
  assert.match(pack, /patchArm64SyscallSites/);
  assert.match(pack, /ARM64_MOV_X8_FACCESSAT2/);
  assert.match(pack, /fail\(`Host libc \${name} sites patched=/);
  assert.match(verify, /from "\.\.\/scripts\/glibc-android-seccomp\.mjs"/);
  assert.match(verify, /countArm64SyscallSites\(hostLibc, ARM64_MOV_X8_FACCESSAT2\)/);
  assert.doesNotMatch(
    pack,
    /bytes\.readUInt32LE\(offset \+ 4\) !== 0xd4000001/,
    "consecutive-only faccessat2 matcher must not remain in package-runtime",
  );
});

test("SIGSYS trap emulates host faccessat2 blocked by Android seccomp", () => {
  const main = read("src/main.cpp");
  assert.match(main, /SYS_SECCOMP/);
  assert.match(main, /SYS_faccessat2|439/);
  assert.match(main, /SYS_faccessat/);
  assert.match(main, /uc_mcontext\.pc \+= 4/);
  assert.match(main, /regs\[0\] = /);
  assert.match(
    main,
    /have_guest/,
    "must not swallow FEX-generated SIGSYS while a guest thread is active",
  );
  assert.match(main, /BachataEmulateSeccompFaccessat2/);
  const handler = main.slice(main.indexOf("static void BachataSigsysHandler"));
  assert.match(handler, /if \(!have_guest && BachataEmulateSeccompFaccessat2\(/);
  assert.match(handler, /return;/);
});
