// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/patch_repository.h"

#include <algorithm>
#include <fstream>
#include <regex>
#include <sstream>

#include <nlohmann/json.hpp>
#include <pugixml.hpp>

#include "common/logging/log.h"
#include "common/sha256.h"

namespace PatchRepository {

namespace {

using json = nlohmann::json;

constexpr int kSupportedSchema = 1;
constexpr u64 kMaxManifestBytes = 4 * 1024 * 1024;

std::string LowerHex(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return out;
}

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

std::optional<json> ParseJsonFile(const std::filesystem::path& path) {
    const auto text = ReadFileString(path, kMaxManifestBytes);
    if (text.empty() && !std::filesystem::exists(path)) {
        return std::nullopt;
    }
    try {
        return json::parse(text);
    } catch (const json::parse_error&) {
        return std::nullopt;
    }
}

bool HasParentTraversal(const std::string& path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos) {
        return true;
    }
    std::istringstream stream(path);
    std::string segment;
    while (std::getline(stream, segment, '/')) {
        if (segment == ".." || segment.empty()) {
            return true;
        }
    }
    return false;
}

bool ValidateSelectorFields(const XmlSelector& selector) {
    return !selector.title.empty() && !selector.name.empty() && !selector.author.empty() &&
           !selector.app_ver.empty() && !selector.app_elf.empty();
}

std::optional<ManifestGameEntry> ParseGameEntry(const std::string& cusa, const json& value) {
    if (!value.is_object()) {
        return std::nullopt;
    }
    if (!ValidateCusa(cusa)) {
        return std::nullopt;
    }

    ManifestGameEntry entry;
    entry.cusa = cusa;

    if (!value.contains("patch_file") || !value["patch_file"].is_string()) {
        return std::nullopt;
    }
    entry.patch_file = value["patch_file"].get<std::string>();
    if (!ValidateRelativePath(entry.patch_file)) {
        return std::nullopt;
    }

    if (!value.contains("sha256") || !value["sha256"].is_string()) {
        return std::nullopt;
    }
    entry.sha256 = LowerHex(value["sha256"].get<std::string>());
    if (!std::regex_match(entry.sha256, std::regex("^[0-9a-f]{64}$"))) {
        return std::nullopt;
    }

    entry.title = value.value("title", cusa);
    if (value.contains("size")) {
        if (!value["size"].is_number_unsigned()) {
            return std::nullopt;
        }
        entry.size = value["size"].get<u64>();
    }

    if (value.contains("presets_file") || value.contains("presets_sha256")) {
        // Both-or-neither: an unverifiable presets file must never be loaded.
        if (!value.contains("presets_file") || !value["presets_file"].is_string() ||
            !value.contains("presets_sha256") || !value["presets_sha256"].is_string()) {
            return std::nullopt;
        }
        entry.presets_file = value["presets_file"].get<std::string>();
        if (!ValidateRelativePath(*entry.presets_file)) {
            return std::nullopt;
        }
        entry.presets_sha256 = LowerHex(value["presets_sha256"].get<std::string>());
        if (!std::regex_match(*entry.presets_sha256, std::regex("^[0-9a-f]{64}$"))) {
            return std::nullopt;
        }
    }

    if (value.contains("versions")) {
        if (!value["versions"].is_array()) {
            return std::nullopt;
        }
        for (const auto& version : value["versions"]) {
            if (!version.is_string()) {
                return std::nullopt;
            }
            entry.versions.push_back(version.get<std::string>());
        }
    }

    if (value.contains("patches")) {
        if (!value["patches"].is_array()) {
            return std::nullopt;
        }
        for (const auto& patch : value["patches"]) {
            if (!patch.is_object() || !patch.contains("id") || !patch["id"].is_string() ||
                !patch.contains("xml_selector") || !patch["xml_selector"].is_object()) {
                return std::nullopt;
            }

            ManifestPatchEntry patch_entry;
            patch_entry.id = patch["id"].get<std::string>();
            if (!ValidatePatchId(patch_entry.id)) {
                return std::nullopt;
            }

            const auto& selector = patch["xml_selector"];
            patch_entry.selector.title = selector.value("title", "");
            patch_entry.selector.name = selector.value("name", "");
            patch_entry.selector.author = selector.value("author", "");
            patch_entry.selector.app_ver = selector.value("app_ver", "");
            patch_entry.selector.app_elf = selector.value("app_elf", "");
            if (!ValidateSelectorFields(patch_entry.selector)) {
                return std::nullopt;
            }

            patch_entry.name = patch.value("name", patch_entry.selector.name);
            patch_entry.author = patch.value("author", patch_entry.selector.author);
            patch_entry.patch_version = patch.value("patch_version", "");
            patch_entry.category = patch.value("category", "");
            patch_entry.risk = patch.value("risk", "");
            if (patch.contains("app_versions") && patch["app_versions"].is_array()) {
                for (const auto& v : patch["app_versions"]) {
                    if (v.is_string()) {
                        patch_entry.app_versions.push_back(v.get<std::string>());
                    }
                }
            }
            patch_entry.app_elf = patch.value("app_elf", patch_entry.selector.app_elf);
            entry.patches.push_back(std::move(patch_entry));
        }
    }

    return entry;
}

} // namespace

bool ValidateCusa(const std::string& cusa) {
    static const std::regex kCusa("^CUSA[0-9]{5}$");
    return std::regex_match(cusa, kCusa);
}

bool ValidatePatchId(const std::string& id) {
    static const std::regex kId("^[a-z0-9][a-z0-9._-]{0,127}$");
    return std::regex_match(id, kId);
}

bool ValidateRelativePath(const std::string& path) {
    return !HasParentTraversal(path);
}

std::optional<std::string> Sha256File(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    sha256::SHA256 hasher;
    std::array<char, 64 * 1024> buffer{};
    while (in) {
        in.read(buffer.data(), buffer.size());
        const auto n = in.gcount();
        if (n > 0) {
            hasher.update(buffer.data(), static_cast<size_t>(n));
        }
    }
    if (in.bad()) {
        return std::nullopt;
    }
    return sha256::SHA256::Hex(hasher.final());
}

LoadResult LoadManifest(const std::filesystem::path& manifest_path) {
    LoadResult result;
    const auto parsed = ParseJsonFile(manifest_path);
    if (!parsed.has_value()) {
        result.error = "manifest-v1.json is missing or malformed JSON";
        return result;
    }
    const auto& root = *parsed;
    if (!root.is_object()) {
        result.error = "manifest root must be an object";
        return result;
    }

    if (!root.contains("schema") || !root["schema"].is_number_integer()) {
        result.error = "manifest missing integer schema";
        return result;
    }
    const int schema = root["schema"].get<int>();
    if (schema != kSupportedSchema) {
        result.error = "unsupported manifest schema " + std::to_string(schema);
        return result;
    }

    Manifest manifest;
    manifest.schema = schema;
    manifest.repository_id = root.value("repository_id", "");
    manifest.revision = root.value("revision", "");
    manifest.generated_at = root.value("generated_at", "");

    if (manifest.repository_id.empty() || manifest.revision.empty()) {
        result.error = "manifest missing repository_id or revision";
        return result;
    }

    if (!root.contains("games") || !root["games"].is_object()) {
        result.error = "manifest missing games object";
        return result;
    }

    for (auto it = root["games"].begin(); it != root["games"].end(); ++it) {
        const auto entry = ParseGameEntry(it.key(), it.value());
        if (!entry.has_value()) {
            result.error = "invalid manifest game entry for " + it.key();
            return result;
        }
        std::unordered_set<std::string> game_patch_ids;
        for (const auto& patch : entry->patches) {
            if (!game_patch_ids.insert(patch.id).second) {
                result.error = "duplicate stable patch ID " + patch.id;
                return result;
            }
        }
        manifest.games[it.key()] = *entry;
    }

    result.ok = true;
    result.manifest = std::move(manifest);
    return result;
}

ResolveResult ResolveGame(const Manifest& manifest, const std::string& cusa,
                          const std::string& app_version,
                          const std::filesystem::path& repository_root) {
    ResolveResult result;
    if (!ValidateCusa(cusa)) {
        result.error = "invalid CUSA " + cusa;
        return result;
    }

    const auto game_it = manifest.games.find(cusa);
    if (game_it == manifest.games.end()) {
        result.error = "no manifest entry for CUSA " + cusa;
        return result;
    }

    const auto& entry = game_it->second;
    const auto patch_path = repository_root / entry.patch_file;

    if (!std::filesystem::exists(patch_path)) {
        result.error = "patch XML missing: " + entry.patch_file;
        return result;
    }

    const auto actual_sha = Sha256File(patch_path);
    if (!actual_sha.has_value()) {
        result.error = "could not read patch XML: " + entry.patch_file;
        return result;
    }
    if (*actual_sha != entry.sha256) {
        result.error = "patch XML SHA-256 mismatch for " + entry.patch_file;
        return result;
    }

    const auto title_ids = MemoryPatcher::EnumeratePatchTitleIds(patch_path);
    if (title_ids.empty()) {
        result.error = "patch XML declares no TitleID";
        return result;
    }
    if (!title_ids.contains(cusa)) {
        result.error = "manifest CUSA " + cusa + " is absent from patch XML TitleIDs";
        return result;
    }

    const auto defs = MemoryPatcher::EnumeratePatchDefinitions(patch_path);
    if (defs.empty()) {
        result.error = "patch XML contains no Metadata";
        return result;
    }

    std::unordered_map<std::string, const MemoryPatcher::PatchDefinition*> def_by_selector;
    std::unordered_map<std::string, size_t> selector_hits;
    for (const auto& def : defs) {
        const auto key = MemoryPatcher::CanonicalPatchSelectorKey(
            def.title, def.name, def.author, def.app_version, def.app_elf);
        def_by_selector[key] = &def;
        ++selector_hits[key];
    }

    ResolvedGame game;
    game.entry = entry;
    game.repository_id = manifest.repository_id;

    for (const auto& manifest_patch : entry.patches) {
        const auto key = MemoryPatcher::CanonicalPatchSelectorKey(
            manifest_patch.selector.title, manifest_patch.selector.name,
            manifest_patch.selector.author, manifest_patch.selector.app_ver,
            manifest_patch.selector.app_elf);

        const auto hit_it = selector_hits.find(key);
        if (hit_it == selector_hits.end() || hit_it->second == 0) {
            result.error = "selector matches zero Metadata entries for patch " + manifest_patch.id;
            return result;
        }
        if (hit_it->second > 1) {
            result.error =
                "selector matches multiple Metadata entries for patch " + manifest_patch.id;
            return result;
        }

        const auto& def = *def_by_selector.at(key);

        ResolvedPatch resolved;
        resolved.id = manifest_patch.id;
        resolved.name = manifest_patch.name;
        resolved.app_version = def.app_version;
        resolved.definition = def;
        resolved.definition.id = manifest_patch.id;

        // The XML Metadata AppVer is authoritative for patch compatibility. The game-level
        // versions array only means "the repository covers these game versions"; it must not
        // override the patch-specific version gate. mask/mask_jump32 lines are version
        // independent, matching the Milestone 1 apply semantics.
        const bool version_matches = (def.app_version == app_version);
        const bool has_mask_lines = std::any_of(def.lines.begin(), def.lines.end(),
                                                [](const MemoryPatcher::PatchLine& l) {
                                                    return MemoryPatcher::IsMaskPatchType(l.type);
                                                });
        if (version_matches || has_mask_lines) {
            resolved.compatibility = PatchCompatibility::Compatible;
        } else {
            resolved.compatibility = PatchCompatibility::VersionMismatch;
        }

        // The manifest's per-patch app_versions is indexing/UI metadata only; it must agree
        // with the XML definition rather than override native semantics.
        if (!manifest_patch.app_versions.empty() &&
            std::find(manifest_patch.app_versions.begin(), manifest_patch.app_versions.end(),
                      def.app_version) == manifest_patch.app_versions.end()) {
            result.error = "manifest app_versions for patch " + manifest_patch.id +
                           " does not include XML AppVer " + def.app_version;
            return result;
        }

        // Unsupported patch types must be surfaced rather than silently written. Line types
        // are re-read from the raw XML because BuildPatchLine coerces unknown types to Byte.
        // Only the resolved metadata entry (matched by identity fields) is validated.
        {
            pugi::xml_document doc;
            if (!doc.load_file(patch_path.c_str())) {
                result.error = "could not reparse patch XML for " + manifest_patch.id;
                return result;
            }
            bool unsupported = false;
            for (auto meta = doc.child("Patch").child("Metadata"); meta;
                 meta = meta.next_sibling("Metadata")) {
                const std::string title = meta.attribute("Title").value();
                const std::string name = meta.attribute("Name").value();
                const std::string author = meta.attribute("Author").value();
                const std::string app_ver = meta.attribute("AppVer").value();
                const std::string app_elf = meta.attribute("AppElf").value();
                if (MemoryPatcher::CanonicalPatchSelectorKey(title, name, author, app_ver,
                                                            app_elf) != key) {
                    continue;
                }
                for (auto line = meta.child("PatchList").child("Line"); line;
                     line = line.next_sibling("Line")) {
                    const auto type = std::string(line.attribute("Type").value());
                    if (!MemoryPatcher::ParsePatchLineType(type).has_value()) {
                        unsupported = true;
                        break;
                    }
                }
                break;
            }
            if (unsupported) {
                result.error = "unsupported patch type for patch " + manifest_patch.id;
                return result;
            }
        }

        game.patches.push_back(std::move(resolved));
    }

    result.ok = true;
    result.game = std::move(game);
    return result;
}

} // namespace PatchRepository
