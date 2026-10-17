// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/memory_patcher.h"
#include "common/types.h"

namespace PatchRepository {

// ---- Stable identity (manifest-controlled) ----

struct XmlSelector {
    std::string title;
    std::string name;
    std::string author;
    std::string app_ver;
    std::string app_elf;
};

struct ManifestPatchEntry {
    std::string id;
    std::string name;
    std::string author;
    std::string patch_version;
    std::vector<std::string> app_versions;
    std::string app_elf;
    std::string category;
    std::string risk;
    XmlSelector selector;
};

struct ManifestGameEntry {
    std::string cusa;
    std::string title;
    std::string patch_file;
    std::string sha256;
    std::optional<u64> size;
    std::vector<std::string> versions;
    std::vector<ManifestPatchEntry> patches;
    // Optional hash-verified preset file. Both fields must appear together: a presets file is
    // only trusted when its SHA-256 is pinned by the immutable manifest. The path is relative
    // to the repository root, same safety rules as patch_file.
    std::optional<std::string> presets_file;
    std::optional<std::string> presets_sha256;
};

struct Manifest {
    int schema{0};
    std::string repository_id;
    std::string revision;
    std::string generated_at;
    std::unordered_map<std::string, ManifestGameEntry> games;
};

// ---- Resolution results ----

enum class PatchCompatibility : uint8_t {
    Compatible,
    VersionMismatch,
    UnknownVersion,
};

struct ResolvedPatch {
    std::string id;
    std::string name;
    std::string app_version;
    PatchCompatibility compatibility{PatchCompatibility::UnknownVersion};
    MemoryPatcher::PatchDefinition definition;
};

struct ResolvedGame {
    ManifestGameEntry entry;
    std::vector<ResolvedPatch> patches;
    // Repository identity of the manifest that produced this resolution. Binds the resolved
    // game to its source repository so per-game user state can be validated against it.
    std::string repository_id;
};

struct LoadResult {
    bool ok{false};
    std::string error;
    Manifest manifest;
};

struct ResolveResult {
    bool ok{false};
    std::string error;
    ResolvedGame game;
};

// ---- Validation / loading ----

// Loads and validates manifest-v1.json. Patch XML is not parsed here; the manifest's
// relative paths and per-patch identity are validated syntactically. No network access.
LoadResult LoadManifest(const std::filesystem::path& manifest_path);

// Loads a game entry from a manifest, verifies its patch XML SHA-256, validates that the
// manifest CUSA exists in the XML TitleID set, resolves every manifest patch ID through its
// xml_selector, and reports APP_VER compatibility. Returns ok=false on the first fatal error.
ResolveResult ResolveGame(const Manifest& manifest, const std::string& cusa,
                          const std::string& app_version,
                          const std::filesystem::path& repository_root);

// Pure validation helpers exposed for tests.
bool ValidateCusa(const std::string& cusa);
bool ValidatePatchId(const std::string& id);
bool ValidateRelativePath(const std::string& path);

// SHA-256 of a file, lowercase hex. Returns nullopt if the file cannot be read.
std::optional<std::string> Sha256File(const std::filesystem::path& path);

} // namespace PatchRepository
