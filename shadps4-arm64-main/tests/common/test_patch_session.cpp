// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/memory_patcher.h"
#include "common/patch_domain.h"
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
        temp_path = fs::temp_directory_path() / ("shadps4_patch_session_test_" + std::to_string(ns) +
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

// Writes a self-consistent repository: XML + manifest hashing it. Returns the repository root
// (the directory that must contain manifest-v1.json).
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

struct CString {
    const char* p = nullptr;
    explicit CString(const char* s) : p(s) {}
    CString(const CString&) = delete;
    CString& operator=(const CString&) = delete;
    ~CString() {
        if (p) {
            pd_free_string(p);
        }
    }
};

std::string DefaultState(const std::string& serial, const std::string& repo,
                         const std::vector<std::string>& enabled = {}) {
    CString out(pd_default_state(serial.c_str(), repo.c_str()));
    auto j = json::parse(out.p);
    j["state"]["enabled_patch_ids"] = enabled;
    return j["state"].dump();
}

const XmlPatchSpec kPatchA = {"Patch A", "author-a"};
const XmlPatchSpec kPatchB = {"Patch B", "author-b"};
const ManifestPatchSpec kManifestA = {"patch.a", "Patch A", "author-a"};
const ManifestPatchSpec kManifestB = {"patch.b", "Patch B", "author-b"};

} // namespace

class PatchSessionTest : public ::testing::Test {
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
        MemoryPatcher::SetPatchMemoryObserver(&PatchSessionTest::Observe);
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

TEST_F(PatchSessionTest, ValidSessionAppliesSelectedPatch) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));
}

TEST_F(PatchSessionTest, NoEnabledIdsLaunchesUnpatched) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official"));

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, SessionStateIsFrozenAfterCreation) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    // Android stages the snapshot, then the user toggles patch.b on.
    const auto staged = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));
    const fs::path session_file = temp_dir->path() / "session.json";
    {
        std::ofstream out(session_file);
        out << PatchSession::Serialize(staged);
    }
    const auto fresh = BuildSession(repo, "CUSA00001", "01.00",
                                    DefaultState("CUSA00001", "bachata-official",
                                                 {"patch.a", "patch.b"}));
    ASSERT_EQ(fresh.enabled_patch_ids,
              std::vector<std::string>({"patch.a", "patch.b"}));

    // The runtime applies the frozen snapshot, not the current user state.
    const auto parsed = PatchSession::LoadFromFile(session_file);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(parsed.config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));
}

TEST_F(PatchSessionTest, PatchXmlChangedAfterSnapshotRejected) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    // Repository updated after the snapshot was pinned: the XML bytes change.
    {
        std::ofstream out(repo / "PATCHES" / "Test.xml", std::ios::app);
        out << "\n<!-- updated after snapshot -->\n";
    }

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::Sha256Mismatch);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, PatchShaMismatchRejected) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    auto config = BuildSession(repo, "CUSA00001", "01.00",
                               DefaultState("CUSA00001", "bachata-official", {"patch.a"}));
    config.patch_sha256 = std::string(64, '0');

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::Sha256Mismatch);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, SessionSerialMismatchRejected) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00002", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::SerialMismatch);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, SessionAppVersionMismatchRejected) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "02.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::AppVersionMismatch);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, RepositoryIdMismatchRejected) {
    const fs::path repo_a = WriteRevision(temp_dir->path() / "repo_a", "r1", "bachata-official",
                                          {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const fs::path repo_b = WriteRevision(temp_dir->path() / "repo_b", "r1", "other-repository",
                                          {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo_a, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo_b, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::RepositoryMismatch);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, RepositoryRevisionMismatchRejected) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    // Repository advanced to a new revision after the snapshot was pinned.
    auto manifest = MakeManifest("r2", "bachata-official", {kManifestA, kManifestB});
    manifest["games"]["CUSA00001"]["sha256"] =
        PatchRepository::Sha256File(repo / "PATCHES" / "Test.xml").value();
    WriteJson(repo / "manifest-v1.json", manifest);

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::RevisionMismatch);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, UnknownSelectedIdSkippedSafely) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    auto config = BuildSession(repo, "CUSA00001", "01.00",
                               DefaultState("CUSA00001", "bachata-official", {"patch.a"}));
    config.enabled_patch_ids = {"patch.nope"};

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, ManagedPatchAppliedExactlyOnce) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto first = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");
    const auto second = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(first.apply.ok);
    EXPECT_FALSE(second.apply.ok); // guard: the XML already applied this session
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));
}

TEST_F(PatchSessionTest, LegacyAndManagedCannotDoubleApply) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto managed = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");
    ASSERT_TRUE(managed.apply.ok);
    EXPECT_TRUE(MemoryPatcher::IsXmlAlreadyApplied(repo / "PATCHES" / "Test.xml"));

    // The legacy auto-scan must not re-apply the same XML that the managed session applied.
    MemoryPatcher::ApplyLegacyPatchesFromXML(repo / "PATCHES" / "Test.xml");
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));
}

TEST_F(PatchSessionTest, NewSessionResetsAppliedFileGuard) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));

    InstallObserver();
    const auto first = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");
    ASSERT_TRUE(first.apply.ok);

    // A new launch is a new session: the guard resets and managed patches may apply again.
    MemoryPatcher::ResetSession();
    const auto second = MemoryPatcher::ApplyManagedSession(config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(second.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A", "Patch A"}));
}

TEST_F(PatchSessionTest, MalformedSessionFailsOpenUnpatched) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const fs::path session_file = temp_dir->path() / "broken.json";
    {
        std::ofstream out(session_file);
        out << "{ not a session !!";
    }

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSessionFile(session_file, temp_dir->path(),
                                                               "CUSA00001", "01.00");

    EXPECT_FALSE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::MalformedSession);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, MissingSessionLaunchesNormally) {
    const fs::path repo = WriteRevision(temp_dir->path(), "r1", "bachata-official",
                                        {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const fs::path session_file = temp_dir->path() / "nope" / "CUSA00001.json";

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSessionFile(session_file, temp_dir->path(),
                                                               "CUSA00001", "01.00");

    EXPECT_FALSE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::MissingSession);
    EXPECT_FALSE(result.apply.ok);
    EXPECT_TRUE(Observed().empty());
}

TEST_F(PatchSessionTest, ManagedSessionFileDerivesRepositoryFromStorageRoot) {
    // Android layout: <storage>/patches/repository/<id> contains manifest-v1.json, and
    // <storage>/patches/session/<serial> holds the staged session snapshot.
    const fs::path repo = WriteRevision(temp_dir->path() / "patches" / "repository",
                                        "bachata-official", "bachata-official", {kPatchA, kPatchB},
                                        {kManifestA, kManifestB});
    const fs::path session_dir = temp_dir->path() / "patches" / "session";
    fs::create_directories(session_dir);
    const auto config = BuildSession(repo, "CUSA00001", "01.00",
                                     DefaultState("CUSA00001", "bachata-official", {"patch.a"}));
    const fs::path session_file = session_dir / "CUSA00001.json";
    {
        std::ofstream out(session_file);
        out << PatchSession::Serialize(config);
    }

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSessionFile(session_file, temp_dir->path(),
                                                               "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));
}

TEST_F(PatchSessionTest, ManagedLaunchBoundary) {
    // Full boundary: default state -> pd_build_session -> frozen session JSON -> parse ->
    // ApplyManagedSession -> PatchMemory, with the repository at the runtime's storage layout.
    const fs::path repo = WriteRevision(temp_dir->path() / "patches" / "repository",
                                        "bachata-official", "bachata-official", {kPatchA, kPatchB},
                                        {kManifestA, kManifestB});
    const auto state = DefaultState("CUSA00001", "bachata-official", {"patch.a"});

    CString out(pd_build_session(repo.string().c_str(), "CUSA00001", "01.00", state.c_str()));
    const auto j = json::parse(out.p);
    ASSERT_TRUE(j["ok"].get<bool>()) << j.value("error", "?");
    const auto& session = j["session"];
    EXPECT_EQ(session["schema"], 1);
    EXPECT_EQ(session["repository_id"], "bachata-official");
    EXPECT_EQ(session["repository_revision"], "bachata-official");
    EXPECT_EQ(session["serial"], "CUSA00001");
    EXPECT_EQ(session["app_version"], "01.00");
    EXPECT_EQ(session["patch_file"], "PATCHES/Test.xml");
    EXPECT_EQ(session["patch_sha256"].get<std::string>().size(), 64u);
    EXPECT_EQ(session["enabled_patch_ids"], json::array({"patch.a"}));
    ASSERT_EQ(session["identities"].size(), 2u);
    EXPECT_EQ(session["identities"][0]["id"], "patch.a");
    EXPECT_EQ(session["identities"][0]["xml_selector"]["name"], "Patch A");
    EXPECT_TRUE(session["selected_preset"].is_null());

    const auto parsed = PatchSession::Parse(session.dump());
    ASSERT_TRUE(parsed.ok) << parsed.error;

    InstallObserver();
    const auto result = MemoryPatcher::ApplyManagedSession(parsed.config, repo, "CUSA00001", "01.00");

    EXPECT_TRUE(result.session_ok);
    EXPECT_EQ(result.reject, PatchSession::RejectReason::None);
    EXPECT_TRUE(result.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));

    // Re-applying the same session must not double-write memory within the same session.
    const auto again = MemoryPatcher::ApplyManagedSession(parsed.config, repo, "CUSA00001", "01.00");
    EXPECT_FALSE(again.apply.ok);
    EXPECT_EQ(Observed(), std::vector<std::string>({"Patch A"}));
}