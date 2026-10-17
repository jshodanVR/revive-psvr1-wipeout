import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("persistent IRQ Register replaces an existing uid instead of asserting", () => {
  const platform = read("src/core/platform.h");
  const registerStart = platform.indexOf("void Register(InterruptId irq, IrqHandler handler, void* uid)");
  const registerEnd = platform.indexOf("void Unregister(InterruptId irq, void* uid)");
  assert.notEqual(registerStart, -1, "Register start marker found");
  assert.notEqual(registerEnd, -1, "Unregister start marker found");
  assert.ok(registerStart < registerEnd, "Register precedes Unregister");
  const registerFn = platform.slice(registerStart, registerEnd);

  assert.doesNotMatch(
    registerFn,
    /The handler is already registered!/,
    "re-registering the same uid must not assert; guest titles re-add GPU eq events",
  );
  assert.match(
    registerFn,
    /persistent_handlers\[uid\]\s*=/,
    "same uid must replace the previous persistent handler",
  );
});
