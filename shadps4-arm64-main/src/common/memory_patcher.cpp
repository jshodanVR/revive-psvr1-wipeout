// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <codecvt>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <nlohmann/json.hpp>
#include <pugixml.hpp>
#include "common/elf_info.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/emulator_state.h"
#include "core/file_format/psf.h"
#include "memory_patcher.h"

namespace MemoryPatcher {

EXPORT uintptr_t g_eboot_address;
uint64_t g_eboot_image_size;
std::string g_game_serial;
std::string patch_file;
std::filesystem::path g_managed_session_path;
std::filesystem::path g_managed_storage_root;
bool patches_applied = false;
std::vector<patchInfo> pending_patches;

namespace {
#ifdef MEMORY_PATCHER_TEST_OBSERVER
PatchMemoryObserver g_patch_memory_observer = nullptr;
#endif

bool ParseStrictHexAddress(const std::string& str, uint64_t& out_val) {
    if (str.empty()) return false;
    std::string_view sv = str;
    if (sv.starts_with("0x") || sv.starts_with("0X")) {
        sv.remove_prefix(2);
    }
    if (sv.empty() || sv.size() > 16) return false;
    uint64_t val = 0;
    for (char c : sv) {
        val <<= 4;
        if (c >= '0' && c <= '9') {
            val |= (c - '0');
        } else if (c >= 'a' && c <= 'f') {
            val |= (c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            val |= (c - 'A' + 10);
        } else {
            return false;
        }
    }
    out_val = val;
    return true;
}

bool ParseStrictHexBytes(const std::string& hex_str, std::vector<uint8_t>& out_bytes) {
    if (hex_str.empty()) return false;
    std::string_view sv = hex_str;
    if (sv.starts_with("0x") || sv.starts_with("0X")) {
        sv.remove_prefix(2);
    }
    if (sv.empty() || sv.length() % 2 != 0) {
        return false;
    }
    out_bytes.clear();
    out_bytes.reserve(sv.length() / 2);
    for (size_t i = 0; i < sv.length(); i += 2) {
        char c1 = sv[i];
        char c2 = sv[i + 1];
        int d1 = (c1 >= '0' && c1 <= '9') ? (c1 - '0') :
                 (c1 >= 'a' && c1 <= 'f') ? (c1 - 'a' + 10) :
                 (c1 >= 'A' && c1 <= 'F') ? (c1 - 'A' + 10) : -1;
        int d2 = (c2 >= '0' && c2 <= '9') ? (c2 - '0') :
                 (c2 >= 'a' && c2 <= 'f') ? (c2 - 'a' + 10) :
                 (c2 >= 'A' && c2 <= 'F') ? (c2 - 'A' + 10) : -1;
        if (d1 < 0 || d2 < 0) {
            return false;
        }
        out_bytes.push_back(static_cast<uint8_t>((d1 << 4) | d2));
    }
    return true;
}

bool IsWithinEbootBounds(uintptr_t addr, size_t len) {
    if (g_eboot_address == 0 || g_eboot_image_size == 0 || len == 0) {
        return false;
    }
    const uintptr_t eboot_start = g_eboot_address;
    const uintptr_t eboot_end = g_eboot_address + g_eboot_image_size;
    if (addr < eboot_start || addr + len > eboot_end || addr + len < addr) {
        return false;
    }
    return true;
}

} // namespace

std::string toHex(u64 value, size_t byteSize) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(byteSize * 2) << value;
    return ss.str();
}

std::string convertValueToHex(const std::string type, const std::string valueStr) {
    std::string result;
    uint64_t hexVal = 0;

    if (type == "byte") {
        if (!ParseStrictHexAddress(valueStr, hexVal)) return "";
        result = toHex(hexVal, 1);
    } else if (type == "bytes16") {
        if (!ParseStrictHexAddress(valueStr, hexVal)) return "";
        result = toHex(hexVal, 2);
    } else if (type == "bytes32") {
        if (!ParseStrictHexAddress(valueStr, hexVal)) return "";
        result = toHex(hexVal, 4);
    } else if (type == "bytes64") {
        if (!ParseStrictHexAddress(valueStr, hexVal)) return "";
        result = toHex(hexVal, 8);
    } else if (type == "float32") {
        union {
            float f;
            uint32_t i;
        } floatUnion;
        floatUnion.f = std::stof(valueStr);
        result = toHex(std::byteswap(floatUnion.i), sizeof(floatUnion.i));
    } else if (type == "float64") {
        union {
            double d;
            uint64_t i;
        } doubleUnion;
        doubleUnion.d = std::stod(valueStr);
        result = toHex(std::byteswap(doubleUnion.i), sizeof(doubleUnion.i));
    } else if (type == "utf8") {
        std::vector<unsigned char> byteArray =
            std::vector<unsigned char>(valueStr.begin(), valueStr.end());
        byteArray.push_back('\0');
        std::stringstream ss;
        for (unsigned char c : byteArray) {
            ss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(c);
        }
        result = ss.str();
    } else if (type == "utf16") {
        std::wstring wide_str(valueStr.size(), L'\0');
        std::mbstowcs(&wide_str[0], valueStr.c_str(), valueStr.size());
        wide_str.resize(std::wcslen(wide_str.c_str()));

        std::u16string valueStringU16;

        for (wchar_t wc : wide_str) {
            if (wc <= 0xFFFF) {
                valueStringU16.push_back(static_cast<char16_t>(wc));
            } else {
                wc -= 0x10000;
                valueStringU16.push_back(static_cast<char16_t>(0xD800 | (wc >> 10)));
                valueStringU16.push_back(static_cast<char16_t>(0xDC00 | (wc & 0x3FF)));
            }
        }

        std::vector<unsigned char> byteArray;
        // convert to little endian
        for (char16_t ch : valueStringU16) {
            unsigned char low_byte = static_cast<unsigned char>(ch & 0x00FF);
            unsigned char high_byte = static_cast<unsigned char>((ch >> 8) & 0x00FF);

            byteArray.push_back(low_byte);
            byteArray.push_back(high_byte);
        }
        byteArray.push_back('\0');
        byteArray.push_back('\0');
        std::stringstream ss;

        for (unsigned char ch : byteArray) {
            ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(ch);
        }
        result = ss.str();
    } else if (type == "bytes") {
        result = valueStr;
    } else if (type == "mask" || type == "mask_jump32") {
        result = valueStr;
    } else {
        LOG_INFO(Loader, "Error applying Patch, unknown type: {}", type);
    }
    return result;
}

void ApplyPendingPatches();

namespace {

std::unordered_set<std::string>& AppliedXmlFiles() {
    static std::unordered_set<std::string> applied;
    return applied;
}

std::string XmlAppliedKey(const std::filesystem::path& xml_path) {
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(xml_path, ec);
    if (!ec && !canonical.empty()) {
        return canonical.string();
    }
    return xml_path.string();
}

std::optional<PatchLineType> PatchLineTypeFromString(const std::string& type) {
    if (type == "byte")
        return PatchLineType::Byte;
    if (type == "bytes16")
        return PatchLineType::Bytes16;
    if (type == "bytes32")
        return PatchLineType::Bytes32;
    if (type == "bytes64")
        return PatchLineType::Bytes64;
    if (type == "bytes")
        return PatchLineType::Bytes;
    if (type == "float32")
        return PatchLineType::Float32;
    if (type == "float64")
        return PatchLineType::Float64;
    if (type == "utf8")
        return PatchLineType::Utf8;
    if (type == "utf16")
        return PatchLineType::Utf16;
    if (type == "mask")
        return PatchLineType::Mask;
    if (type == "mask_jump32")
        return PatchLineType::MaskJump32;
    return std::nullopt;
}

PatchLine BuildPatchLine(const pugi::xml_node& node) {
    PatchLine line;
    const std::string type = node.attribute("Type").value();
    const auto parsed_type = PatchLineTypeFromString(type);
    if (parsed_type.has_value()) {
        line.type = *parsed_type;
    }

    line.address = node.attribute("Address").value();
    line.value = node.attribute("Value").value();
    line.target = node.attribute("Target").value();
    line.size = node.attribute("Size").value();
    const std::string offset = node.attribute("Offset").value();
    if (!offset.empty()) {
        try {
            line.mask_offset = std::stoi(offset, nullptr, 10);
        } catch (...) {
            line.mask_offset = 0;
        }
    }

    if (type == "mask_jump32") {
        line.target = node.attribute("Target").value();
        line.size = node.attribute("Size").value();
    } else {
        line.value = convertValueToHex(type, line.value);
    }

    line.little_endian = (type == "bytes16" || type == "bytes32" || type == "bytes64");
    return line;
}

std::string CurrentAppVersion() {
    auto* param_sfo = Common::Singleton<PSF>::Instance();
    return std::string{param_sfo->GetString("APP_VER").value_or("Unknown version")};
}

bool IsMaskType(PatchLineType type) {
    return type == PatchLineType::Mask || type == PatchLineType::MaskJump32;
}

PatchMask ToPatchMask(PatchLineType type) {
    if (type == PatchLineType::Mask)
        return PatchMask::Mask;
    if (type == PatchLineType::MaskJump32)
        return PatchMask::Mask_Jump32;
    return PatchMask::None;
}

bool ApplyLine(const PatchDefinition& def, const PatchLine& line, bool version_matches,
               std::string& error) {
    // Preserve the exact historical version-gating semantics: mask/mask_jump32 lines are
    // applied regardless of APP_VER, while fixed-address lines require an exact match.
    if (!version_matches && !IsMaskType(line.type))
        return true;

    PatchMask patch_mask = ToPatchMask(line.type);
    try {
        if (!PatchMemory(def.name, line.address, line.value, line.target, line.size, false,
                         line.little_endian, patch_mask, line.mask_offset)) {
            error = "memory patch write failed or out of bounds";
            return false;
        }
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
    return true;
}

std::string CanonicalSelectorKey(const std::string& title, const std::string& name,
                                 const std::string& author, const std::string& app_version,
                                 const std::string& app_elf) {
    // Reproducible identity used both for the identity-map lookup and the legacy fallback.
    std::string key = title + "\x1f" + name + "\x1f" + author + "\x1f" + app_version + "\x1f" +
                      app_elf;
    return key;
}

} // namespace

std::string CanonicalPatchSelectorKey(const std::string& title, const std::string& name,
                                      const std::string& author, const std::string& app_version,
                                      const std::string& app_elf) {
    return CanonicalSelectorKey(title, name, author, app_version, app_elf);
}

std::optional<PatchLineType> ParsePatchLineType(const std::string& type) {
    return PatchLineTypeFromString(type);
}

bool IsMaskPatchType(PatchLineType type) {
    return IsMaskType(type);
}

std::string PatchDefinitionIdentity(const std::string& title, const std::string& name,
                                    const std::string& author, const std::string& app_version,
                                    const std::string& app_elf, const PatchIdentityMap& identities) {
    const auto key = CanonicalSelectorKey(title, name, author, app_version, app_elf);
    if (auto it = identities.find(key); it != identities.end()) {
        return it->second;
    }
    // Deterministic fallback for unmanaged/local XML. Not as stable as a repository-supplied
    // ID, but never derived from list position, and stable across platforms/compilers/process
    // restarts (FNV-1a over the canonical UTF-8 identity fields).
    constexpr u64 kFnvOffset = 14695981039346656037ull;
    constexpr u64 kFnvPrime = 1099511628211ull;
    u64 hash = kFnvOffset;
    for (const unsigned char c : key) {
        hash ^= c;
        hash *= kFnvPrime;
    }
    return "legacy:" + toHex(hash, sizeof(hash));
}

std::vector<PatchDefinition> EnumeratePatchDefinitions(const std::filesystem::path& xml_path,
                                                      const PatchIdentityMap& identities) {
    std::vector<PatchDefinition> result;
    pugi::xml_document doc;
    pugi::xml_parse_result parse_result = doc.load_file(xml_path.c_str());
    if (!parse_result) {
        return result;
    }

    auto patch_xml = doc.child("Patch");
    for (auto it = patch_xml.children().begin(); it != patch_xml.children().end(); ++it) {
        if (std::string(it->name()) != "Metadata")
            continue;

        PatchDefinition def;
        def.title = it->attribute("Title").value();
        def.name = it->attribute("Name").value();
        def.author = it->attribute("Author").value();
        def.patch_version = it->attribute("PatchVer").value();
        def.app_version = it->attribute("AppVer").value();
        def.app_elf = it->attribute("AppElf").value();
        def.note = it->attribute("Note").value();
        def.is_enabled = std::string(it->attribute("isEnabled").value()) == "true";
        def.id = PatchDefinitionIdentity(def.title, def.name, def.author, def.app_version,
                                         def.app_elf, identities);

        auto patch_list = it->first_child();
        for (auto line_it = patch_list.children().begin(); line_it != patch_list.children().end();
             ++line_it) {
            def.lines.push_back(BuildPatchLine(*line_it));
        }
        result.push_back(std::move(def));
    }
    return result;
}

std::unordered_set<std::string> EnumeratePatchTitleIds(const std::filesystem::path& xml_path) {
    std::unordered_set<std::string> ids;
    pugi::xml_document doc;
    if (!doc.load_file(xml_path.c_str())) {
        return ids;
    }

    auto patch_xml = doc.child("Patch");
    for (auto it = patch_xml.children().begin(); it != patch_xml.children().end(); ++it) {
        if (std::string(it->name()) != "TitleID")
            continue;
        for (auto id_it = it->children().begin(); id_it != it->children().end(); ++id_it) {
            if (std::string(id_it->name()) == "ID") {
                const auto value = std::string(id_it->child_value());
                if (!value.empty()) {
                    ids.insert(value);
                }
            }
        }
    }
    return ids;
}

PatchApplyResult ApplyManagedPatches(const std::filesystem::path& xml_path,
                                     const std::unordered_set<std::string>& enabled_ids,
                                     const PatchIdentityMap& identities) {
    PatchApplyResult result;
    if (IsXmlAlreadyApplied(xml_path)) {
        result.ok = false;
        result.records.push_back({"", "", PatchApplyStatus::Disabled,
                                  "XML already applied in this session"});
        return result;
    }

    const auto defs = EnumeratePatchDefinitions(xml_path, identities);
    if (defs.empty()) {
        result.ok = false;
        return result;
    }
    MarkXmlApplied(xml_path);

    // Reject duplicate stable IDs deterministically (first wins, later entries are skipped
    // with an InvalidDefinition record).
    std::unordered_set<std::string> seen_ids;
    const auto app_version = CurrentAppVersion();

    for (const auto& def : defs) {
        if (!seen_ids.insert(def.id).second) {
            result.ok = false;
            result.records.push_back({def.id, def.name, PatchApplyStatus::InvalidDefinition,
                                      "duplicate stable ID"});
            continue;
        }

        if (enabled_ids.find(def.id) == enabled_ids.end()) {
            result.records.push_back({def.id, def.name, PatchApplyStatus::Disabled,
                                      "not in enabled set"});
            continue;
        }

        const bool version_matches = (def.app_version == app_version);
        if (!version_matches && std::none_of(def.lines.begin(), def.lines.end(), [](const auto& l) {
                return IsMaskType(l.type);
            })) {
            result.records.push_back({def.id, def.name, PatchApplyStatus::VersionMismatch,
                                      "requires app version " + def.app_version});
            continue;
        }

        bool all_lines_ok = true;
        std::string error;
        for (const auto& line : def.lines) {
            if (!ApplyLine(def, line, version_matches, error)) {
                all_lines_ok = false;
                result.ok = false;
                result.records.push_back({def.id, def.name, PatchApplyStatus::WriteFailed, error});
                break;
            }
        }
        if (all_lines_ok) {
            result.records.push_back({def.id, def.name, PatchApplyStatus::Applied, ""});
        }
    }

    return result;
}

void ApplyLegacyPatchesFromXML(std::filesystem::path path) {
    if (IsXmlAlreadyApplied(path)) {
        LOG_WARNING(Loader, "Skipping legacy patch application; XML already applied: {}",
                    path.string());
        return;
    }
    MarkXmlApplied(path);

    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(path.c_str());

    if (!result) {
        LOG_ERROR(Loader, "Could not parse patch XML: {}", result.description());
        return;
    }

    const auto app_version = CurrentAppVersion();
    auto patch_xml = doc.child("Patch");
    for (auto it = patch_xml.children().begin(); it != patch_xml.children().end(); ++it) {
        if (std::string(it->name()) != "Metadata")
            continue;
        if (std::string(it->attribute("isEnabled").value()) != "true")
            continue;

        const std::string name = it->attribute("Name").value();
        const std::string metadata_app_ver = it->attribute("AppVer").value();
        const bool version_matches = metadata_app_ver == app_version;

        auto patch_list = it->first_child();
        for (auto line_it = patch_list.children().begin(); line_it != patch_list.children().end();
             ++line_it) {
            const auto line = BuildPatchLine(*line_it);
            if (!version_matches && !IsMaskType(line.type))
                continue;
            std::string error;
            PatchDefinition def;
            def.name = name;
            if (!ApplyLine(def, line, version_matches, error)) {
                LOG_ERROR(Loader, "Failed to apply legacy patch {}: {}", name, error);
            }
        }
    }
}

void ApplyPatchesFromXML(std::filesystem::path path) {
    ApplyLegacyPatchesFromXML(path);
}

bool IsXmlAlreadyApplied(const std::filesystem::path& xml_path) {
    return AppliedXmlFiles().contains(XmlAppliedKey(xml_path));
}

void MarkXmlApplied(const std::filesystem::path& xml_path) {
    AppliedXmlFiles().insert(XmlAppliedKey(xml_path));
}

void ResetAppliedXmlFiles() {
    AppliedXmlFiles().clear();
}

void ResetSession() {
    ResetAppliedXmlFiles();
    patches_applied = false;
    pending_patches.clear();
}

namespace {

const char* ApplyStatusReason(PatchApplyStatus status) {
    switch (status) {
    case PatchApplyStatus::Applied:
        return "applied";
    case PatchApplyStatus::Disabled:
        return "not_enabled";
    case PatchApplyStatus::VersionMismatch:
        return "version_mismatch";
    case PatchApplyStatus::UnknownPatchId:
        return "unknown_id";
    case PatchApplyStatus::InvalidDefinition:
        return "invalid_definition";
    case PatchApplyStatus::UnsupportedType:
        return "unsupported_type";
    case PatchApplyStatus::WriteFailed:
        return "write_failed";
    }
    return "unknown";
}

PatchIdentityMap IdentityMapFromSession(const PatchSession::Config& session) {
    PatchIdentityMap map;
    for (const auto& identity : session.identities) {
        map[CanonicalPatchSelectorKey(identity.selector.title, identity.selector.name,
                                      identity.selector.author, identity.selector.app_ver,
                                      identity.selector.app_elf)] = identity.id;
    }
    return map;
}

} // namespace

ManagedSessionResult ApplyManagedSession(const PatchSession::Config& session,
                                         const std::filesystem::path& repository_root,
                                         const std::string& actual_serial,
                                         const std::string& actual_app_version) {
    ManagedSessionResult outcome;
    outcome.session_ok = true;

    LOG_INFO(Loader, "PATCH_SESSION_LOAD repository={} revision={} serial={} app_ver={}",
             session.repository_id, session.repository_revision, session.serial,
             session.app_version);
    for (const auto& id : session.enabled_patch_ids) {
        LOG_INFO(Loader, "PATCH_SESSION_SELECTED id={}", id);
    }

    const auto reject =
        PatchSession::Validate(session, repository_root, actual_serial, actual_app_version);
    if (reject != PatchSession::RejectReason::None) {
        outcome.reject = reject;
        outcome.apply.ok = false;
        LOG_WARNING(Loader, "PATCH_SESSION_REJECTED reason={}",
                    PatchSession::RejectReasonString(reject));
        return outcome;
    }

    const std::filesystem::path xml_path = repository_root / session.patch_file;
    LOG_INFO(Loader, "PATCH_SESSION_VERIFY file={} sha256=ok", session.patch_file);

    const auto identities = IdentityMapFromSession(session);
    const std::unordered_set<std::string> enabled_ids(session.enabled_patch_ids.begin(),
                                                      session.enabled_patch_ids.end());
    outcome.apply = ApplyManagedPatches(xml_path, enabled_ids, identities);

    // Enabled IDs that the XML does not declare at all are skipped safely (never a crash).
    const auto defs = EnumeratePatchDefinitions(xml_path, identities);
    std::unordered_set<std::string> known_ids;
    for (const auto& def : defs) {
        known_ids.insert(def.id);
    }
    for (const auto& id : session.enabled_patch_ids) {
        if (!known_ids.contains(id)) {
            LOG_INFO(Loader, "PATCH_SKIPPED id={} reason=unknown_id", id);
        }
    }
    for (const auto& record : outcome.apply.records) {
        if (record.status == PatchApplyStatus::Applied) {
            LOG_INFO(Loader, "PATCH_APPLIED id={}", record.id);
        } else if (!record.id.empty()) {
            LOG_INFO(Loader, "PATCH_SKIPPED id={} reason={}", record.id,
                     ApplyStatusReason(record.status));
        }
    }
    return outcome;
}

ManagedSessionResult ApplyManagedSessionFile(const std::filesystem::path& session_path,
                                             const std::filesystem::path& storage_root,
                                             const std::string& actual_serial,
                                             const std::string& actual_app_version) {
    ManagedSessionResult outcome;
    std::error_code ec;
    if (!std::filesystem::exists(session_path, ec)) {
        outcome.reject = PatchSession::RejectReason::MissingSession;
        outcome.apply.ok = false;
        LOG_WARNING(Loader, "PATCH_SESSION_REJECTED reason={}",
                    PatchSession::RejectReasonString(outcome.reject));
        return outcome;
    }
    const auto parsed = PatchSession::LoadFromFile(session_path);
    if (!parsed.ok) {
        outcome.reject = PatchSession::RejectReason::MalformedSession;
        outcome.apply.ok = false;
        LOG_WARNING(Loader, "PATCH_SESSION_REJECTED reason={}",
                    PatchSession::RejectReasonString(outcome.reject));
        return outcome;
    }
    const std::filesystem::path repository_root =
        storage_root / "patches" / "repository" / parsed.config.repository_id;
    return ApplyManagedSession(parsed.config, repository_root, actual_serial, actual_app_version);
}

#ifdef MEMORY_PATCHER_TEST_OBSERVER
void SetPatchMemoryObserver(PatchMemoryObserver observer) {
    g_patch_memory_observer = observer;
}

PatchMemoryObserver GetPatchMemoryObserver() {
    return g_patch_memory_observer;
}
#endif

void OnGameLoaded() {
    // Managed launch: the frozen session snapshot is the ONLY source of patches. The legacy
    // auto-scan (--patch CLI file + files.json) is skipped entirely so a managed repository can
    // never also be applied through the legacy path. Fail-open: a rejected/missing session
    // simply means the game launches unpatched.
    if (!g_managed_session_path.empty()) {
        ApplyManagedSessionFile(g_managed_session_path, g_managed_storage_root, g_game_serial,
                                CurrentAppVersion());
        ApplyPendingPatches();
        return;
    }

    std::filesystem::path patch_dir = Common::FS::GetUserPath(Common::FS::PathType::PatchesDir);
    if (!patch_file.empty()) {

        auto file_path = (patch_dir / patch_file).native();
        if (std::filesystem::exists(patch_file)) {
            ApplyPatchesFromXML(patch_file);
        } else {
            ApplyPatchesFromXML(file_path);
        }
    } else if (EmulatorState::GetInstance()->IsAutoPatchesLoadEnabled()) {
        for (auto const& repo : std::filesystem::directory_iterator(patch_dir)) {
            if (!repo.is_directory()) {
                continue;
            }
            std::ifstream json_file{repo.path() / "files.json"};
            nlohmann::json available_patches = nlohmann::json::parse(json_file);
            std::filesystem::path game_patch_file;
            for (auto const& [filename, serials] : available_patches.items()) {
                if (std::find(serials.begin(), serials.end(), g_game_serial) != serials.end()) {
                    game_patch_file = repo.path() / filename;
                    break;
                }
            }
            if (std::filesystem::exists(game_patch_file)) {
                ApplyPatchesFromXML(game_patch_file);
            }
        }
    }
    ApplyPendingPatches();
}

void AddPatchToQueue(patchInfo patchToAdd) {
    if (patches_applied) {
        if (!PatchMemory(patchToAdd.modNameStr, patchToAdd.offsetStr, patchToAdd.valueStr,
                         patchToAdd.targetStr, patchToAdd.sizeStr, patchToAdd.isOffset,
                         patchToAdd.littleEndian, patchToAdd.patchMask, patchToAdd.maskOffset)) {
            LOG_ERROR(Loader, "Failed to apply queued patch {}", patchToAdd.modNameStr);
        }
        return;
    }
    pending_patches.push_back(patchToAdd);
}

void ApplyPendingPatches() {
    patches_applied = true;
    for (size_t i = 0; i < pending_patches.size(); ++i) {
        const patchInfo& currentPatch = pending_patches[i];

        if (currentPatch.gameSerial != "*" && currentPatch.gameSerial != g_game_serial)
            continue;

        if (!PatchMemory(currentPatch.modNameStr, currentPatch.offsetStr, currentPatch.valueStr,
                         currentPatch.targetStr, currentPatch.sizeStr, currentPatch.isOffset,
                         currentPatch.littleEndian, currentPatch.patchMask,
                         currentPatch.maskOffset)) {
            LOG_ERROR(Loader, "Failed to apply pending patch {}", currentPatch.modNameStr);
        }
    }

    pending_patches.clear();
}

bool PatchMemory(std::string modNameStr, std::string offsetStr, std::string valueStr,
                 std::string targetStr, std::string sizeStr, bool isOffset, bool littleEndian,
                 PatchMask patchMask, int maskOffset) {
    if (patchMask == PatchMask::None) {
        uint64_t parsedAddress = 0;
        if (!ParseStrictHexAddress(offsetStr, parsedAddress)) {
            LOG_ERROR(Loader, "Invalid hex address in patch {}: {}", modNameStr, offsetStr);
            return false;
        }

        uintptr_t cheatAddress = 0;
        if (isOffset) {
            cheatAddress = g_eboot_address + parsedAddress;
        } else {
            if (parsedAddress < 0x400000) {
                LOG_ERROR(Loader, "Address 0x{:x} below 0x400000 base in patch {}", parsedAddress, modNameStr);
                return false;
            }
            cheatAddress = g_eboot_address + (parsedAddress - 0x400000);
        }

        std::vector<unsigned char> bytePatch;
        if (!ParseStrictHexBytes(valueStr, bytePatch)) {
            LOG_ERROR(Loader, "Invalid hex value in patch {}: {}", modNameStr, valueStr);
            return false;
        }

        if (littleEndian) {
            std::reverse(bytePatch.begin(), bytePatch.end());
        }

        if (!IsWithinEbootBounds(cheatAddress, bytePatch.size())) {
            LOG_ERROR(Loader, "Patch {} write out of bounds: address=0x{:x}, size={}", modNameStr, cheatAddress, bytePatch.size());
            return false;
        }

#ifdef MEMORY_PATCHER_TEST_OBSERVER
        if (auto observer = GetPatchMemoryObserver()) {
            observer(modNameStr);
        }
#endif

        std::memcpy(reinterpret_cast<void*>(cheatAddress), bytePatch.data(), bytePatch.size());

        LOG_INFO(Loader, "Applied patch: {}, Offset: {}, Value: {}", modNameStr,
                 cheatAddress, valueStr);
        return true;
    }

    if (patchMask == PatchMask::Mask) {
        uintptr_t baseAddress = PatternScan(offsetStr);
        if (baseAddress == 0) {
            LOG_ERROR(Loader, "PatternScan failed for mask with pattern: {}", offsetStr);
            return false;
        }

        uintptr_t patchAddress = baseAddress + maskOffset;
        std::vector<unsigned char> bytePatch;
        if (!ParseStrictHexBytes(valueStr, bytePatch)) {
            LOG_ERROR(Loader, "Invalid hex value in mask patch {}: {}", modNameStr, valueStr);
            return false;
        }

        if (!IsWithinEbootBounds(patchAddress, bytePatch.size())) {
            LOG_ERROR(Loader, "Mask patch {} write out of bounds: address=0x{:x}, size={}", modNameStr, patchAddress, bytePatch.size());
            return false;
        }

#ifdef MEMORY_PATCHER_TEST_OBSERVER
        if (auto observer = GetPatchMemoryObserver()) {
            observer(modNameStr);
        }
#endif

        std::memcpy(reinterpret_cast<void*>(patchAddress), bytePatch.data(), bytePatch.size());

        LOG_INFO(Loader, "Applied mask patch: {}, Offset: {}, Value: {}", modNameStr,
                 patchAddress, valueStr);
        return true;
    }

    if (patchMask == PatchMask::Mask_Jump32) {
        int jumpSize = 0;
        try {
            jumpSize = std::stoi(sizeStr);
        } catch (...) {
            LOG_ERROR(Loader, "Invalid jump size in mask_jump32: {}", sizeStr);
            return false;
        }

        constexpr int MAX_PATTERN_LENGTH = 256;
        if (jumpSize < 5) {
            LOG_ERROR(Loader, "Jump size must be at least 5 bytes");
            return false;
        }
        if (jumpSize > MAX_PATTERN_LENGTH) {
            LOG_ERROR(Loader, "Jump size must be no more than {} bytes.", MAX_PATTERN_LENGTH);
            return false;
        }

        // Find the base address using "Address"
        uintptr_t baseAddress = PatternScan(offsetStr);
        if (baseAddress == 0) {
            LOG_ERROR(Loader, "PatternScan failed for mask_jump32 with pattern: {}", offsetStr);
            return false;
        }
        uintptr_t patchAddress = baseAddress + maskOffset;

        // Use "Target" to locate the start of the code cave
        uintptr_t jump_target = PatternScan(targetStr);
        if (jump_target == 0) {
            LOG_ERROR(Loader, "PatternScan failed to Target with pattern: {}", targetStr);
            return false;
        }

        // Converts the Value attribute to a byte array (payload)
        std::vector<u8> payload;
        if (!ParseStrictHexBytes(valueStr, payload)) {
            LOG_ERROR(Loader, "Invalid hex value in mask_jump32 patch {}: {}", modNameStr, valueStr);
            return false;
        }

        // Calculates the end of the code cave (where the return jump will be inserted)
        uintptr_t code_cave_end = jump_target + payload.size();

        // Check bounds for all written regions
        if (!IsWithinEbootBounds(patchAddress, jumpSize)) {
            LOG_ERROR(Loader, "mask_jump32 patchAddress out of bounds: 0x{:x}, size={}", patchAddress, jumpSize);
            return false;
        }
        if (!IsWithinEbootBounds(jump_target, payload.size() + 5)) { // payload + 5 bytes return jump
            LOG_ERROR(Loader, "mask_jump32 code cave out of bounds: 0x{:x}, size={}", jump_target, payload.size() + 5);
            return false;
        }

#ifdef MEMORY_PATCHER_TEST_OBSERVER
        if (auto observer = GetPatchMemoryObserver()) {
            observer(modNameStr);
        }
#endif

        // Fills the original region (jumpSize bytes) with NOPs
        std::vector<u8> nopBytes(jumpSize, 0x90);
        std::memcpy(reinterpret_cast<void*>(patchAddress), nopBytes.data(), nopBytes.size());

        // Write the payload to the code cave, from jump_target
        std::memcpy(reinterpret_cast<void*>(jump_target), payload.data(), payload.size());

        // Inserts the initial jump in the original region to divert to the code cave
        u8 jumpInstruction[5];
        jumpInstruction[0] = 0xE9;
        s32 relJump = static_cast<s32>(jump_target - patchAddress - 5);
        std::memcpy(&jumpInstruction[1], &relJump, sizeof(relJump));
        std::memcpy(reinterpret_cast<void*>(patchAddress), jumpInstruction,
                    sizeof(jumpInstruction));

        // Inserts jump back at the end of the code cave to resume execution after patching
        u8 jumpBack[5];
        jumpBack[0] = 0xE9;
        // Calculates the relative offset to return to the instruction immediately following the
        // overwritten region
        s32 target_return = static_cast<s32>((patchAddress + jumpSize) - (code_cave_end + 5));
        std::memcpy(&jumpBack[1], &target_return, sizeof(target_return));
        std::memcpy(reinterpret_cast<void*>(code_cave_end), jumpBack, sizeof(jumpBack));

        LOG_INFO(Loader,
                 "Applied Patch mask_jump32: {}, PatchAddress: {:#x}, JumpTarget: {:#x}, "
                 "CodeCaveEnd: {:#x}, JumpSize: {}",
                 modNameStr, patchAddress, jump_target, code_cave_end, jumpSize);
        return true;
    }

    return false;
}

static std::vector<int32_t> PatternToByte(const std::string& pattern) {
    std::vector<int32_t> bytes;
    const char* start = pattern.data();
    const char* end = start + pattern.size();

    for (const char* current = start; current < end; ++current) {
        if (*current == '?') {
            ++current;
            if (*current == '?')
                ++current;
            bytes.push_back(-1);
        } else {
            bytes.push_back(strtoul(current, const_cast<char**>(&current), 16));
        }
    }

    return bytes;
}

uintptr_t PatternScan(const std::string& signature) {
    std::vector<int32_t> patternBytes = PatternToByte(signature);
    const auto scanBytes = static_cast<uint8_t*>((void*)g_eboot_address);

    const int32_t* sigPtr = patternBytes.data();
    const size_t sigSize = patternBytes.size();

    uint32_t foundResults = 0;
    for (uint32_t i = 0; i < g_eboot_image_size - sigSize; ++i) {
        bool found = true;
        for (uint32_t j = 0; j < sigSize; ++j) {
            if (scanBytes[i + j] != sigPtr[j] && sigPtr[j] != -1) {
                found = false;
                break;
            }
        }

        if (found) {
            foundResults++;
            return reinterpret_cast<uintptr_t>(&scanBytes[i]);
        }
    }

    return 0;
}

} // namespace MemoryPatcher
