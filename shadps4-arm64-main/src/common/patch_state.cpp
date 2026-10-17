// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/patch_state.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <nlohmann/json.hpp>

#include "common/io_file.h"

namespace PatchUserState {

namespace {

using json = nlohmann::json;

constexpr u64 kMaxStateBytes = 1024 * 1024;

std::string ReadFileString(const std::filesystem::path& path, u64 max_bytes) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    in.seekg(0, std::ios::end);
    const auto size = in.tellg();
    if (size < 0 || static_cast<u64>(size) > max_bytes) {
        return {};
    }
    in.seekg(0, std::ios::beg);
    std::string out(static_cast<size_t>(size), '\0');
    if (!out.empty()) {
        in.read(out.data(), size);
    }
    return out;
}

std::string Iso8601Now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &time);
#else
    gmtime_r(&time, &tm);
#endif
    std::ostringstream out;
    out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// Deduplicates while preserving first-seen order. A removed/unknown ID is still a well-formed
// patch ID and is deliberately preserved so a reintroduced patch recovers its prior selection.
std::vector<std::string> NormalizeIds(const std::vector<std::string>& ids) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& id : ids) {
        if (seen.insert(id).second) {
            out.push_back(id);
        }
    }
    return out;
}

// Parses a JSON string array into the target vector. Returns false on non-array or non-string
// elements. Invalid patch-ID format is rejected (garbage must not enter state), but valid IDs
// that merely no longer exist in the manifest are kept.
bool ParseIdArray(const json& value, std::vector<std::string>& out) {
    if (!value.is_array()) {
        return false;
    }
    std::vector<std::string> parsed;
    parsed.reserve(value.size());
    for (const auto& item : value) {
        if (!item.is_string()) {
            return false;
        }
        const auto id = item.get<std::string>();
        if (!PatchRepository::ValidatePatchId(id)) {
            return false;
        }
        parsed.push_back(id);
    }
    out = NormalizeIds(parsed);
    return true;
}

json ToJson(const State& state) {
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

// Validates a parsed state JSON object into `out`. Does NOT enforce the filename-stem == serial
// rule; the caller that knows the file name applies that check. Single source of truth shared by
// Load() and ParseState() so the Android domain bridge never re-implements state semantics.
bool ParseStateObject(const json& root, State& out, std::string& error) {
    if (!root.is_object()) {
        error = "state root must be an object";
        return false;
    }

    if (!root.contains("schema") || !root["schema"].is_number_integer()) {
        error = "state missing integer schema";
        return false;
    }
    const int schema = root["schema"].get<int>();
    if (schema != kSupportedSchema) {
        error = "unsupported state schema " + std::to_string(schema);
        return false;
    }

    if (!root.contains("serial") || !root["serial"].is_string()) {
        error = "state missing serial";
        return false;
    }
    out.serial = root["serial"].get<std::string>();
    if (!PatchRepository::ValidateCusa(out.serial)) {
        error = "state serial is not a valid CUSA";
        return false;
    }

    if (!root.contains("repository_id") || !root["repository_id"].is_string() ||
        root["repository_id"].get<std::string>().empty()) {
        error = "state missing repository_id";
        return false;
    }
    out.repository_id = root["repository_id"].get<std::string>();

    if (root.contains("selected_preset")) {
        if (!root["selected_preset"].is_null() && !root["selected_preset"].is_string()) {
            error = "state selected_preset must be null or a string";
            return false;
        }
        if (root["selected_preset"].is_string()) {
            out.selected_preset = root["selected_preset"].get<std::string>();
        }
    }

    if (root.contains("enabled_patch_ids")) {
        if (!ParseIdArray(root["enabled_patch_ids"], out.enabled_patch_ids)) {
            error = "state enabled_patch_ids must be an array of patch IDs";
            return false;
        }
    }
    if (root.contains("disabled_patch_ids")) {
        if (!ParseIdArray(root["disabled_patch_ids"], out.disabled_patch_ids)) {
            error = "state disabled_patch_ids must be an array of patch IDs";
            return false;
        }
    }

    if (root.contains("last_seen_repository_revision")) {
        if (!root["last_seen_repository_revision"].is_string()) {
            error = "state last_seen_repository_revision must be a string";
            return false;
        }
        out.last_seen_repository_revision =
            root["last_seen_repository_revision"].get<std::string>();
    }
    if (root.contains("updated_at")) {
        if (!root["updated_at"].is_string()) {
            error = "state updated_at must be a string";
            return false;
        }
        out.updated_at = root["updated_at"].get<std::string>();
    }
    return true;
}

} // namespace

LoadResult Load(const std::filesystem::path& state_path) {
    LoadResult result;
    result.state = Default("");

    if (!std::filesystem::exists(state_path)) {
        result.ok = true;
        result.file_found = false;
        return result;
    }
    result.file_found = true;

    const auto text = ReadFileString(state_path, kMaxStateBytes);
    if (text.empty()) {
        result.error = "state file is missing, empty, or exceeds the size bound";
        return result;
    }

    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error&) {
        result.error = "state file is malformed JSON";
        return result;
    }
    if (!ParseStateObject(root, result.state, result.error)) {
        return result;
    }
    // The file name is part of the state's identity. A state file whose embedded serial does
    // not match its name is treated as corruption rather than trusted.
    const std::string stem = state_path.stem().string();
    if (PatchRepository::ValidateCusa(stem) && stem != result.state.serial) {
        result.error = "state serial " + result.state.serial + " does not match file " + stem;
        return result;
    }

    result.ok = true;
    return result;
}

std::optional<State> ParseState(const std::string& text, std::string* error) {
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error&) {
        if (error) {
            *error = "state text is malformed JSON";
        }
        return std::nullopt;
    }
    State state;
    std::string err;
    if (!ParseStateObject(root, state, err)) {
        if (error) {
            *error = std::move(err);
        }
        return std::nullopt;
    }
    return state;
}

bool Save(const std::filesystem::path& state_path, const State& state) {
    if (state.schema != kSupportedSchema || !PatchRepository::ValidateCusa(state.serial) ||
        state.repository_id.empty()) {
        return false;
    }

    std::error_code ec;
    if (state_path.has_parent_path()) {
        std::filesystem::create_directories(state_path.parent_path(), ec);
        if (ec) {
            return false;
        }
    }

    const std::filesystem::path tmp_path = state_path.string() + ".tmp";
    const std::string body = ToJson(state).dump(2);
    {
        Common::FS::IOFile out(tmp_path, Common::FS::FileAccessMode::Create);
        if (!out.IsOpen()) {
            return false;
        }
        if (out.WriteString(body) != body.size()) {
            return false;
        }
        if (!out.Flush() || !out.Commit()) {
            return false;
        }
    } // Close before rename.

    std::filesystem::rename(tmp_path, state_path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

State Default(const std::string& serial, const std::string& repository_id) {
    State state;
    state.serial = serial;
    state.repository_id = repository_id;
    return state;
}

State StampRepositoryRevision(const State& state, const std::string& revision) {
    State stamped = state;
    stamped.last_seen_repository_revision = revision;
    stamped.updated_at = Iso8601Now();
    return stamped;
}

EffectiveSelection ResolveEffectiveSelection(const PatchRepository::ResolvedGame& game,
                                             const State& state,
                                             const std::vector<std::string>& preset_base_ids) {
    EffectiveSelection result;

    // The persisted repository_id must mean something: state written for one repository must
    // never drive selection for another. An unbound state (empty repository_id) is also not
    // authoritative. Fail safe: no entries, no apply_ids, leave the state untouched.
    if (!game.repository_id.empty() && state.repository_id != game.repository_id) {
        result.repository_mismatch = true;
        return result;
    }

    std::unordered_set<std::string> enabled(state.enabled_patch_ids.begin(),
                                            state.enabled_patch_ids.end());
    std::unordered_set<std::string> disabled(state.disabled_patch_ids.begin(),
                                             state.disabled_patch_ids.end());
    // The selected preset's base IDs contribute an enabled set the user did not have to
    // toggle individually. Explicit user choices are layered on top and always win.
    std::unordered_set<std::string> base(preset_base_ids.begin(), preset_base_ids.end());
    std::unordered_set<std::string> present;
    present.reserve(game.patches.size());

    result.entries.reserve(game.patches.size() + enabled.size() + disabled.size());

    for (const auto& patch : game.patches) {
        present.insert(patch.id);
        SelectionEntry entry;
        entry.id = patch.id;
        if (disabled.contains(patch.id)) {
            // Explicitly disabled wins over any preset/enable, deterministically.
            entry.status = SelectionStatus::Disabled;
        } else if (enabled.contains(patch.id) || base.contains(patch.id)) {
            if (patch.compatibility == PatchRepository::PatchCompatibility::Compatible) {
                entry.status = SelectionStatus::Enabled;
                result.apply_ids.insert(patch.id);
            } else {
                entry.status = SelectionStatus::Incompatible;
            }
        } else {
            entry.status = SelectionStatus::DefaultOff;
        }
        result.entries.push_back(std::move(entry));
    }

    // State-referenced IDs absent from this revision are reported as unavailable, never
    // dropped. They keep the selection alive so a reintroduced ID recovers its prior state.
    const auto append_missing = [&](const std::vector<std::string>& ids) {
        for (const auto& id : ids) {
            if (present.contains(id)) {
                continue;
            }
            const bool already_listed =
                std::any_of(result.entries.begin(), result.entries.end(),
                            [&](const SelectionEntry& e) { return e.id == id; });
            if (already_listed) {
                continue;
            }
            SelectionEntry entry;
            entry.id = id;
            entry.status = SelectionStatus::Unavailable;
            result.entries.push_back(std::move(entry));
        }
    };
    append_missing(state.enabled_patch_ids);
    append_missing(state.disabled_patch_ids);

    return result;
}

} // namespace PatchUserState