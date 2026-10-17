// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/patch_preset.h"

#include <algorithm>
#include <fstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "common/sha256.h"

namespace PatchPreset {

namespace {

using json = nlohmann::json;

std::string ReadFileString(const std::filesystem::path& path, std::size_t max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0 || static_cast<std::size_t>(size) > max_bytes) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::string out(static_cast<std::size_t>(size), '\0');
    if (!out.empty()) {
        in.read(out.data(), size);
    }
    return out;
}

bool ParsePresets(const json& root, const std::string& cusa,
                  const std::unordered_set<std::string>& manifest_patch_ids,
                  std::vector<Preset>& presets, std::string& error) {
    if (!root.is_object()) {
        error = "presets root must be an object";
        return false;
    }
    if (!root.contains("schema") || !root["schema"].is_number_integer()) {
        error = "presets missing integer schema";
        return false;
    }
    const int schema = root["schema"].get<int>();
    if (schema != kSupportedSchema) {
        error = "unsupported presets schema " + std::to_string(schema);
        return false;
    }
    if (!root.contains("serial") || !root["serial"].is_string()) {
        error = "presets missing serial";
        return false;
    }
    const std::string serial = root["serial"].get<std::string>();
    if (serial != cusa) {
        error = "presets serial " + serial + " does not match game " + cusa;
        return false;
    }
    if (!root.contains("presets") || !root["presets"].is_array()) {
        error = "presets missing presets array";
        return false;
    }

    std::unordered_set<std::string> preset_ids;
    for (const auto& item : root["presets"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_string()) {
            error = "preset entry must be an object with an id";
            return false;
        }
        Preset preset;
        preset.id = item["id"].get<std::string>();
        if (!PatchRepository::ValidatePatchId(preset.id) ||
            !preset_ids.insert(preset.id).second) {
            error = "invalid or duplicate preset id " + preset.id;
            return false;
        }
        preset.name = item.value("name", "");
        preset.description = item.value("description", "");
        if (!item.contains("patch_ids") || !item["patch_ids"].is_array()) {
            error = "preset " + preset.id + " missing patch_ids array";
            return false;
        }
        std::unordered_set<std::string> seen_patches;
        for (const auto& patch : item["patch_ids"]) {
            if (!patch.is_string()) {
                error = "preset " + preset.id + " patch_ids must be strings";
                return false;
            }
            const std::string patch_id = patch.get<std::string>();
            if (!PatchRepository::ValidatePatchId(patch_id) ||
                !seen_patches.insert(patch_id).second) {
                error = "invalid or duplicate patch id in preset " + preset.id;
                return false;
            }
            if (!manifest_patch_ids.contains(patch_id)) {
                error = "preset " + preset.id + " references unknown patch " + patch_id;
                return false;
            }
            preset.patch_ids.push_back(patch_id);
        }
        presets.push_back(std::move(preset));
    }
    return true;
}

} // namespace

LoadResult Load(const PatchRepository::ManifestGameEntry& game,
                const std::filesystem::path& repository_root) {
    LoadResult result;
    if (!game.presets_file.has_value() || !game.presets_sha256.has_value()) {
        result.ok = true;
        return result;
    }

    const std::filesystem::path presets_path = repository_root / *game.presets_file;
    if (!std::filesystem::exists(presets_path)) {
        result.error = "presets file missing: " + *game.presets_file;
        return result;
    }

    // SHA-256 first: the pinned hash is what makes the file trustworthy, so any read failure
    // or mismatch is a hard reject regardless of the JSON contents.
    const auto actual_sha = PatchRepository::Sha256File(presets_path);
    if (!actual_sha.has_value()) {
        result.error = "could not read presets file: " + *game.presets_file;
        return result;
    }
    if (*actual_sha != *game.presets_sha256) {
        result.error = "presets SHA-256 mismatch for " + *game.presets_file;
        return result;
    }

    const auto text = ReadFileString(presets_path, kMaxPresetBytes);
    if (text.empty()) {
        result.error = "presets file is empty or exceeds the size bound";
        return result;
    }

    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error&) {
        result.error = "presets file is malformed JSON";
        return result;
    }

    std::unordered_set<std::string> manifest_patch_ids;
    manifest_patch_ids.reserve(game.patches.size());
    for (const auto& patch : game.patches) {
        manifest_patch_ids.insert(patch.id);
    }

    std::string error;
    if (!ParsePresets(root, game.cusa, manifest_patch_ids, result.presets, error)) {
        result.error = std::move(error);
        return result;
    }
    result.ok = true;
    return result;
}

BaseResult ResolveBase(const PatchRepository::ManifestGameEntry& game,
                       const std::filesystem::path& repository_root,
                       const std::optional<std::string>& selected_preset) {
    BaseResult result;
    if (!selected_preset.has_value()) {
        result.ok = true;
        return result;
    }

    const auto loaded = Load(game, repository_root);
    if (!loaded.ok) {
        result.error = loaded.error;
        return result;
    }

    const auto it = std::find_if(loaded.presets.begin(), loaded.presets.end(),
                                 [&](const Preset& p) { return p.id == *selected_preset; });
    if (it == loaded.presets.end()) {
        result.error = "unknown preset " + *selected_preset;
        return result;
    }
    result.base_ids = it->patch_ids;
    result.preset_id = it->id;
    result.ok = true;
    return result;
}

} // namespace PatchPreset