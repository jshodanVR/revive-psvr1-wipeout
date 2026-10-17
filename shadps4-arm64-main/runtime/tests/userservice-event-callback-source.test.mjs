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

test("sceUserServiceRegisterEventCallback stores a guest callback and replays queued login", () => {
  const header = read("src/core/libraries/system/userservice.h");
  const impl = read("src/core/libraries/system/userservice.cpp");

  assert.match(
    header,
    /using OrbisUserServiceEventCallback\s*=\s*void\s+PS4_SYSV_ABI\s*\(\*\)\(OrbisUserServiceEvent\*\)/,
    "callback typedef must take OrbisUserServiceEvent*",
  );
  assert.match(
    header,
    /sceUserServiceRegisterEventCallback\(OrbisUserServiceEventCallback/,
    "RegisterEventCallback must accept the callback pointer; 0-arg stub drops rdi",
  );
  assert.match(
    header,
    /sceUserServiceUnregisterEventCallback\(OrbisUserServiceEventCallback/,
  );

  const registerFn = section(
    impl,
    "int PS4_SYSV_ABI sceUserServiceRegisterEventCallback(",
    "int PS4_SYSV_ABI sceUserServiceSetAccessibilityKeyremapData()",
  );
  assert.doesNotMatch(
    registerFn,
    /\(STUBBED\) called/,
    "SnowRunner waits on this callback for login before scePadOpen",
  );
  assert.match(registerFn, /g_event_callback\s*=/);
  assert.match(
    registerFn,
    /user_service_event_queue/,
    "register after bootstrap LoginUser must replay the queued Login event",
  );
  assert.match(registerFn, /InvokeUserServiceEventCallback/);

  const addFn = section(
    impl,
    "void AddUserServiceEvent(const OrbisUserServiceEvent e)",
    "s32 PS4_SYSV_ABI sceUserServiceGetEvent",
  );
  assert.match(addFn, /InvokeUserServiceEventCallback/);

  assert.match(impl, /#include "core\/guest_cpu\/guest_callback\.h"/);
  assert.match(
    impl,
    /RunGuestFunctionOrAbort\([\s\S]*sceUserServiceEventCallback/,
    "FEX must invoke the guest callback through the existing bridge",
  );
});

test("sceUserServiceGetForegroundUser writes the logged-in player-one user id", () => {
  const header = read("src/core/libraries/system/userservice.h");
  const impl = read("src/core/libraries/system/userservice.cpp");
  assert.match(
    header,
    /sceUserServiceGetForegroundUser\(OrbisUserServiceUserId\*\s*userId\)/,
  );
  const fn = section(
    impl,
    "s32 PS4_SYSV_ABI sceUserServiceGetForegroundUser(",
    "int PS4_SYSV_ABI sceUserServiceGetFriendCustomListLastFocus()",
  );
  assert.doesNotMatch(fn, /\(STUBBED\) called/);
  assert.match(fn, /GetDefaultUser\(\)\.user_id/);
  assert.match(fn, /\*userId\s*=/);
});

test("scePadGetHandle opens a pad when the guest never called scePadOpen", () => {
  const pad = read("src/core/libraries/pad/pad.cpp");
  const fn = section(
    pad,
    "int PS4_SYSV_ABI scePadGetHandle(",
    "int PS4_SYSV_ABI scePadGetIdleCount()",
  );
  assert.match(
    fn,
    /scePadOpen\(/,
    "SnowRunner never calls scePadOpen; GetHandle must create the handle",
  );
});

test("Bachata bootstrap connects a virtual pad after user login", () => {
  const main = read("src/main.cpp");
  const bachata = section(
    main,
    "// Desktop input discovery logs in player one",
    "// Initialize key manager",
  );
  assert.match(bachata, /LoginUser\(UserManagement\.GetUserByPlayerIndex\(1\),\s*1\)/);
  assert.match(
    bachata,
    /ConnectController\(nullptr\)/,
    "Android skips TryOpenSDLControllers, so player-one pad stays disconnected until overlay input",
  );
});
