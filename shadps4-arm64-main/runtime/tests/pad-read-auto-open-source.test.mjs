import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");
const section = (source, startMarker, endMarker) => {
  const start = source.indexOf(startMarker);
  assert.notEqual(start, -1, `missing start marker: ${startMarker}`);
  const end = source.indexOf(endMarker, start);
  assert.notEqual(end, -1, `missing end marker: ${endMarker}`);
  return source.slice(start, end);
};

const pad = () => read("src/core/libraries/pad/pad.cpp");
const userservice = () => read("src/core/libraries/system/userservice.cpp");

test("scePadReadState auto-opens a missing handle instead of silent INVALID_HANDLE", () => {
  const impl = pad();
  assert.match(
    impl,
    /EnsurePadHandle\s*\(/,
    "SnowRunner title polls ReadState without scePadOpen; missing handles must auto-open",
  );
  const fn = section(
    impl,
    "int PS4_SYSV_ABI scePadReadState(",
    "int PS4_SYSV_ABI scePadReadStateExt()",
  );
  assert.match(fn, /EnsurePadHandle\s*\(\s*handle\s*\)/);
  assert.match(fn, /LOG_INFO\(Lib_Pad/);
});

test("scePadRead auto-opens a missing handle", () => {
  const fn = section(
    pad(),
    "int PS4_SYSV_ABI scePadRead(",
    "int PS4_SYSV_ABI scePadReadBlasterForTracker()",
  );
  assert.match(fn, /EnsurePadHandle\s*\(\s*handle\s*\)/);
});

test("scePadGetControllerInformation auto-opens a missing handle", () => {
  const fn = section(
    pad(),
    "int PS4_SYSV_ABI scePadGetControllerInformation(",
    "int PS4_SYSV_ABI scePadGetDataInternal()",
  );
  assert.match(fn, /EnsurePadHandle\s*\(\s*handle\s*\)/);
});

test("GetLoginUserIdList and GetInitialUser log at INFO so title input can be traced", () => {
  const impl = userservice();
  const login = section(
    impl,
    "s32 PS4_SYSV_ABI sceUserServiceGetLoginUserIdList(",
    "int PS4_SYSV_ABI sceUserServiceGetMicLevel()",
  );
  assert.match(login, /LOG_INFO\(Lib_UserService/);
  const initial = section(
    impl,
    "s32 PS4_SYSV_ABI sceUserServiceGetInitialUser(",
    "int PS4_SYSV_ABI sceUserServiceGetIPDLeft()",
  );
  assert.match(initial, /LOG_INFO\(Lib_UserService/);
});
