import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const lock = JSON.parse(readFileSync(new URL("../locks/runtime-inputs.lock.json", import.meta.url), "utf8"));
const packageSource = readFileSync(new URL("../scripts/package-runtime.mjs", import.meta.url), "utf8");

test("managed runtime excludes deprecated Turnip archives and executables", () => {
  const lockedNames = lock.inputs.map(({ name }) => name);
  for (const archive of [
    "Turnip_v25.3.0_R11.zip",
    "turnip-25.0.0.tzst",
    "turnip-26.1.0.tzst",
  ]) {
    assert.ok(!lockedNames.includes(archive), `${archive} must not be a managed-runtime input`);
    assert.ok(!packageSource.includes(archive), `${archive} must not be packaged in the managed runtime`);
  }
  assert.doesNotMatch(packageSource, /drivers\/turnip-/);
  assert.doesNotMatch(packageSource, /vulkan\.ad07xx\.so/);
  assert.doesNotMatch(packageSource, /libvulkan_freedreno\.so/);
});
