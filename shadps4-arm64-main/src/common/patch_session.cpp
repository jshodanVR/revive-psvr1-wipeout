// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/patch_session.h"

#include <fstream>
#include <sstream>
#include <unordered_set>

#include <nlohmann/json.hpp>

#include "common/patch_repository.h"

namespace PatchSession {

using json = nlohmann::json;

namespace {

std::optional<XmlSelectorInfo> ParseSelector(const json& obj) {
    if (!obj.is_object()) {
        return std::nullopt;
    }
    XmlSelectorInfo sel;
    sel.title = obj.value("title", "");
    sel.name = obj.value("name", "");
    sel.author = obj.value("author", "");
    sel.app_ver = obj.value("app_ver", "");
    sel.app_elf = obj.value("app_elf", "");
    if (sel.title.empty() || sel.name.empty()) {
        return std::nullopt;
    }
    return sel;
}

std::optional<Identity> ParseIdentity(const json& obj) {
    if (!obj.is_object()) {
        return std::nullopt;
    }
    const std::string id = obj.value("id", "");
    if (!PatchRepository::ValidatePatchId(id)) {
        return std::nullopt;
    }
    const auto selector = obj.contains("xml_selector") ? ParseSelector(obj["xml_selector"])
                                                       : std::nullopt;
    if (!selector.has_value()) {
        return std::nullopt;
    }
    Identity identity;
    identity.id = id;
    identity.selector = *selector;
    return identity;
}

} // namespace

ParseResult Parse(const std::string& text) {
    ParseResult result;
    json root;
    try {
        root = json::parse(text);
    } catch (const json::parse_error&) {
        result.error = "session JSON is malformed";
        return result;
    }
    if (!root.is_object()) {
        result.error = "session root must be an object";
        return result;
    }
    if (!root.contains("schema") || !root["schema"].is_number_integer()) {
        result.error = "session missing integer schema";
        return result;
    }
    const int schema = root["schema"].get<int>();
    if (schema != kSupportedSchema) {
        result.error = "unsupported session schema " + std::to_string(schema);
        return result;
    }
    Config config;
    config.schema = schema;
    config.repository_id = root.value("repository_id", "");
    config.repository_revision = root.value("repository_revision", "");
    config.serial = root.value("serial", "");
    config.app_version = root.value("app_version", "");
    config.patch_file = root.value("patch_file", "");
    config.patch_sha256 = root.value("patch_sha256", "");
    if (config.repository_id.empty() || config.repository_revision.empty() ||
        config.serial.empty() || config.app_version.empty()) {
        result.error = "session missing repository identity, serial, or app version";
        return result;
    }
    if (!PatchRepository::ValidateCusa(config.serial)) {
        result.error = "session serial is not a valid CUSA";
        return result;
    }
    if (config.patch_file.empty() || !PatchRepository::ValidateRelativePath(config.patch_file)) {
        result.error = "session patch_file is not a safe relative path";
        return result;
    }
    if (config.patch_sha256.size() != 64 ||
        config.patch_sha256.find_first_not_of("0123456789abcdef") != std::string::npos) {
        result.error = "session patch_sha256 is not a lowercase hex SHA-256";
        return result;
    }
    if (!root.contains("enabled_patch_ids") || !root["enabled_patch_ids"].is_array()) {
        result.error = "session missing enabled_patch_ids array";
        return result;
    }
    std::unordered_set<std::string> seen_ids;
    for (const auto& item : root["enabled_patch_ids"]) {
        if (!item.is_string()) {
            result.error = "enabled_patch_ids must be strings";
            return result;
        }
        const std::string id = item.get<std::string>();
        if (!PatchRepository::ValidatePatchId(id) || !seen_ids.insert(id).second) {
            result.error = "invalid or duplicate enabled patch id";
            return result;
        }
        config.enabled_patch_ids.push_back(id);
    }
    if (root.contains("identities")) {
        if (!root["identities"].is_array()) {
            result.error = "identities must be an array";
            return result;
        }
        std::unordered_set<std::string> identity_ids;
        for (const auto& item : root["identities"]) {
            const auto identity = ParseIdentity(item);
            if (!identity.has_value()) {
                result.error = "invalid session identity";
                return result;
            }
            if (!identity_ids.insert(identity->id).second) {
                result.error = "duplicate session identity id";
                return result;
            }
            config.identities.push_back(*identity);
        }
    }
    if (root.contains("selected_preset")) {
        const auto& preset = root["selected_preset"];
        if (!preset.is_null() && preset.is_string()) {
            config.selected_preset = preset.get<std::string>();
        }
    }
    result.ok = true;
    result.config = std::move(config);
    return result;
}

ParseResult LoadFromFile(const std::filesystem::path& path) {
    ParseResult result;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        result.error = "session file missing";
        return result;
    }
    std::stringstream ss;
    ss << in.rdbuf();
    const auto text = ss.str();
    if (text.size() > kMaxSessionBytes) {
        result.error = "session file exceeds size bound";
        return result;
    }
    return Parse(text);
}

std::string Serialize(const Config& config) {
    json root;
    root["schema"] = config.schema;
    root["repository_id"] = config.repository_id;
    root["repository_revision"] = config.repository_revision;
    root["serial"] = config.serial;
    root["app_version"] = config.app_version;
    root["patch_file"] = config.patch_file;
    root["patch_sha256"] = config.patch_sha256;
    root["enabled_patch_ids"] = config.enabled_patch_ids;
    json identities = json::array();
    for (const auto& identity : config.identities) {
        json entry;
        entry["id"] = identity.id;
        entry["xml_selector"] = {
            {"title", identity.selector.title},
            {"name", identity.selector.name},
            {"author", identity.selector.author},
            {"app_ver", identity.selector.app_ver},
            {"app_elf", identity.selector.app_elf},
        };
        identities.push_back(std::move(entry));
    }
    root["identities"] = std::move(identities);
    root["selected_preset"] =
        config.selected_preset.has_value() ? json(config.selected_preset.value()) : json(nullptr);
    return root.dump();
}

const char* RejectReasonString(RejectReason reason) {
    switch (reason) {
    case RejectReason::None:
        return "none";
    case RejectReason::MissingSession:
        return "missing_session";
    case RejectReason::MalformedSession:
        return "malformed_session";
    case RejectReason::RepositoryMismatch:
        return "repository_mismatch";
    case RejectReason::RevisionMismatch:
        return "revision_mismatch";
    case RejectReason::SerialMismatch:
        return "serial_mismatch";
    case RejectReason::AppVersionMismatch:
        return "app_version_mismatch";
    case RejectReason::PatchFileMissing:
        return "patch_file_missing";
    case RejectReason::Sha256Mismatch:
        return "sha256_mismatch";
    }
    return "none";
}

RejectReason Validate(const Config& session, const std::filesystem::path& repository_root,
                      const std::string& actual_serial, const std::string& actual_app_version) {
    // The runtime applies the session only to the game it actually loaded.
    if (session.serial != actual_serial) {
        return RejectReason::SerialMismatch;
    }
    if (session.app_version != actual_app_version) {
        return RejectReason::AppVersionMismatch;
    }
    // Repository identity/revision against the on-disk manifest (TOCTOU guard: the repository
    // must not change between Android resolution and runtime apply).
    const auto loaded = PatchRepository::LoadManifest(repository_root / "manifest-v1.json");
    if (!loaded.ok) {
        return RejectReason::RepositoryMismatch;
    }
    if (loaded.manifest.repository_id != session.repository_id) {
        return RejectReason::RepositoryMismatch;
    }
    if (loaded.manifest.revision != session.repository_revision) {
        return RejectReason::RevisionMismatch;
    }
    // The pinned patch definition must be byte-identical to what Android resolved.
    const std::filesystem::path xml_path = repository_root / session.patch_file;
    if (!std::filesystem::exists(xml_path)) {
        return RejectReason::PatchFileMissing;
    }
    const auto actual_sha = PatchRepository::Sha256File(xml_path);
    if (!actual_sha.has_value() || *actual_sha != session.patch_sha256) {
        return RejectReason::Sha256Mismatch;
    }
    return RejectReason::None;
}

} // namespace PatchSession