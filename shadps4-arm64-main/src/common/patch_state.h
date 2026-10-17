// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "common/patch_repository.h"

namespace PatchUserState {

constexpr int kSupportedSchema = 1;

// Per-game persistent user state. Lives entirely outside the immutable repository:
// repository updates never read, rewrite, or delete these files.
struct State {
    int schema{kSupportedSchema};
    std::string serial;
    std::string repository_id;
    std::optional<std::string> selected_preset; // null == no preset chosen (presets reserved)
    std::vector<std::string> enabled_patch_ids; // unique, insertion-ordered
    std::vector<std::string> disabled_patch_ids;
    std::string last_seen_repository_revision; // informational; preserved across updates
    std::string updated_at;                     // informational ISO-8601
};

struct LoadResult {
    bool ok{false};
    std::string error;
    State state;
    bool file_found{false};
};

// Loads and validates a per-game state file. Missing file -> ok with the default state and
// file_found=false. Malformed/unsupported/mismatched files -> ok=false with an error, never a
// crash, and the default state is returned so callers can fail safely.
LoadResult Load(const std::filesystem::path& state_path);

// Atomically writes the state: temp file in the same directory, flush, fsync, then rename
// over the destination. The primary file is never truncated first. Creates missing parent
// directories. Returns false (without corrupting the destination) on any failure.
bool Save(const std::filesystem::path& state_path, const State& state);

// Fresh default state for a game, bound to the currently selected repository. Missing state
// means no managed patches enabled by default. The repository_id binds the state to the
// repository that produced it; resolution against a different repository is rejected.
State Default(const std::string& serial, const std::string& repository_id = "");

// Parses and validates a state JSON document (schema, serial, repository_id, id arrays, etc.).
// Returns nullopt and sets error on failure. Does not enforce the filename-stem == serial rule;
// callers that know the file name apply that check themselves. Single source of truth for the
// state JSON contract so Android/Kotlin marshalling never re-implements state semantics.
std::optional<State> ParseState(const std::string& text, std::string* error);

// Returns a copy with last_seen_repository_revision and updated_at stamped. Purely in memory;
// the caller decides when to persist it.
State StampRepositoryRevision(const State& state, const std::string& revision);

// ---- Effective selection ----

enum class SelectionStatus : uint8_t {
    Enabled,      // present, compatible, user-enabled -> feeds ApplyManagedPatches
    Disabled,     // present, explicitly disabled by the user
    Incompatible, // present and enabled, but version-mismatched -> not applied
    Unavailable,  // referenced in user state but absent from this repository revision
    DefaultOff,   // present but not in user state
};

struct SelectionEntry {
    std::string id;
    SelectionStatus status;
};

struct EffectiveSelection {
    // Deterministic: resolved patches in manifest order, then state-referenced-but-absent IDs.
    std::vector<SelectionEntry> entries;
    // Subset of entries with status Enabled; pass this set to MemoryPatcher::ApplyManagedPatches.
    std::unordered_set<std::string> apply_ids;
    // True when the state was written for a different repository than the one that produced the
    // resolved game. No entries and no apply_ids are produced; the state file is left untouched
    // (this function never writes). Callers must fail safe instead of applying cross-repo state.
    bool repository_mismatch{false};
};

// Computes the effective per-game selection from the resolved repository game (Milestone 2),
// the persisted user state, and the selected preset's base IDs (Milestone 6). The preset
// contributes a base set of enabled IDs on top of the explicitly enabled ones; explicit
// disabled IDs always win. Pure function: never reads or writes the state file, so a
// repository update cannot rewrite user choices. Pass an empty preset_base_ids when no
// preset is selected to preserve the pre-preset behavior exactly.
EffectiveSelection ResolveEffectiveSelection(const PatchRepository::ResolvedGame& game,
                                             const State& state,
                                             const std::vector<std::string>& preset_base_ids);

} // namespace PatchUserState