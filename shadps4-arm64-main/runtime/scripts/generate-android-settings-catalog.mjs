#!/usr/bin/env node

import { readFileSync, writeFileSync } from "node:fs";
import { dirname, resolve } from "node:path";
import { fileURLToPath } from "node:url";

const projectRoot = resolve(dirname(fileURLToPath(import.meta.url)), "../..");
const headerPath = resolve(projectRoot, "src/core/emulator_settings.h");
const metadataPath = resolve(projectRoot, "runtime/settings/android-setting-metadata.json");
const shadOutput = resolve(projectRoot, "android/BachataS4/core/runtime/src/main/resources/runtime-settings/shadps4.json");

const shadOverrides = {
  "General.trophy_notification_side": { kind: "ENUM", choices: ["right", "left"] },
  "Log.type": { kind: "ENUM", choices: ["wincolor", "sync", "async"] },
  "Input.cursor_state": { kind: "ENUM", choices: ["0", "1", "2"], nativeEnumOrdinal: true },
  "Audio.audio_backend": { kind: "ENUM", choices: ["SDL", "OpenAL"], nativeEnumOrdinal: true },
  "GPU.full_screen_mode": { kind: "ENUM", choices: ["Windowed", "Fullscreen", "Borderless"] },
  "GPU.present_mode": { kind: "ENUM", choices: ["Immediate", "Mailbox", "Fifo", "FifoRelaxed"] },
};

function humanize(value) {
  return value
    .replace(/([a-z0-9])([A-Z])/g, "$1 $2")
    .replace(/_/g, " ")
    .toLowerCase()
    .replace(/(^|\s)\S/g, (letter) => letter.toUpperCase());
}

function literalDefault(type, initializer) {
  const value = initializer?.trim();
  if (!value) {
    if (type === "bool") return false;
    if (/^(?:u?int|[ius]\d+|float|double|long)/.test(type)) return 0;
    if (type.includes("vector")) return [];
    return "";
  }
  if (value === "true") return true;
  if (value === "false") return false;
  if (/^-?\d+(?:\.\d+)?$/.test(value)) return Number(value);
  const quoted = value.match(/^"([\s\S]*)"$/);
  return quoted ? quoted[1] : null;
}

function kindForCpp(type) {
  if (type === "bool") return "BOOLEAN";
  if (type.includes("vector")) return "LIST";
  if (type.includes("filesystem::path")) return "PATH";
  if (type === "float" || type === "double") return "DECIMAL";
  if (/^(?:u?int|[ius]\d+|long)/.test(type)) return "INTEGER";
  return "STRING";
}

function discoverShadPs4() {
  const source = readFileSync(headerPath, "utf8");
  const specs = [];
  const structPattern = /struct\s+(\w+Settings)\s*\{([\s\S]*?)\n\};/g;
  for (const match of source.matchAll(structPattern)) {
    const section = match[1].replace(/Settings$/, "");
    const fieldPattern = /^\s*Setting<(.+?)>\s+(\w+)(?:\{([^;]*)\})?;/gm;
    for (const field of match[2].matchAll(fieldPattern)) {
      const [, type, name, initializer] = field;
      const nativeKey = `${section}.${name}`;
      const title = humanize(name);
      const base = {
        id: `${section.toLowerCase()}.${name}`,
        nativeKey,
        section,
        category: section,
        title,
        help: `Controls shadPS4 ${title.toLowerCase()}.`,
        kind: kindForCpp(type.trim()),
        defaultValue: literalDefault(type.trim(), initializer),
        minimum: null,
        maximum: null,
        choices: [],
        scope: name.endsWith("_dir") || name === "install_dirs" ? "GLOBAL_ONLY" : "GLOBAL_AND_GAME",
        restartRequired: true,
        risk: section === "Debug" || name.includes("validation") ? "ADVANCED" : "NORMAL",
        readOnlyReason: null,
      };
      specs.push({ ...base, ...(shadOverrides[nativeKey] ?? {}) });
    }
  }
  return specs.sort(compareSpecs);
}

function compareSpecs(a, b) {
  return a.category.localeCompare(b.category) || a.nativeKey.localeCompare(b.nativeKey);
}

function metadataMap(specs) {
  return Object.fromEntries(specs.map((spec) => [spec.nativeKey, spec]));
}

function applyMetadata(discovered, metadata, label) {
  const discoveredKeys = new Set(discovered.map((spec) => spec.nativeKey));
  const metadataKeys = new Set(Object.keys(metadata));
  const missing = [...discoveredKeys].filter((key) => !metadataKeys.has(key));
  const extra = [...metadataKeys].filter((key) => !discoveredKeys.has(key));
  if (missing.length || extra.length) {
    throw new Error(`${label} metadata mismatch; missing=[${missing.join(",")}] extra=[${extra.join(",")}]`);
  }
  return Object.values(metadata).sort(compareSpecs);
}

function serialized(value) {
  return `${JSON.stringify(value, null, 2)}\n`;
}

const discoveredShad = discoverShadPs4();
const bootstrap = process.argv.includes("--bootstrap-metadata");
const check = process.argv.includes("--check");

if (bootstrap) {
  writeFileSync(metadataPath, serialized({ shadps4: metadataMap(discoveredShad) }));
}

const metadata = JSON.parse(readFileSync(metadataPath, "utf8"));
const shadCatalog = applyMetadata(discoveredShad, metadata.shadps4, "shadPS4");
const expectedShad = serialized(shadCatalog);

if (check) {
  if (readFileSync(shadOutput, "utf8") !== expectedShad) {
    throw new Error("Generated Android settings catalogs are stale");
  }
} else {
  writeFileSync(shadOutput, expectedShad);
}

console.log(`shadPS4=${shadCatalog.length}`);
