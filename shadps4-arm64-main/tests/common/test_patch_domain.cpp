// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/patch_domain.h"
#include "common/patch_repository.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class TempDir {
public:
    TempDir() {
        auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        temp_path = fs::temp_directory_path() / ("shadps4_patch_domain_test_" + std::to_string(ns) +
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
};

struct ManifestPatchSpec {
    std::string id;
    std::string name;
    std::string author;
};

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
               spec.author + "\" PatchVer=\"1.0\" AppVer=\"01.00\" AppElf=\"eboot.bin\" " +
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
            {"app_versions", json::array({"01.00"})},
            {"category", "performance"},
            {"risk", "low"},
            {"xml_selector",
             {{"title", "Test Game"},
              {"name", spec.name},
              {"author", spec.author},
              {"app_ver", "01.00"},
              {"app_elf", "eboot.bin"}}},
        });
    }
    root["games"]["CUSA00001"]["title"] = "Test Game";
    root["games"]["CUSA00001"]["patch_file"] = "PATCHES/Test.xml";
    root["games"]["CUSA00001"]["versions"] = {"01.00"};
    root["games"]["CUSA00001"]["patches"] = patches;
    return root;
}

void WriteJson(const fs::path& p, const json& j) {
    std::ofstream out(p);
    out << std::setw(2) << j;
}

// Writes a self-consistent repository revision: XML + manifest hashing it.
fs::path WriteRevision(const fs::path& root, const std::string& revision,
                       const std::string& repository_id,
                       const std::vector<XmlPatchSpec>& xml_specs,
                       const std::vector<ManifestPatchSpec>& manifest_specs) {
    const fs::path repo = root / revision;
    fs::create_directories(repo / "PATCHES");
    const auto xml = PatchXml(xml_specs);
    const fs::path xml_path = repo / "PATCHES" / "Test.xml";
    {
        std::ofstream out(xml_path, std::ios::binary);
        out << xml;
    }
    auto manifest = MakeManifest(revision, repository_id, manifest_specs);
    manifest["games"]["CUSA00001"]["sha256"] = PatchRepository::Sha256File(xml_path).value();
    WriteJson(repo / "manifest-v1.json", manifest);
    return repo;
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

json Call(const char* (*fn)(const char*), const std::string& arg) {
    CString out(fn(arg.c_str()));
    return json::parse(out.p);
}

json Call(const char* (*fn)(const char*, const char*, const char*, const char*),
          const std::string& a, const std::string& b, const std::string& c,
          const std::string& d) {
    CString out(fn(a.c_str(), b.c_str(), c.c_str(), d.empty() ? nullptr : d.c_str()));
    return json::parse(out.p);
}

const XmlPatchSpec kPatchA = {"Patch A", "author-a"};
const XmlPatchSpec kPatchB = {"Patch B", "author-b"};
const XmlPatchSpec kPatchC = {"Patch C", "author-c"};
const ManifestPatchSpec kManifestA = {"patch.a", "Patch A", "author-a"};
const ManifestPatchSpec kManifestB = {"patch.b", "Patch B", "author-b"};
const ManifestPatchSpec kManifestC = {"patch.c", "Patch C", "author-c"};

std::string DefaultState(const std::string& serial, const std::string& repo,
                         const std::vector<std::string>& enabled = {},
                         const std::vector<std::string>& disabled = {}) {
    CString out(pd_default_state(serial.c_str(), repo.c_str()));
    auto j = json::parse(out.p);
    j["state"]["enabled_patch_ids"] = enabled;
    j["state"]["disabled_patch_ids"] = disabled;
    return j["state"].dump();
}

} // namespace

TEST(PatchDomainBridgeTest, LoadManifestOk) {
    const TempDir temp;
    const fs::path repo = WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA, kPatchB},
                                        {kManifestA, kManifestB});
    CString out(pd_load_manifest(repo.string().c_str()));
    const auto j = json::parse(out.p);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["manifest"]["repository_id"], "bachata-official");
    EXPECT_EQ(j["manifest"]["revision"], "r1");
}

TEST(PatchDomainBridgeTest, LoadManifestMissing) {
    const TempDir temp;
    CString out(pd_load_manifest((temp.path() / "nope").string().c_str()));
    const auto j = json::parse(out.p);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_FALSE(j["error"].is_null());
}

TEST(PatchDomainBridgeTest, LoadStateMissingReturnsDefault) {
    const TempDir temp;
    const fs::path state_path = temp.path() / "CUSA00001.json";
    CString out(pd_load_state(state_path.string().c_str()));
    const auto j = json::parse(out.p);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_FALSE(j["file_found"].get<bool>());
    EXPECT_TRUE(j["state"]["enabled_patch_ids"].empty());
}

TEST(PatchDomainBridgeTest, LoadStateMalformedFailsSafely) {
    const TempDir temp;
    const fs::path state_path = temp.path() / "CUSA00001.json";
    std::ofstream(state_path) << "{ not valid json !!!";
    CString out(pd_load_state(state_path.string().c_str()));
    const auto j = json::parse(out.p);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_FALSE(j["error"].is_null());
    // Default state is still returned so callers can continue.
    EXPECT_TRUE(j["state"]["enabled_patch_ids"].empty());
}

TEST(PatchDomainBridgeTest, DefaultStateBindsRepository) {
    CString out(pd_default_state("CUSA00001", "shadps4-official"));
    const auto j = json::parse(out.p);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["state"]["serial"], "CUSA00001");
    EXPECT_EQ(j["state"]["repository_id"], "shadps4-official");
}

TEST(PatchDomainBridgeTest, SaveStateRoundTrip) {
    const TempDir temp;
    const fs::path state_path = temp.path() / "CUSA00001.json";
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.a"});
    CString save(pd_save_state(state_path.string().c_str(), state.c_str()));
    EXPECT_TRUE(json::parse(save.p)["ok"].get<bool>());

    CString load(pd_load_state(state_path.string().c_str()));
    const auto j = json::parse(load.p);
    EXPECT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["file_found"].get<bool>());
    EXPECT_EQ(j["state"]["enabled_patch_ids"], json::array({"patch.a"}));
}

TEST(PatchDomainBridgeTest, SaveStateRejectsMalformed) {
    const TempDir temp;
    const fs::path state_path = temp.path() / "CUSA00001.json";
    CString save(pd_save_state(state_path.string().c_str(), "{\"schema\":\"bogus\"}"));
    const auto j = json::parse(save.p);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_FALSE(fs::exists(state_path));
}

TEST(PatchDomainBridgeTest, SaveStateRejectsSerialFilenameMismatch) {
    const TempDir temp;
    const fs::path state_path = temp.path() / "CUSA00001.json";
    CString save(pd_save_state(state_path.string().c_str(),
                               DefaultState("CUSA00002", "bachata-official").c_str()));
    const auto j = json::parse(save.p);
    EXPECT_FALSE(j["ok"].get<bool>());
    EXPECT_FALSE(fs::exists(state_path));
}

TEST(PatchDomainBridgeTest, ResolveMissingStateAllDefaultOff) {
    const TempDir temp;
    const fs::path repo =
        WriteRevision(temp.path(), "r1", "bachata-official",
                      {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC});
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", "");
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_FALSE(j["repository_mismatch"].get<bool>());
    EXPECT_EQ(j["entries"].size(), 3);
    for (const auto& entry : j["entries"]) {
        EXPECT_EQ(entry["status"], "default_off");
    }
    EXPECT_TRUE(j["apply_ids"].empty());
}

TEST(PatchDomainBridgeTest, ResolveEnabledSurfacesEnabled) {
    const TempDir temp;
    const fs::path repo =
        WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA, kPatchB},
                      {kManifestA, kManifestB});
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.a"});
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["apply_ids"], json::array({"patch.a"}));
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.a") {
            EXPECT_EQ(entry["status"], "enabled");
            EXPECT_EQ(entry["name"], "Patch A");
            EXPECT_EQ(entry["author"], "author-a");
            EXPECT_EQ(entry["category"], "performance");
            EXPECT_EQ(entry["risk"], "low");
            EXPECT_EQ(entry["compatibility"], "compatible");
        }
    }
}

TEST(PatchDomainBridgeTest, ResolveDisabledSurfacesDisabled) {
    const TempDir temp;
    const fs::path repo = WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA, kPatchB},
                                        {kManifestA, kManifestB});
    const auto state = DefaultState("CUSA00001", "bachata-official", {}, {"patch.b"});
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["apply_ids"].empty());
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.b") {
            EXPECT_EQ(entry["status"], "disabled");
        }
    }
}

TEST(PatchDomainBridgeTest, ResolveVersionMismatchIncompatible) {
    const TempDir temp;
    const fs::path repo = WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA, kPatchB},
                                        {kManifestA, kManifestB});
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.a"});
    // Installed APP_VER differs from the patch AppVer -> version mismatch, not applied.
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "02.00", state);
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["apply_ids"].empty());
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.a") {
            EXPECT_EQ(entry["status"], "incompatible");
            EXPECT_EQ(entry["compatibility"], "version_mismatch");
        }
    }
}

TEST(PatchDomainBridgeTest, ResolveUnavailableIdPreserved) {
    const TempDir temp;
    const fs::path repo = WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA, kPatchB},
                                        {kManifestA, kManifestB});
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.c"});
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["apply_ids"].empty());
    bool saw_unavailable = false;
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.c") {
            EXPECT_EQ(entry["status"], "unavailable");
            saw_unavailable = true;
        }
    }
    EXPECT_TRUE(saw_unavailable);
}

TEST(PatchDomainBridgeTest, ResolveRepositoryMismatch) {
    const TempDir temp;
    const fs::path repo =
        WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA}, {kManifestA});
    // State written for a different repository, but containing the same patch ID.
    const auto foreign = DefaultState("CUSA00001", "shadps4-official", {"patch.a"});
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", foreign);
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_TRUE(j["repository_mismatch"].get<bool>());
    EXPECT_TRUE(j["entries"].empty());
    EXPECT_TRUE(j["apply_ids"].empty());
}

TEST(PatchDomainBridgeTest, ResolveEffectiveEndToEndBoundary) {
    // The Milestone 4 boundary test: an Android/domain request of CUSA00001 + APP_VER 01.00,
    // a local manifest, and a state file enabling patch.a must resolve to patch.a = Enabled
    // with effective apply IDs exactly {"patch.a"}.
    const TempDir temp;
    const fs::path repo =
        WriteRevision(temp.path(), "r1", "bachata-official", {kPatchA, kPatchB},
                      {kManifestA, kManifestB});
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.a"});
    const auto j = Call(pd_resolve_effective, repo.string(), "CUSA00001", "01.00", state);
    ASSERT_TRUE(j["ok"].get<bool>());
    EXPECT_EQ(j["repository_id"], "bachata-official");
    EXPECT_EQ(j["repository_revision"], "r1");
    EXPECT_EQ(j["apply_ids"], json::array({"patch.a"}));
    json expected;
    expected["id"] = "patch.a";
    expected["status"] = "enabled";
    bool found = false;
    for (const auto& entry : j["entries"]) {
        if (entry["id"] == "patch.a") {
            found = true;
            EXPECT_EQ(entry["status"], "enabled");
        }
    }
    EXPECT_TRUE(found);
}