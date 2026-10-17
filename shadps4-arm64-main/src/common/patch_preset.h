// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "common/patch_repository.h"

namespace PatchPreset {

constexpr int kSupportedSchema = 1;
// Bound on the on-disk presets file. Guards against a corrupted or hostile file being read
// into memory before its SHA-256 can be verified.
constexpr std::size_t kMaxPresetBytes = 4 * 1024 * 1024;

// A named group of patch IDs the repository curates for a game (e.g. "Mobile Performance").
struct Preset {
    std::string id;
    std::string name;
    std::string description;
    // Unique, insertion-ordered. Every id must exist in the game's manifest entry.
    std::vector<std::string> patch_ids;
};

struct LoadResult {
    bool ok{false};
    std::string error;
    // Empty when the game entry declares no presets file.
    std::vector<Preset> presets;
};

struct BaseResult {
    bool ok{false};
    std::string error;
    // patch_ids of the selected preset; empty when no preset is selected.
    std::vector<std::string> base_ids;
    // The selected preset id (diagnostic metadata); empty when none selected.
    std::string preset_id;
};

// Loads and validates the game's presets file with the same protections as the patch XML:
// relative path only, SHA-256 verification against the manifest pin, size bound, schema 1,
// CUSA match, unique preset IDs, valid patch IDs, and every referenced patch ID must exist in
// the game's manifest entry. A game with no presets_file configured loads ok with empty
// presets. Pure and offline: only the repository files are read.
LoadResult Load(const PatchRepository::ManifestGameEntry& game,
                const std::filesystem::path& repository_root);

// Loads the presets and resolves the base IDs for the selected preset. No preset selected ->
// ok=true with empty base_ids. A selected preset that is not present in the (validated) file
// -> ok=false: an unknown preset is rejected deterministically rather than silently ignored,
// so the caller fails safe (unpatched launch) instead of guessing.
BaseResult ResolveBase(const PatchRepository::ManifestGameEntry& game,
                       const std::filesystem::path& repository_root,
                       const std::optional<std::string>& selected_preset);

} // namespace PatchPreset