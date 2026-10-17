// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Pure C ABI bridge over the tested native PatchRepository + PatchUserState modules. Android
// (Kotlin/JNI) and host tests call these functions; every function returns a JSON string that
// the caller must release with pd_free_string. All semantics (manifest validation, SHA-256,
// selector resolution, APP_VER compatibility, state normalization, effective selection, and
// repository identity binding) live in the native modules — Kotlin only marshals JSON.

#ifdef __cplusplus
extern "C" {
#endif

// Loads and validates <repository_root>/manifest-v1.json.
// JSON: {"ok":true,"manifest":{"schema","repository_id","revision","generated_at"}}
//   or {"ok":false,"error"}
const char* pd_load_manifest(const char* repository_root);

// Loads and validates the per-game state file at state_path (missing -> ok + default state).
// JSON: {"ok","file_found","error"?,"state":{...schema 1...}}
const char* pd_load_state(const char* state_path);

// Atomically writes state_json to state_path using the authoritative native Save contract.
// state_json is validated first; the filename stem must match the embedded serial when the
// stem is itself a valid CUSA.
// JSON: {"ok":true} or {"ok":false,"error"}
const char* pd_save_state(const char* state_path, const char* state_json);

// Fresh default state bound to the given repository.
// JSON: {"ok":true,"state":{...schema 1...}}
const char* pd_default_state(const char* serial, const char* repository_id);

// Full pipeline: load manifest, resolve the game (CUSA + APP_VER), merge the passed state
// JSON, and compute the effective selection. state_json must be the validated JSON returned by
// pd_load_state/pd_default_state; a null state_json falls back to a default bound to the
// manifest's repository.
// JSON: {"ok":true,"repository_id","repository_revision","repository_mismatch",
//        "selected_preset","presets":[{"id","name","description","patch_ids",
//                                      "compatible","compatible_patch_count"}],
//        "entries":[{"id","name","author","category","risk","patch_version","app_versions",
//                    "app_version","compatibility","status"}],
//        "apply_ids":[...]}
// A preset's "compatible" flag is computed per queried APP_VER: true only when every
// referenced patch is Compatible for that version; callers must gate preset selection on it.
//   or {"ok":false,"error"}
const char* pd_resolve_effective(const char* repository_root, const char* cusa,
                                 const char* app_version, const char* state_json);

// Builds a frozen launch session snapshot from the current repository + state: repository
// identity/revision, serial, APP_VER, the resolved game's patch_file + SHA-256, the effective
// enabled IDs, and the selector identities. Cross-repository state is rejected. The session is
// self-contained: the runtime applies exactly these IDs without re-reading mutable user state.
// JSON: {"ok":true,"session":{...schema 1...}} or {"ok":false,"error"}
const char* pd_build_session(const char* repository_root, const char* cusa,
                             const char* app_version, const char* state_json);

// Releases a string returned by any pd_* function.
void pd_free_string(const char* p);

#ifdef __cplusplus
} // extern "C"
#endif