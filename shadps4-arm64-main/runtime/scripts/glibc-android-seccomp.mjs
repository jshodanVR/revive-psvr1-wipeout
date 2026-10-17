export const ARM64_SVC0 = 0xd4000001;
export const ARM64_MOV_X8_CLONE3 = 0xd2803668;
export const ARM64_MOV_X8_FACCESSAT2 = 0xd28036e8;
export const ARM64_MOV_X0_NEG_ENOSYS = 0x928004a0;
// Debian glibc faccessat2 places svc 32 bytes after `mov x8, #439`.
export const ARM64_SYSCALL_LOOKAHEAD = 32;

export function countArm64SyscallSites(bytes, movOpcode, lookahead = ARM64_SYSCALL_LOOKAHEAD) {
  let count = 0;
  for (let offset = 0; offset + 4 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) !== movOpcode) continue;
    for (let so = offset + 4; so <= offset + lookahead && so + 4 <= bytes.length; so += 4) {
      if (bytes.readUInt32LE(so) !== ARM64_SVC0) continue;
      count++;
      break;
    }
  }
  return count;
}

export function patchArm64SyscallSites(bytes, movOpcode, lookahead = ARM64_SYSCALL_LOOKAHEAD) {
  let patched = 0;
  for (let offset = 0; offset + 4 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) !== movOpcode) continue;
    for (let so = offset + 4; so <= offset + lookahead && so + 4 <= bytes.length; so += 4) {
      if (bytes.readUInt32LE(so) !== ARM64_SVC0) continue;
      bytes.writeUInt32LE(ARM64_MOV_X0_NEG_ENOSYS, so);
      patched++;
      break;
    }
  }
  return patched;
}

export function countArm64EnosysStubs(bytes, movOpcode, lookahead = ARM64_SYSCALL_LOOKAHEAD) {
  let count = 0;
  for (let offset = 0; offset + 4 <= bytes.length; offset += 4) {
    if (bytes.readUInt32LE(offset) !== movOpcode) continue;
    for (let so = offset + 4; so <= offset + lookahead && so + 4 <= bytes.length; so += 4) {
      if (bytes.readUInt32LE(so) !== ARM64_MOV_X0_NEG_ENOSYS) continue;
      count++;
      break;
    }
  }
  return count;
}
