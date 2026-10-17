import assert from "node:assert/strict";
import { execFileSync } from "node:child_process";
import { mkdtempSync, readFileSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("SelectClipCullArrays keeps clip and drops cull when combined limit is 8", () => {
  const dir = mkdtempSync(join(tmpdir(), "bachata-clip-cull-"));
  const src = join(dir, "probe.cpp");
  const bin = join(dir, "probe");
  writeFileSync(
    src,
    `#include "shader_recompiler/clip_cull.h"
#include <cstdio>
int main() {
  using Shader::SelectClipCullArrays;
  using Shader::AttributeUsedCount;
  auto a = SelectClipCullArrays(8, 8, 8, 8, 8, true, true);
  std::printf("%u %u\\n", a.clip_size, a.cull_size);
  auto b = SelectClipCullArrays(4, 4, 8, 8, 8, true, true);
  std::printf("%u %u\\n", b.clip_size, b.cull_size);
  auto c = SelectClipCullArrays(8, 8, 8, 8, 8, false, true);
  std::printf("%u %u\\n", c.clip_size, c.cull_size);
  auto d = SelectClipCullArrays(4, 4, 8, 8, 8, true, false);
  std::printf("%u %u\\n", d.clip_size, d.cull_size);
  std::printf("%u %u\\n", AttributeUsedCount(0x0f), AttributeUsedCount(0x80));
}
`,
  );
  execFileSync("g++", ["-std=c++20", `-I${join(root, "src")}`, "-o", bin, src]);
  const out = execFileSync(bin, { encoding: "utf8" });
  assert.equal(out, "8 0\n4 4\n0 8\n4 0\n4 8\n");
});

test("VS clip/cull arrays use device limits instead of hardcoded 8+8", () => {
  const emit = read("src/shader_recompiler/backend/spirv/spirv_emit_context.cpp");
  const defineStart = emit.indexOf("void EmitContext::DefineVertexBlock()");
  const defineEnd = emit.indexOf("void EmitContext::DefineOutputs()");
  assert.notEqual(defineStart, -1, "DefineVertexBlock found");
  assert.notEqual(defineEnd, -1, "DefineOutputs found");
  const block = emit.slice(defineStart, defineEnd);

  assert.match(block, /SelectClipCullArrays/, "must size clip/cull arrays from device limits");
  assert.doesNotMatch(
    block,
    /TypeArray\(F32\[1\], ConstU32\(8U\)\).*TypeArray\(F32\[1\], ConstU32\(8U\)\)/s,
    "must not declare both clip[8] and cull[8] unconditionally",
  );

  const setAttr = read("src/shader_recompiler/backend/spirv/emit_spirv_context_get_set.cpp");
  assert.match(
    setAttr,
    /ValidId\(ctx\.clip_distances\)/,
    "dropped clip planes must not be stored",
  );
  assert.match(
    setAttr,
    /ValidId\(ctx\.cull_distances\)/,
    "dropped cull planes must not be stored",
  );

  const instance = read("src/video_core/renderer_vulkan/vk_instance.cpp");
  assert.match(
    instance,
    /\.shaderCullDistance\s*=\s*features\.shaderCullDistance/,
    "CullDistance builtin requires the Vulkan feature to be enabled",
  );
});
