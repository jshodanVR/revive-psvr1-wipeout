// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/patch_session.h"

#if defined(WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif

namespace MemoryPatcher {

extern EXPORT uintptr_t g_eboot_address;
extern uint64_t g_eboot_image_size;
extern std::string g_game_serial;
extern std::string patch_file;

// Managed launch session. Set by the boot/CLI path before OnGameLoaded runs. A non-empty
// session path marks this launch as a managed launch: the frozen session snapshot is the only
// source of patches and the legacy auto-scan is skipped entirely.
extern std::filesystem::path g_managed_session_path;
extern std::filesystem::path g_managed_storage_root;

enum PatchMask : uint8_t {
    None,
    Mask,
    Mask_Jump32,
};

enum class PatchLineType : uint8_t {
    Byte,
    Bytes16,
    Bytes32,
    Bytes64,
    Bytes,
    Float32,
    Float64,
    Utf8,
    Utf16,
    Mask,
    MaskJump32,
};

struct PatchLine {
    PatchLineType type{PatchLineType::Byte};
    std::string address;
    std::string value;
    std::string target;
    std::string size;
    int mask_offset{0};
    bool little_endian{false};
};

struct PatchDefinition {
    std::string id;
    std::string title;
    std::string name;
    std::string author;
    std::string patch_version;
    std::string app_version;
    std::string app_elf;
    std::string note;
    bool is_enabled{false};
    std::vector<PatchLine> lines;
};

enum class PatchApplyStatus : uint8_t {
    Applied,
    Disabled,
    VersionMismatch,
    UnknownPatchId,
    InvalidDefinition,
    UnsupportedType,
    WriteFailed,
};

struct PatchApplyRecord {
    std::string id;
    std::string name;
    PatchApplyStatus status{PatchApplyStatus::Disabled};
    std::string reason;
};

struct PatchApplyResult {
    std::vector<PatchApplyRecord> records;
    bool ok{true};
};

using PatchIdentityMap = std::unordered_map<std::string, std::string>;

struct patchInfo {
    std::string gameSerial;
    std::string modNameStr;
    std::string offsetStr;
    std::string valueStr;
    std::string targetStr;
    std::string sizeStr;
    bool isOffset;
    bool littleEndian;
    PatchMask patchMask;
    int maskOffset;
};

std::string convertValueToHex(const std::string type, const std::string valueStr);

void OnGameLoaded();
void AddPatchToQueue(patchInfo patchToAdd);

bool PatchMemory(std::string modNameStr, std::string offsetStr, std::string valueStr,
                 std::string targetStr, std::string sizeStr, bool isOffset, bool littleEndian,
                 PatchMask patchMask = PatchMask::None, int maskOffset = 0);

static std::vector<int32_t> PatternToByte(const std::string& pattern);
uintptr_t PatternScan(const std::string& signature);

// Milestone 1 selection seam: enumerate patch metadata without applying it, then apply an
// externally supplied set of stable patch IDs. Legacy XML isEnabled behavior is isolated in
// ApplyLegacyPatchesFromXML.
std::vector<PatchDefinition> EnumeratePatchDefinitions(
    const std::filesystem::path& xml_path, const PatchIdentityMap& identities = {});

PatchApplyResult ApplyManagedPatches(const std::filesystem::path& xml_path,
                                     const std::unordered_set<std::string>& enabled_ids,
                                     const PatchIdentityMap& identities = {});

// ---- Managed launch session (Milestone 5) ----

struct ManagedSessionResult {
    // True when the session file was present and parsed; false for missing/malformed sessions.
    bool session_ok{false};
    // Non-None when the session was rejected; nothing is applied (fail-open).
    PatchSession::RejectReason reject{PatchSession::RejectReason::None};
    PatchApplyResult apply;
};

// Validates and applies a parsed launch snapshot against the on-disk repository and the game
// the runtime actually loaded. Any rejection is logged (PATCH_SESSION_REJECTED) and returns
// without applying anything, so the game launches unpatched.
ManagedSessionResult ApplyManagedSession(const PatchSession::Config& session,
                                         const std::filesystem::path& repository_root,
                                         const std::string& actual_serial,
                                         const std::string& actual_app_version);

// Loads the session file (bounded), derives the repository root from the storage-root layout
// (<storage>/patches/repository/<repository_id>), and applies it.
ManagedSessionResult ApplyManagedSessionFile(const std::filesystem::path& session_path,
                                             const std::filesystem::path& storage_root,
                                             const std::string& actual_serial,
                                             const std::string& actual_app_version);

void ApplyLegacyPatchesFromXML(std::filesystem::path path);

// Returns the stable identity used by Enumerate/ApplyManagedPatches for a metadata entry.
std::string PatchDefinitionIdentity(const std::string& title, const std::string& name,
                                    const std::string& author, const std::string& app_version,
                                    const std::string& app_elf, const PatchIdentityMap& identities);

// Canonical selector key shared by the identity map and the manifest xml_selector resolver.
std::string CanonicalPatchSelectorKey(const std::string& title, const std::string& name,
                                      const std::string& author, const std::string& app_version,
                                      const std::string& app_elf);

// Returns the set of TitleID IDs declared by a patch XML, without parsing Metadata lines.
std::unordered_set<std::string> EnumeratePatchTitleIds(const std::filesystem::path& xml_path);

// Maps an XML Line Type attribute to a PatchLineType, or std::nullopt if unsupported.
std::optional<PatchLineType> ParsePatchLineType(const std::string& type);

// Returns true for the version-independent mask/mask_jump32 line types.
bool IsMaskPatchType(PatchLineType type);

// Session-scoped guard against applying the same XML through both the managed and legacy
// routes. Managed and legacy callers both consult/record the same set.
bool IsXmlAlreadyApplied(const std::filesystem::path& xml_path);
void MarkXmlApplied(const std::filesystem::path& xml_path);
void ResetAppliedXmlFiles();

// Resets all per-session patch state (applied-XML guard, the legacy queue, and the
// patches-applied latch). Called at the true session boundary (start of Emulator::Run).
void ResetSession();

#ifdef MEMORY_PATCHER_TEST_OBSERVER
// Test-only observer invoked once at the start of PatchMemory. It lets unit tests prove
// selection behavior without redirecting or redesigning the write engine itself. This is
// compiled out of production builds.
using PatchMemoryObserver = void (*)(const std::string& modName);
void SetPatchMemoryObserver(PatchMemoryObserver observer);
PatchMemoryObserver GetPatchMemoryObserver();
#endif

} // namespace MemoryPatcher
