import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import test from "node:test";

const excludes = readFileSync(new URL("../../.release-exclude", import.meta.url), "utf8");

test("public-hardening evidence and experiments are private", () => {
  assert.match(excludes, /^reports\/public-hardening\/\*\*$/m);
  assert.match(excludes, /^private\/public-hardening\/\*\*$/m);
});
