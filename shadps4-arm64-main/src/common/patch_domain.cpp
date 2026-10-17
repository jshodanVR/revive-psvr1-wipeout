// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/patch_domain.h"

#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "common/patch_repository.h"
#include "common/patch_preset.h"
#include "common/patch_session.h"
#include "common/patch_state.h"

namespace {

using json = nlohmann::json;

// Every pd_* function returns a heap string owned by the caller; the JNI wrapper and host tests
// free it via pd_free_string.
char* Duplicate(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) {
        return nullptr;
    }
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

std::string ErrorJson(const std::string& error) {
    json out;
    out["ok"] = false;
    out["error"] = error;
    return out.dump();
}

json StateToJson(const PatchUserState::State& state) {
    json root;
    root["schema"] = state.schema;
    root["serial"] = state.serial;
    root["repository_id"] = state.repository_id;
    root["selected_preset"] =
        state.selected_preset.has_value() ? json(state.selected_preset.value()) : json(nullptr);
    root["enabled_patch_ids"] = state.enabled_patch_ids;
    root["disabled_patch_ids"] = state.disabled_patch_ids;
    root["last_seen_repository_revision"] = state.last_seen_repository_revision;
    root["updated_at"] = state.updated_at;
    return root;
}

const char* StatusString(PatchUserState::SelectionStatus status) {
    switch (status) {
    case PatchUserState::SelectionStatus::Enabled:
        return "enabled";
    case PatchUserState::SelectionStatus::Disabled:
        return "disabled";
    case PatchUserState::SelectionStatus::Incompatible:
        return "incompatible";
    case PatchUserState::SelectionStatus::Unavailable:
        return "unavailable";
    case PatchUserState::SelectionStatus::DefaultOff:
        return "default_off";
    }
    return "default_off";
}

const char* CompatibilityString(PatchRepository::PatchCompatibility compatibility) {
    switch (compatibility) {
    case PatchRepository::PatchCompatibility::Compatible:
        return "compatible";
    case PatchRepository::PatchCompatibility::VersionMismatch:
        return "version_mismatch";
    case PatchRepository::PatchCompatibility::UnknownVersion:
        return "unknown_version";
    }
    return "unknown_version";
}

json LoadStateJson(const char* state_json, const PatchRepository::Manifest& manifest,
                   const std::string& cusa) {
    if (!state_json || state_json[0] == '\0') {
        return StateToJson(PatchUserState::Default(cusa, manifest.repository_id));
    }
    std::string error;
    const auto state = PatchUserState::ParseState(state_json, &error);
    if (!state.has_value()) {
        // Malformed state is a caller bug; fail the whole resolution with the error.
        return json();
    }
    return StateToJson(*state);
}

// Per-preset APP_VER support is derived from the resolved per-patch compatibility: a preset is
// compatible only when every referenced patch resolves as Compatible for the queried app
// version. Selection gating uses this verdict so users cannot activate curated bundles that
// silently apply nothing on their installed game version.
json PresetSupportJson(const PatchPreset::Preset& preset,
                       const std::unordered_map<std::string,
                           PatchRepository::PatchCompatibility>& compatibility_by_id) {
    int supported = 0;
    for (const auto& id : preset.patch_ids) {
        const auto it = compatibility_by_id.find(id);
        if (it != compatibility_by_id.end() &&
            it->second == PatchRepository::PatchCompatibility::Compatible) {
            ++supported;
        }
    }
    json out;
    out["compatible"] = supported == static_cast<int>(preset.patch_ids.size());
    out["compatible_patch_count"] = supported;
    return out;
}

PatchSession::Config BuildSessionConfig(const PatchRepository::ResolvedGame& game,
                                        const std::string& revision,
                                        const std::string& app_version,
                                        const PatchUserState::EffectiveSelection& selection,
                                        const std::optional<std::string>& selected_preset) {
    PatchSession::Config config;
    config.repository_id = game.repository_id;
    config.repository_revision = revision;
    config.serial = game.entry.cusa;
    config.app_version = app_version;
    config.patch_file = game.entry.patch_file;
    config.patch_sha256 = game.entry.sha256;
    // The session freezes the RESOLVED IDs, not the preset semantics: the preset was already
    // resolved (base + explicit enable - explicit disable) before launch, so the runtime only
    // ever sees the concrete patch IDs. The preset id itself is diagnostic metadata only and
    // is never re-resolved at apply time.
    // Deterministic order: selection.entries is manifest order, then absent-but-referenced IDs.
    for (const auto& entry : selection.entries) {
        if (entry.status == PatchUserState::SelectionStatus::Enabled) {
            config.enabled_patch_ids.push_back(entry.id);
        }
    }
    config.selected_preset = selected_preset;
    // The manifest xml_selector is authoritative for mapping XML metadata to stable IDs.
    const auto& manifest_patches = game.entry.patches;
    for (size_t i = 0; i < game.patches.size() && i < manifest_patches.size(); ++i) {
        const auto& mp = manifest_patches[i];
        PatchSession::Identity identity;
        identity.id = game.patches[i].id;
        identity.selector.title = mp.selector.title;
        identity.selector.name = mp.selector.name;
        identity.selector.author = mp.selector.author;
        identity.selector.app_ver = mp.selector.app_ver;
        identity.selector.app_elf = mp.selector.app_elf;
        config.identities.push_back(std::move(identity));
    }
    return config;
}

} // namespace

extern "C" {

const char* pd_load_manifest(const char* repository_root) {
    if (!repository_root || repository_root[0] == '\0') {
        return Duplicate(ErrorJson("missing repository root"));
    }
    const auto loaded = PatchRepository::LoadManifest(
        std::filesystem::path(repository_root) / "manifest-v1.json");
    if (!loaded.ok) {
        return Duplicate(ErrorJson(loaded.error));
    }
    json out;
    out["ok"] = true;
    out["manifest"] = {
        {"schema", loaded.manifest.schema},
        {"repository_id", loaded.manifest.repository_id},
        {"revision", loaded.manifest.revision},
        {"generated_at", loaded.manifest.generated_at},
    };
    return Duplicate(out.dump());
}

const char* pd_load_state(const char* state_path) {
    if (!state_path || state_path[0] == '\0') {
        return Duplicate(ErrorJson("missing state path"));
    }
    const auto result = PatchUserState::Load(std::filesystem::path(state_path));
    json out;
    out["ok"] = result.ok;
    out["file_found"] = result.file_found;
    if (!result.error.empty()) {
        out["error"] = result.error;
    }
    out["state"] = StateToJson(result.state);
    return Duplicate(out.dump());
}

const char* pd_save_state(const char* state_path, const char* state_json) {
    if (!state_path || state_path[0] == '\0') {
        return Duplicate(ErrorJson("missing state path"));
    }
    if (!state_json || state_json[0] == '\0') {
        return Duplicate(ErrorJson("missing state JSON"));
    }
    std::string error;
    const auto state = PatchUserState::ParseState(state_json, &error);
    if (!state.has_value()) {
        return Duplicate(ErrorJson(std::move(error)));
    }
    const std::string stem = std::filesystem::path(state_path).stem().string();
    if (PatchRepository::ValidateCusa(stem) && stem != state->serial) {
        return Duplicate(ErrorJson("state serial " + state->serial + " does not match file " +
                                   stem));
    }
    if (!PatchUserState::Save(std::filesystem::path(state_path), *state)) {
        return Duplicate(ErrorJson("state save failed"));
    }
    json out;
    out["ok"] = true;
    return Duplicate(out.dump());
}

const char* pd_default_state(const char* serial, const char* repository_id) {
    if (!serial || !repository_id) {
        return Duplicate(ErrorJson("missing serial or repository_id"));
    }
    json out;
    out["ok"] = true;
    out["state"] = StateToJson(PatchUserState::Default(serial, repository_id));
    return Duplicate(out.dump());
}

const char* pd_resolve_effective(const char* repository_root, const char* cusa,
                                 const char* app_version, const char* state_json) {
    if (!repository_root || !cusa || !app_version) {
        return Duplicate(ErrorJson("missing repository root, serial, or app version"));
    }
    const std::filesystem::path root(repository_root);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    if (!loaded.ok) {
        return Duplicate(ErrorJson(loaded.error));
    }

    const auto state = LoadStateJson(state_json, loaded.manifest, cusa);
    if (state.is_null()) {
        return Duplicate(ErrorJson("state JSON is malformed"));
    }

    // Build the State object again from the (possibly defaulted) JSON so the resolver always
    // sees validated state.
    std::string state_error;
    const auto state_obj = PatchUserState::ParseState(state.dump(), &state_error);
    if (!state_obj.has_value()) {
        return Duplicate(ErrorJson(std::move(state_error)));
    }

    const auto resolved = PatchRepository::ResolveGame(loaded.manifest, cusa, app_version, root);
    if (!resolved.ok) {
        return Duplicate(ErrorJson(resolved.error));
    }

    // Resolve the selected preset into concrete base IDs (Milestone 6). A selected preset
    // that fails to load/validate, or is unknown, rejects the resolution deterministically;
    // the Android layer fails open to an unpatched launch rather than guessing.
    const auto preset = PatchPreset::ResolveBase(resolved.game.entry, root,
                                                 state_obj->selected_preset);
    if (!preset.ok) {
        return Duplicate(ErrorJson(preset.error));
    }

    const auto selection = PatchUserState::ResolveEffectiveSelection(resolved.game, *state_obj,
                                                                     preset.base_ids);

    json out;
    out["ok"] = true;
    out["repository_id"] = loaded.manifest.repository_id;
    out["repository_revision"] = loaded.manifest.revision;
    out["repository_mismatch"] = selection.repository_mismatch;
    out["selected_preset"] = preset.preset_id.empty() ? json(nullptr) : json(preset.preset_id);

    json presets_array = json::array();
    const auto loaded_presets = PatchPreset::Load(resolved.game.entry, root);
    if (loaded_presets.ok) {
        std::unordered_map<std::string, PatchRepository::PatchCompatibility> compatibility_by_id;
        compatibility_by_id.reserve(resolved.game.patches.size());
        for (const auto& patch : resolved.game.patches) {
            compatibility_by_id.emplace(patch.id, patch.compatibility);
        }
        for (const auto& p : loaded_presets.presets) {
            json pj;
            pj["id"] = p.id;
            pj["name"] = p.name;
            pj["description"] = p.description;
            pj["patch_ids"] = p.patch_ids;
            for (const auto& [key, value] : PresetSupportJson(p, compatibility_by_id).items()) {
                pj[key] = value;
            }
            presets_array.push_back(std::move(pj));
        }
    }
    out["presets"] = std::move(presets_array);

    json entries = json::array();
    json apply_ids = json::array();
    if (!selection.repository_mismatch) {
        std::unordered_map<std::string, PatchUserState::SelectionStatus> status_by_id;
        for (const auto& entry : selection.entries) {
            status_by_id[entry.id] = entry.status;
        }
        std::unordered_set<std::string> resolved_ids;
        // Resolved patches in manifest order, with manifest metadata.
        const auto& manifest_patches = resolved.game.entry.patches;
        for (size_t i = 0; i < resolved.game.patches.size(); ++i) {
            const auto& patch = resolved.game.patches[i];
            resolved_ids.insert(patch.id);
            json entry;
            entry["id"] = patch.id;
            entry["name"] = patch.name;
            entry["app_version"] = patch.app_version;
            entry["compatibility"] = CompatibilityString(patch.compatibility);
            const auto status_it = status_by_id.find(patch.id);
            const auto status = status_it == status_by_id.end()
                                    ? PatchUserState::SelectionStatus::DefaultOff
                                    : status_it->second;
            entry["status"] = StatusString(status);
            if (i < manifest_patches.size()) {
                const auto& mp = manifest_patches[i];
                entry["name"] = mp.name;
                entry["author"] = mp.author;
                entry["patch_version"] = mp.patch_version;
                entry["app_versions"] = mp.app_versions;
                entry["category"] = mp.category;
                entry["risk"] = mp.risk;
            } else {
                entry["author"] = "";
                entry["patch_version"] = "";
                entry["app_versions"] = json::array();
                entry["category"] = "";
                entry["risk"] = "";
            }
            if (status == PatchUserState::SelectionStatus::Enabled) {
                apply_ids.push_back(patch.id);
            }
            entries.push_back(std::move(entry));
        }
        // State-referenced-but-absent IDs (Unavailable) keep their selection alive.
        for (const auto& entry : selection.entries) {
            if (entry.status == PatchUserState::SelectionStatus::Unavailable &&
                !resolved_ids.contains(entry.id)) {
                json missing;
                missing["id"] = entry.id;
                missing["name"] = "";
                missing["author"] = "";
                missing["patch_version"] = "";
                missing["app_versions"] = json::array();
                missing["app_version"] = "";
                missing["category"] = "";
                missing["risk"] = "";
                missing["compatibility"] = "unknown_version";
                missing["status"] = StatusString(entry.status);
                entries.push_back(std::move(missing));
            }
        }
    }
    out["entries"] = std::move(entries);
    out["apply_ids"] = std::move(apply_ids);
    return Duplicate(out.dump());
}

const char* pd_build_session(const char* repository_root, const char* cusa,
                             const char* app_version, const char* state_json) {
    if (!repository_root || !cusa || !app_version) {
        return Duplicate(ErrorJson("missing repository root, serial, or app version"));
    }
    const std::filesystem::path root(repository_root);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    if (!loaded.ok) {
        return Duplicate(ErrorJson(loaded.error));
    }

    const auto state = LoadStateJson(state_json, loaded.manifest, cusa);
    if (state.is_null()) {
        return Duplicate(ErrorJson("state JSON is malformed"));
    }
    std::string state_error;
    const auto state_obj = PatchUserState::ParseState(state.dump(), &state_error);
    if (!state_obj.has_value()) {
        return Duplicate(ErrorJson(std::move(state_error)));
    }

    const auto resolved = PatchRepository::ResolveGame(loaded.manifest, cusa, app_version, root);
    if (!resolved.ok) {
        return Duplicate(ErrorJson(resolved.error));
    }

    const auto preset = PatchPreset::ResolveBase(resolved.game.entry, root,
                                                 state_obj->selected_preset);
    if (!preset.ok) {
        return Duplicate(ErrorJson(preset.error));
    }

    const auto selection = PatchUserState::ResolveEffectiveSelection(resolved.game, *state_obj,
                                                                     preset.base_ids);
    if (selection.repository_mismatch) {
        return Duplicate(
            ErrorJson("repository mismatch: state is bound to a different repository"));
    }

    json out;
    out["ok"] = true;
    out["session"] = json::parse(PatchSession::Serialize(BuildSessionConfig(
        resolved.game, loaded.manifest.revision, app_version, selection,
        state_obj->selected_preset)));
    return Duplicate(out.dump());
}

void pd_free_string(const char* p) {
    std::free(const_cast<char*>(p));
}

} // extern "C"