// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/memory_patcher.h"
#include "common/patch_domain.h"
#include "common/patch_preset.h"
#include "common/patch_repository.h"
#include "common/patch_session.h"
#include "common/singleton.h"
#include "common/types.h"
#include "core/file_format/psf.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class TempDir {
public:
    TempDir() {
        auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        temp_path = fs::temp_directory_path() / ("shadps4_preset_test_" + std::to_string(ns) +
                                                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
        fs::create_directories(temp_path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(temp_path, ec);
    }
    const fs::path& path() const {
        return temp_path;
    }

private:
    fs::path temp_path;
};

struct XmlPatchSpec {
    std::string name;
    std::string author;
    std::string app_ver = "01.00";
};

struct ManifestPatchSpec {
    std::string id;
    std::string name;
    std::string author;
    std::string app_ver = "01.00";
};

struct PresetSpec {
    std::string id;
    std::string name;
    std::string description;
    std::vector<std::string> patch_ids;
};

constexpr const char* kPresetsRelPath = "PRESETS/presets.json";

std::string PatchXml(const std::vector<XmlPatchSpec>& specs) {
    std::string xml =
        "<Patch>\n"
        "    <TitleID>\n"
        "        <ID>CUSA00001</ID>\n"
        "    </TitleID>\n";
    const std::vector<std::string> addresses = {"400000", "400004", "400008", "40000C"};
    size_t i = 0;
    for (const auto& spec : specs) {
        xml += "    <Metadata Title=\"Test Game\" Name=\"" + spec.name + "\" Author=\"" +
               spec.author + "\" PatchVer=\"1.0\" AppVer=\"" + spec.app_ver +
               "\" AppElf=\"eboot.bin\" " +
               "isEnabled=\"false\">\n"
               "        <PatchList><Line Type=\"bytes32\" Address=\"" +
               addresses[i % addresses.size()] + "\" Value=\"00000001\"/></PatchList>\n"
               "    </Metadata>\n";
        ++i;
    }
    xml += "</Patch>\n";
    return xml;
}

json MakeManifest(const std::string& revision, const std::string& repository_id,
                  const std::vector<ManifestPatchSpec>& specs) {
    json root;
    root["schema"] = 1;
    root["repository_id"] = repository_id;
    root["revision"] = revision;
    root["generated_at"] = "2026-08-19T00:00:00Z";
    json patches = json::array();
    for (const auto& spec : specs) {
        patches.push_back({
            {"id", spec.id},
            {"name", spec.name},
            {"author", spec.author},
            {"patch_version", "1.0"},
            {"app_versions", json::array({spec.app_ver})},
            {"category", "performance"},
            {"risk", "low"},
            {"xml_selector",
             {{"title", "Test Game"},
              {"name", spec.name},
              {"author", spec.author},
              {"app_ver", spec.app_ver},
              {"app_elf", "eboot.bin"}}},
        });
    }
    root["games"]["CUSA00001"]["title"] = "Test Game";
    root["games"]["CUSA00001"]["patch_file"] = "PATCHES/Test.xml";
    root["games"]["CUSA00001"]["versions"] = {"01.00"};
    root["games"]["CUSA00001"]["patches"] = patches;
    return root;
}

json MakePresetsJson(const std::vector<PresetSpec>& presets, const std::string& serial = "CUSA00001") {
    json root;
    root["schema"] = 1;
    root["serial"] = serial;
    json arr = json::array();
    for (const auto& spec : presets) {
        arr.push_back({
            {"id", spec.id},
            {"name", spec.name},
            {"description", spec.description},
            {"patch_ids", spec.patch_ids},
        });
    }
    root["presets"] = arr;
    return root;
}

void WriteJson(const fs::path& p, const json& j) {
    std::ofstream out(p);
    out << std::setw(2) << j;
}

// Writes a self-consistent repository revision: XML + presets + manifest hashing both. The
// presets file is pinned by SHA-256 exactly like the patch XML, so the manifest can only trust
// a presets file it hashes itself.
fs::path WritePresetRepo(const fs::path& root, const std::string& revision,
                         const std::string& repository_id,
                         const std::vector<XmlPatchSpec>& xml_specs,
                         const std::vector<ManifestPatchSpec>& manifest_specs,
                         const std::vector<PresetSpec>& presets,
                         const std::string& presets_serial = "CUSA00001") {
    const fs::path repo = root / revision;
    fs::create_directories(repo / "PATCHES");
    fs::create_directories(repo / "PRESETS");
    const auto xml = PatchXml(xml_specs);
    const fs::path xml_path = repo / "PATCHES" / "Test.xml";
    {
        std::ofstream out(xml_path, std::ios::binary);
        out << xml;
    }
    const fs::path presets_path = repo / kPresetsRelPath;
    WriteJson(presets_path, MakePresetsJson(presets, presets_serial));

    auto manifest = MakeManifest(revision, repository_id, manifest_specs);
    manifest["games"]["CUSA00001"]["sha256"] = PatchRepository::Sha256File(xml_path).value();
    manifest["games"]["CUSA00001"]["presets_file"] = kPresetsRelPath;
    manifest["games"]["CUSA00001"]["presets_sha256"] =
        PatchRepository::Sha256File(presets_path).value();
    WriteJson(repo / "manifest-v1.json", manifest);
    return repo;
}

// Loads the CUSA00001 game entry from the written repository.
PatchRepository::ManifestGameEntry GameEntry(const fs::path& repo) {
    auto loaded = PatchRepository::LoadManifest(repo / "manifest-v1.json");
    EXPECT_TRUE(loaded.ok) << loaded.error;
    return loaded.manifest.games.at("CUSA00001");
}

// Owned string helper so tests never leak pd_* strings.
struct CString {
    const char* p = nullptr;
    explicit CString(const char* s) : p(s) {}
    CString(const CString&) = delete;
    CString& operator=(const CString&) = delete;
    CString(CString&& other) noexcept : p(other.p) { other.p = nullptr; }
    ~CString() {
        if (p) {
            pd_free_string(p);
        }
    }
};

json Call(const char* (*fn)(const char*, const char*, const char*, const char*),
          const std::string& a, const std::string& b, const std::string& c,
          const std::string& d) {
    CString out(fn(a.c_str(), b.c_str(), c.c_str(), d.empty() ? nullptr : d.c_str()));
    return json::parse(out.p);
}

const XmlPatchSpec kPatchA = {"Patch A", "author-a"};
const XmlPatchSpec kPatchB = {"Patch B", "author-b"};
const XmlPatchSpec kPatchC = {"Patch C", "author-c"};
const XmlPatchSpec kPatchD = {"Patch D", "author-d"};
const ManifestPatchSpec kManifestA = {"patch.a", "Patch A", "author-a"};
const ManifestPatchSpec kManifestB = {"patch.b", "Patch B", "author-b"};
const ManifestPatchSpec kManifestC = {"patch.c", "Patch C", "author-c"};
const ManifestPatchSpec kManifestD = {"patch.d", "Patch D", "author-d"};

const PresetSpec kMobilePerformance = {"mobile-performance", "Mobile Performance",
                                       "Mobile-optimized settings", {"patch.a", "patch.c"}};
const PresetSpec kPostProcessing = {"post-processing", "Post-Processing",
                                    "SSAO/DoF/AA reduction", {"patch.c"}};
const PresetSpec kMobileAll = {"mobile-performance", "Mobile Performance",
                               "Mobile-optimized settings", {"patch.a", "patch.b", "patch.c"}};
const PresetSpec kMobileWithD = {"mobile-performance", "Mobile Performance",
                                 "Mobile-optimized settings", {"patch.a", "patch.b", "patch.c", "patch.d"}};
const PresetSpec kMobileWithoutD = {"mobile-performance", "Mobile Performance",
                                    "Mobile-optimized settings", {"patch.a", "patch.b", "patch.c"}};

std::string DefaultState(const std::string& serial, const std::string& repo,
                         const std::vector<std::string>& enabled = {},
                         const std::vector<std::string>& disabled = {}) {
    CString out(pd_default_state(serial.c_str(), repo.c_str()));
    auto j = json::parse(out.p);
    j["state"]["enabled_patch_ids"] = enabled;
    j["state"]["disabled_patch_ids"] = disabled;
    return j["state"].dump();
}

std::string DefaultStateWithPreset(const std::string& serial, const std::string& repo,
                                   const std::string& preset_id,
                                   const std::vector<std::string>& enabled = {},
                                   const std::vector<std::string>& disabled = {}) {
    auto state = DefaultState(serial, repo, enabled, disabled);
    auto j = json::parse(state);
    j["selected_preset"] = preset_id;
    return j.dump();
}

} // namespace

// ---- Preset file loading (Part A protections) ----

TEST(PatchPresetTest, ValidPresetLoads) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance, kPostProcessing});

    const auto result = PatchPreset::Load(GameEntry(repo), repo);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.presets.size(), 2);
    EXPECT_EQ(result.presets[0].id, "mobile-performance");
    EXPECT_EQ(result.presets[0].name, "Mobile Performance");
    EXPECT_EQ(result.presets[0].patch_ids, std::vector<std::string>({"patch.a", "patch.c"}));
    EXPECT_EQ(result.presets[1].id, "post-processing");
}

TEST(PatchPresetTest, PresetShaMismatchRejected) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});

    // Presets file changed after the manifest pinned its SHA-256.
    WriteJson(repo / kPresetsRelPath,
              MakePresetsJson({PresetSpec{"tampered", "Tampered", "", {"patch.a"}}}));

    const auto result = PatchPreset::Load(GameEntry(repo), repo);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("SHA-256 mismatch"), std::string::npos) << result.error;
}

TEST(PatchPresetTest, PresetSerialMismatchRejected) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance}, "CUSA99999");

    const auto result = PatchPreset::Load(GameEntry(repo), repo);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("serial"), std::string::npos) << result.error;
}

TEST(PatchPresetTest, DuplicatePresetIdRejected) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance, kMobilePerformance});

    const auto result = PatchPreset::Load(GameEntry(repo), repo);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("duplicate preset"), std::string::npos) << result.error;
}

TEST(PatchPresetTest, PresetUnknownPatchIdRejected) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {PresetSpec{"bad", "Bad", "", {"patch.nope"}}});

    const auto result = PatchPreset::Load(GameEntry(repo), repo);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unknown patch"), std::string::npos) << result.error;
}

TEST(PatchPresetTest, PresetFileRequiresShaPin) {
    // A presets_file without a presets_sha256 pin must be rejected by the manifest parser:
    // an unhashed presets file is untrusted and must never be read.
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    auto manifest = json::parse(std::ifstream(repo / "manifest-v1.json"));
    manifest["games"]["CUSA00001"].erase("presets_sha256");
    WriteJson(repo / "manifest-v1.json", manifest);

    const auto loaded = PatchRepository::LoadManifest(repo / "manifest-v1.json");
    EXPECT_FALSE(loaded.ok);
}

TEST(PatchPresetTest, UnknownPresetRejectedSafely) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto entry = GameEntry(repo);

    const auto none = PatchPreset::ResolveBase(entry, repo, std::nullopt);
    EXPECT_TRUE(none.ok);
    EXPECT_TRUE(none.base_ids.empty());
    EXPECT_TRUE(none.preset_id.empty());

    const auto unknown = PatchPreset::ResolveBase(entry, repo, std::string("nope"));
    EXPECT_FALSE(unknown.ok);
    EXPECT_NE(unknown.error.find("unknown preset"), std::string::npos) << unknown.error;
}

// ---- Effective selection with a preset (Part B) ----

TEST(PatchPresetTest, SelectedPresetProvidesBaseIds) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance");

    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    EXPECT_EQ(j["selected_preset"], "mobile-performance");
    // patch.a and patch.c come from the preset base; nothing is explicitly enabled.
    EXPECT_EQ(j["apply_ids"], json::array({"patch.a", "patch.c"}));
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.b") {
            EXPECT_EQ(entry["status"], "default_off");
        }
    }
}

TEST(PatchPresetTest, ExplicitEnableAddsToPreset) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance",
                                              {"patch.b"});

    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    // Preset base [a,c] + explicit enable [b] -> all three applied.
    EXPECT_EQ(j["apply_ids"], json::array({"patch.a", "patch.b", "patch.c"}));
}

TEST(PatchPresetTest, ExplicitDisableOverridesPreset) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance",
                                              {}, {"patch.a"});

    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    // Explicit disable of patch.a wins over the preset base; patch.c still applies.
    EXPECT_EQ(j["apply_ids"], json::array({"patch.c"}));
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.a") {
            EXPECT_EQ(entry["status"], "disabled");
        }
    }
}

TEST(PatchPresetTest, PresetRevisionAddsPatchWithoutLosingOverride) {
    const TempDir temp;
    // r1: preset = A B C. User explicitly disabled B.
    const fs::path repo_r1 =
        WritePresetRepo(temp.path() / "r1", "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobileAll});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance",
                                              {}, {"patch.b"});

    const auto first = Call(pd_resolve_effective, repo_r1.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(first["ok"].get<bool>()) << first.value("error", "?");
    EXPECT_EQ(first["apply_ids"], json::array({"patch.a", "patch.c"}));

    // r2: preset grows to A B C D. The explicit B override must survive the update, and D
    // joins through the new preset base.
    const fs::path repo_r2 =
        WritePresetRepo(temp.path() / "r2", "r2", "bachata-official",
                        {kPatchA, kPatchB, kPatchC, kPatchD},
                        {kManifestA, kManifestB, kManifestC, kManifestD}, {kMobileWithD});
    const auto second = Call(pd_resolve_effective, repo_r2.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(second["ok"].get<bool>()) << second.value("error", "?");
    EXPECT_EQ(second["apply_ids"], json::array({"patch.a", "patch.c", "patch.d"}));
    for (const auto& entry : second["entries"]) {
        if (entry["id"] == "patch.b") {
            EXPECT_EQ(entry["status"], "disabled");
        }
    }
}

TEST(PatchPresetTest, PresetRemovedPatchBecomesUnavailable) {
    const TempDir temp;
    // r1: preset includes D. r2: the curators drop D from the preset (patch.d still exists in
    // the manifest, but is no longer part of the preset base).
    const fs::path repo_r1 =
        WritePresetRepo(temp.path() / "r1", "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC, kPatchD},
                        {kManifestA, kManifestB, kManifestC, kManifestD}, {kMobileWithD});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance");

    const auto first = Call(pd_resolve_effective, repo_r1.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(first["ok"].get<bool>()) << first.value("error", "?");
    EXPECT_EQ(first["apply_ids"], json::array({"patch.a", "patch.b", "patch.c", "patch.d"}));

    const fs::path repo_r2 =
        WritePresetRepo(temp.path() / "r2", "r2", "bachata-official",
                        {kPatchA, kPatchB, kPatchC, kPatchD},
                        {kManifestA, kManifestB, kManifestC, kManifestD}, {kMobileWithoutD});
    const auto second = Call(pd_resolve_effective, repo_r2.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(second["ok"].get<bool>()) << second.value("error", "?");
    EXPECT_EQ(second["apply_ids"], json::array({"patch.a", "patch.b", "patch.c"}));
    for (const auto& entry : second["entries"]) {
        if (entry["id"] == "patch.d") {
            EXPECT_EQ(entry["status"], "default_off");
        }
    }
}

TEST(PatchPresetTest, NoSelectedPresetPreservesM3Behavior) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.b"});

    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    // Presets exist but none selected: only the explicit enable applies, exactly as before
    // presets existed. The preset base must not leak in.
    EXPECT_TRUE(j["selected_preset"].is_null());
    EXPECT_EQ(j["apply_ids"], json::array({"patch.b"}));
}

TEST(PatchPresetTest, UnknownSelectedPresetRejectsResolution) {
    const TempDir temp;
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "nope");

    // An unknown selected preset is rejected deterministically: fail safe instead of guessing.
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_NE(j.value("error", "").find("unknown preset"), std::string::npos);
}

// ---- Per-preset APP_VER compatibility metadata ----

TEST(PatchPresetTest, ResolveSurfacesPerPresetVersionCompatibility) {
    const TempDir temp;
    // patch.a targets 01.00 only; patch.c targets 02.00 only.
    const XmlPatchSpec kNewC = {"Patch C", "author-c", "02.00"};
    const ManifestPatchSpec kNewManifestC = {"patch.c", "Patch C", "author-c", "02.00"};
    const PresetSpec kMobileOnly = {"mobile", "Mobile", "", {"patch.a"}};
    const PresetSpec kLegacyOnly = {"legacy", "Legacy", "", {"patch.c"}};
    const PresetSpec kMixed = {"mixed", "Mixed", "", {"patch.a", "patch.c"}};
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kNewC}, {kManifestA, kNewManifestC},
                        {kMobileOnly, kLegacyOnly, kMixed});

    const auto at_100 = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00",
                             DefaultState("CUSA00001", "bachata-official"));
    ASSERT_TRUE(at_100["ok"].get<bool>()) << at_100.value("error", "?");
    std::unordered_map<std::string, json> by_id;
    for (const auto& p : at_100["presets"]) {
        by_id[p["id"].get<std::string>()] = p;
    }
    EXPECT_TRUE(by_id.at("mobile")["compatible"].get<bool>());
    EXPECT_EQ(by_id.at("mobile")["compatible_patch_count"], 1);
    EXPECT_FALSE(by_id.at("legacy")["compatible"].get<bool>());
    EXPECT_EQ(by_id.at("legacy")["compatible_patch_count"], 0);
    // Partial support is not compatible: one referenced patch does not match this version.
    EXPECT_FALSE(by_id.at("mixed")["compatible"].get<bool>());
    EXPECT_EQ(by_id.at("mixed")["compatible_patch_count"], 1);

    const auto at_200 = Call(pd_resolve_effective, repo.string(), "CUSA00001", "02.00",
                             DefaultState("CUSA00001", "bachata-official"));
    ASSERT_TRUE(at_200["ok"].get<bool>()) << at_200.value("error", "?");
    by_id.clear();
    for (const auto& p : at_200["presets"]) {
        by_id[p["id"].get<std::string>()] = p;
    }
    EXPECT_FALSE(by_id.at("mobile")["compatible"].get<bool>());
    EXPECT_EQ(by_id.at("mobile")["compatible_patch_count"], 0);
    EXPECT_TRUE(by_id.at("legacy")["compatible"].get<bool>());
    EXPECT_EQ(by_id.at("legacy")["compatible_patch_count"], 1);
}

TEST(PatchPresetTest, SelectedUnsupportedPresetFailsOpenAtLaunch) {
    const TempDir temp;
    const XmlPatchSpec kNewC = {"Patch C", "author-c", "02.00"};
    const ManifestPatchSpec kNewManifestC = {"patch.c", "Patch C", "author-c", "02.00"};
    const PresetSpec kMobileOnly = {"mobile", "Mobile", "", {"patch.a"}};
    const fs::path repo =
        WritePresetRepo(temp.path(), "r1", "bachata-official",
                        {kPatchA, kNewC}, {kManifestA, kNewManifestC}, {kMobileOnly});
    const auto state =
        DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile");

    // A preset persisted under an older game version must not brick resolution after an
    // update: launch fails open unpatched while keeping the selection recoverable.
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "02.00", state);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    EXPECT_EQ(j["selected_preset"], "mobile");
    EXPECT_TRUE(j["apply_ids"].empty());
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.a") {
            EXPECT_EQ(entry["status"], "incompatible");
        }
    }
}

// ---- Session freeze (Part C) + full chain ----

class PresetSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir = std::make_unique<TempDir>();
        Common::Singleton<PSF>::Instance()->AddString("APP_VER", "01.00");

        backing.resize(4096, 0x90);
        MemoryPatcher::g_eboot_address = reinterpret_cast<uintptr_t>(backing.data());
        MemoryPatcher::g_eboot_image_size = backing.size();
        MemoryPatcher::g_managed_session_path.clear();
        MemoryPatcher::g_managed_storage_root.clear();
        MemoryPatcher::SetPatchMemoryObserver(nullptr);
    }

    void TearDown() override {
        MemoryPatcher::SetPatchMemoryObserver(nullptr);
        MemoryPatcher::ResetSession();
        MemoryPatcher::g_eboot_address = 0;
        MemoryPatcher::g_eboot_image_size = 0;
        MemoryPatcher::g_managed_session_path.clear();
        MemoryPatcher::g_managed_storage_root.clear();
        temp_dir.reset();
    }

    static std::vector<std::string>& Observed() {
        static std::vector<std::string> observed;
        return observed;
    }

    static void Observe(const std::string& modName) {
        Observed().push_back(modName);
    }

    void InstallObserver() {
        Observed().clear();
        MemoryPatcher::SetPatchMemoryObserver(&PresetSessionTest::Observe);
    }

    // Builds the frozen launch snapshot exactly as Android would (pd_build_session) and parses
    // it back into the runtime Config.
    PatchSession::Config BuildSession(const fs::path& repo, const std::string& serial,
                                      const std::string& app_ver,
                                      const std::string& state_json) {
        CString out(pd_build_session(repo.string().c_str(), serial.c_str(), app_ver.c_str(),
                                     state_json.c_str()));
        auto j = json::parse(out.p);
        EXPECT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
        const auto parsed = PatchSession::Parse(j["session"].dump());
        EXPECT_TRUE(parsed.ok) << parsed.error;
        return parsed.config;
    }

    std::unique_ptr<TempDir> temp_dir;
    std::vector<u8> backing;
};

TEST_F(PresetSessionTest, SessionFreezesResolvedPresetIds) {
    const fs::path repo =
        WritePresetRepo(temp_dir->path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance",
                                              {}, {"patch.a"});

    const auto config = BuildSession(repo, "CUSA00001", "01.00", state);

    // The session freezes the RESOLVED IDs: preset base [a,c] minus explicit disable [a].
    EXPECT_EQ(config.enabled_patch_ids, std::vector<std::string>({"patch.c"}));
    EXPECT_EQ(config.selected_preset, "mobile-performance");

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch C"}));
}

TEST_F(PresetSessionTest, PresetChangeAfterSessionDoesNotAffectRunningSession) {
    const fs::path repo =
        WritePresetRepo(temp_dir->path(), "r1", "bachata-official",
                        {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC},
                        {kMobilePerformance});
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "mobile-performance");
    const auto config = BuildSession(repo, "CUSA00001", "01.00", state);
    ASSERT_EQ(config.enabled_patch_ids, std::vector<std::string>({"patch.a", "patch.c"}));

    // After the session was staged, the repository's presets file is replaced entirely. A
    // fresh resolution now fails, but the staged session must apply its frozen IDs unchanged:
    // the runtime never re-reads the presets file at apply time.
    WriteJson(repo / kPresetsRelPath, json{{"schema", 1}, {"serial", "CUSA00001"}, {"presets", json::array()}});
    const auto re_resolve = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_FALSE(re_resolve["ok"].get<bool>());

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A", "Patch C"}));
}

TEST_F(PresetSessionTest, PresetFullChainAppliesResolvedIdsOnce) {
    // Full Milestone 6 chain: local pilot repository layout -> selected_preset ->
    // resolve -> state overrides -> pd_build_session -> frozen PatchSessionConfig -> runtime ->
    // the exact resulting IDs are applied exactly once.
    const fs::path repo = WritePresetRepo(
        temp_dir->path() / "patches" / "repository" / "bachata-bloodborne-pilot", "r1",
        "bachata-bloodborne-pilot", {kPatchA, kPatchB, kPatchC},
        {kManifestA, kManifestB, kManifestC}, {kMobilePerformance});
    // Explicit disable beats the preset base; an explicit enable adds a patch outside it.
    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-bloodborne-pilot",
                                              "mobile-performance", {"patch.b"}, {"patch.a"});

    CString out(pd_build_session(repo.string().c_str(), "CUSA00001", "01.00", state.c_str()));
    const auto j = json::parse(out.p);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    const auto& session = j["session"];
    EXPECT_EQ(session["schema"], 1);
    EXPECT_EQ(session["repository_id"], "bachata-bloodborne-pilot");
    EXPECT_EQ(session["selected_preset"], "mobile-performance");
    // Resolved IDs: patch.a explicitly disabled, patch.b explicitly enabled, patch.c from the
    // preset base. The preset itself appears only as diagnostics.
    EXPECT_EQ(session["enabled_patch_ids"], json::array({"patch.b", "patch.c"}));

    const auto parsed = PatchSession::Parse(session.dump());
    ASSERT_TRUE(parsed.ok) << parsed.error;

    InstallObserver();
    const auto result =
        MemoryPatcher::ApplyManagedSession(parsed.config, repo, "CUSA00001", "01.00");
    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch B", "Patch C"}));

    // Re-applying the same frozen session within the same launch must not double-write.
    const auto again = MemoryPatcher::ApplyManagedSession(parsed.config, repo, "CUSA00001", "01.00");
    EXPECT_FALSE(again.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch B", "Patch C"}));
}

TEST_F(PresetSessionTest, OverlappingPatchAddressesApplyInXmlOrder) {
    // Model LOD 2 (Lowest) and Performance Patch both touch 0x0216fc09 in Bloodborne.xml.
    // When both are enabled via preset, linear XML document order ensures that the later
    // patch's value overwrites the earlier one (LOD 2 wins over Performance Patch).
    const fs::path repo = temp_dir->path() / "r1";
    fs::create_directories(repo / "PATCHES");
    fs::create_directories(repo / "PRESETS");

    const std::string xml =
        "<Patch>\n"
        "    <TitleID><ID>CUSA00001</ID></TitleID>\n"
        "    <Metadata Title=\"Test Game\" Name=\"Performance Patch\" Author=\"Author1\" "
        "PatchVer=\"1.0\" AppVer=\"01.00\" AppElf=\"eboot.bin\" isEnabled=\"false\">\n"
        "        <PatchList><Line Type=\"bytes32\" Address=\"400000\" Value=\"11111111\"/></PatchList>\n"
        "    </Metadata>\n"
        "    <Metadata Title=\"Test Game\" Name=\"Model LOD 2\" Author=\"Author2\" "
        "PatchVer=\"1.0\" AppVer=\"01.00\" AppElf=\"eboot.bin\" isEnabled=\"false\">\n"
        "        <PatchList><Line Type=\"bytes32\" Address=\"400000\" Value=\"22222222\"/></PatchList>\n"
        "    </Metadata>\n"
        "</Patch>\n";

    const fs::path xml_path = repo / "PATCHES" / "Test.xml";
    {
        std::ofstream out(xml_path, std::ios::binary);
        out << xml;
    }

    const std::vector<PresetSpec> presets = {
        {"perf-lod", "Perf + LOD2 Preset", "Preset with overlapping writes", {"patch.perf", "patch.lod2"}}
    };
    const fs::path presets_path = repo / kPresetsRelPath;
    WriteJson(presets_path, MakePresetsJson(presets, "CUSA00001"));

    const std::vector<ManifestPatchSpec> manifest_specs = {
        {"patch.perf", "Performance Patch", "Author1"},
        {"patch.lod2", "Model LOD 2", "Author2"},
    };
    auto manifest = MakeManifest("r1", "bachata-official", manifest_specs);
    manifest["games"]["CUSA00001"]["sha256"] = PatchRepository::Sha256File(xml_path).value();
    manifest["games"]["CUSA00001"]["presets_file"] = kPresetsRelPath;
    manifest["games"]["CUSA00001"]["presets_sha256"] =
        PatchRepository::Sha256File(presets_path).value();
    WriteJson(repo / "manifest-v1.json", manifest);

    const auto state = DefaultStateWithPreset("CUSA00001", "bachata-official", "perf-lod");
    const auto config = BuildSession(repo, "CUSA00001", "01.00", state);

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Performance Patch", "Model LOD 2"}));

    // Backing memory at offset 0 (0x400000) must contain the bytes written by Model LOD 2 (0x22, 0x22, 0x22, 0x22)
    EXPECT_EQ(backing[0], 0x22);
    EXPECT_EQ(backing[1], 0x22);
    EXPECT_EQ(backing[2], 0x22);
    EXPECT_EQ(backing[3], 0x22);
}