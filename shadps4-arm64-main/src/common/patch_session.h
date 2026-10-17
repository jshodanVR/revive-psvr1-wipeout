// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace PatchSession {

constexpr int kSupportedSchema = 1;
// Bound on the on-disk session file. Android stages small files; this only guards against a
// corrupted or hostile file being read into memory.
constexpr std::size_t kMaxSessionBytes = 4 * 1024 * 1024;

struct XmlSelectorInfo {
    std::string title;
    std::string name;
    std::string author;
    std::string app_ver;
    std::string app_elf;
};

struct Identity {
    std::string id;
    XmlSelectorInfo selector;
};

// Frozen, self-contained launch snapshot. The runtime applies exactly the patches the Android
// layer resolved before launch: repository identity/revision, the game's serial + APP_VER, the
// patch file relative to the repository root plus its expected SHA-256, the enabled stable IDs,
// and the selector identities used to map XML metadata to repository IDs. The runtime never
// re-reads mutable user state from this snapshot.
struct Config {
    int schema{kSupportedSchema};
    std::string repository_id;
    std::string repository_revision;
    std::string serial;
    std::string app_version;
    std::string patch_file;   // relative to <storage>/patches/repository/<repository_id>
    std::string patch_sha256; // lowercase hex SHA-256 of the patch file
    std::vector<std::string> enabled_patch_ids;
    std::vector<Identity> identities;
    std::optional<std::string> selected_preset; // null == no preset chosen (presets reserved)
};

struct ParseResult {
    bool ok{false};
    std::string error;
    Config config;
};

// Parses and validates a session JSON document (schema, repository identity, serial, safe
// relative patch_file, id arrays, selector identities). Single source of truth for the session
// JSON contract so Kotlin marshalling never re-implements session semantics.
ParseResult Parse(const std::string& text);

// Reads (bounded) + parses a session file. Missing file -> ok=false with error "session file
// missing"; the caller decides the fail-open policy.
ParseResult LoadFromFile(const std::filesystem::path& path);

// Serializes a config to the session JSON contract. Used by pd_build_session to hand the
// Android layer a validated snapshot to stage before launch.
std::string Serialize(const Config& config);

enum class RejectReason : uint8_t {
    None,
    MissingSession,     // session path configured but file absent
    MalformedSession,   // session file did not parse/validate
    RepositoryMismatch, // on-disk manifest repository_id differs (or manifest unreadable)
    RevisionMismatch,   // on-disk manifest revision differs
    SerialMismatch,     // session serial differs from the game actually loaded
    AppVersionMismatch, // session app_version differs from the game actually loaded
    PatchFileMissing,   // patch file absent from the repository root
    Sha256Mismatch,     // on-disk patch file hash differs from the pinned value
};

const char* RejectReasonString(RejectReason reason);

// Verifies the pinned launch context against the on-disk repository and the game the runtime
// actually loaded. Pure check; never applies anything. Returns None when the session is safe.
RejectReason Validate(const Config& session, const std::filesystem::path& repository_root,
                      const std::string& actual_serial, const std::string& actual_app_version);

} // namespace PatchSession