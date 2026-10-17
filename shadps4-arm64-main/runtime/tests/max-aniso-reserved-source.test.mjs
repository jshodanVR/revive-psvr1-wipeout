import assert from "node:assert/strict";
import { readFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";
import test from "node:test";

const root = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const read = (relative) => readFileSync(resolve(root, relative), "utf8");

test("AnisoRatioToFloat maps reserved 3-bit values 5-7 to 16", () => {
  const header = read("src/video_core/amdgpu/resource.h");
  const start = header.indexOf("constexpr float AnisoRatioToFloat");
  const end = header.indexOf("}", header.indexOf("default:", start));
  assert.notEqual(start, -1, "AnisoRatioToFloat found");
  const fn = header.slice(start, end + 1);
  assert.match(fn, /case AnisoRatio::One:\s*return 1\.0f/);
  assert.match(fn, /case AnisoRatio::Two:\s*return 2\.0f/);
  assert.match(fn, /case AnisoRatio::Four:\s*return 4\.0f/);
  assert.match(fn, /case AnisoRatio::Eight:\s*return 8\.0f/);
  assert.match(fn, /case AnisoRatio::Sixteen:\s*default:\s*return 16\.0f/s);
  assert.doesNotMatch(fn, /UNREACHABLE/);
});

test("rasterizer image_infos can hold 64 images plus samplers", () => {
  const header = read("src/video_core/renderer_vulkan/vk_rasterizer.h");
  assert.match(
    header,
    /small_vector<vk::DescriptorImageInfo,\s*Shader::NUM_IMAGES \+ Shader::NUM_SAMPLERS>/,
  );
  assert.match(header, /small_vector<ImageBindingInfo, Shader::NUM_IMAGES>/);
});

test("sampler resource list can grow past the old 16-slot static cap", () => {
  const header = read("src/shader_recompiler/resource.h");
  assert.match(header, /NUM_SAMPLERS = 32/);
  assert.match(
    header,
    /using SamplerResourceList = boost::container::small_vector<SamplerResource, NUM_SAMPLERS>/,
  );
});

test("Sampler::MaxAniso does not trap on reserved aniso ratios", () => {
  const header = read("src/video_core/amdgpu/resource.h");
  const start = header.indexOf("float MaxAniso()");
  const end = header.indexOf("};", start);
  assert.notEqual(start, -1, "MaxAniso found");
  const fn = header.slice(start, end);
  assert.doesNotMatch(fn, /UNREACHABLE/, "reserved TEX max_aniso must not trap");
  assert.match(fn, /AnisoRatioToFloat/, "must use the 3-bit-safe mapping");
});
