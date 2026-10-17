// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/patch_repository.h"
#include "common/sha256.h"

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace {

class TempDir {
public:
    TempDir() {
        auto ns = std::chrono::steady_clock::now().time_since_epoch().count();
        temp_path = fs::temp_directory_path() / ("shadps4_repo_test_" + std::to_string(ns) + "_" +
                                                 std::to_string(reinterpret_cast<uintptr_t>(this)));
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

std::string WriteText(const fs::path& p, const std::string& contents) {
    std::ofstream out(p, std::ios::binary);
    out << contents;
    out.close();
    return sha256::SHA256::Hex(contents);
}

constexpr const char* kValidPatch = R"(
<Patch>
    <TitleID>
        <ID>CUSA00001</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Performance Patch" Author="Kyo" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList>
            <Line Type="bytes32" Address="400000" Value="00000001"/>
        </PatchList>
    </Metadata>
</Patch>
)";

json ValidManifest(const std::string& patch_sha, const std::string& patch_file = "PATCHES/Test.xml") {
    json root;
    root["schema"] = 1;
    root["repository_id"] = "bachata-official";
    root["revision"] = "2026.08.19.1";
    root["generated_at"] = "2026-08-19T00:00:00Z";
    root["games"]["CUSA00001"]["title"] = "Test Game";
    root["games"]["CUSA00001"]["patch_file"] = patch_file;
    root["games"]["CUSA00001"]["sha256"] = patch_sha;
    root["games"]["CUSA00001"]["versions"] = {"01.00"};
    root["games"]["CUSA00001"]["patches"] = json::array({
        {{"id", "test.performance-patch"},
         {"name", "Performance Patch"},
         {"author", "Kyo"},
         {"patch_version", "1.0"},
         {"app_versions", json::array({"01.00"})},
         {"xml_selector",
          {{"title", "Test Game"},
           {"name", "Performance Patch"},
           {"author", "Kyo"},
           {"app_ver", "01.00"},
           {"app_elf", "eboot.bin"}}}},
    });
    return root;
}

void WriteJson(const fs::path& p, const json& j) {
    std::ofstream out(p);
    out << std::setw(2) << j;
}

} // namespace

class PatchRepositoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir = std::make_unique<TempDir>();
    }
    void TearDown() override {
        temp_dir.reset();
    }

    fs::path WriteRepository(const std::string& xml_contents, json manifest_json,
                             const std::string& patch_file = "PATCHES/Test.xml") {
        const fs::path root = temp_dir->path();
        fs::create_directories(root / "PATCHES");
        const fs::path xml_path = root / patch_file;
        WriteText(xml_path, xml_contents);
        const fs::path manifest_path = root / "manifest-v1.json";
        WriteJson(manifest_path, manifest_json);
        return root;
    }

    std::unique_ptr<TempDir> temp_dir;
};

TEST_F(PatchRepositoryTest, ValidManifestParses) {
    const auto sha = sha256::SHA256::Hex(kValidPatch);
    const auto root = WriteRepository(kValidPatch, ValidManifest(sha));
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(result.ok) << result.error;
    EXPECT_EQ(result.manifest.schema, 1);
    EXPECT_EQ(result.manifest.repository_id, "bachata-official");
    EXPECT_EQ(result.manifest.games.at("CUSA00001").patches.size(), 1u);
}

TEST_F(PatchRepositoryTest, UnsupportedSchemaRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["schema"] = 99;
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unsupported"), std::string::npos);
}

TEST_F(PatchRepositoryTest, MalformedManifestRejected) {
    const fs::path root = temp_dir->path();
    WriteText(root / "manifest-v1.json", "{not valid json");
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    EXPECT_FALSE(result.ok);
}

TEST_F(PatchRepositoryTest, InvalidCusaRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["games"]["BADGAME"] = manifest["games"]["CUSA00001"];
    manifest["games"].erase("CUSA00001");
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid"), std::string::npos);
}

TEST_F(PatchRepositoryTest, AbsolutePathRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["games"]["CUSA00001"]["patch_file"] = "/PATCHES/Test.xml";
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("invalid"), std::string::npos);
}

TEST_F(PatchRepositoryTest, PathTraversalRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["games"]["CUSA00001"]["patch_file"] = "../PATCHES/Test.xml";
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    EXPECT_FALSE(result.ok);
}

TEST_F(PatchRepositoryTest, MissingXmlRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch), "PATCHES/Missing.xml");
    const auto root = WriteRepository(kValidPatch, manifest, "PATCHES/Other.xml");
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("missing"), std::string::npos);
}

TEST_F(PatchRepositoryTest, Sha256MismatchRejected) {
    auto manifest = ValidManifest(std::string(64, '0'));
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("SHA-256 mismatch"), std::string::npos);
}

TEST_F(PatchRepositoryTest, ManifestCusaAbsentFromXmlTitleIdsRejected) {
    // XML declares CUSA00002, manifest maps CUSA00001.
    const std::string xml = R"(
<Patch>
    <TitleID>
        <ID>CUSA00002</ID>
    </TitleID>
    <Metadata Title="Test Game" Name="Performance Patch" Author="Kyo" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="bytes32" Address="400000" Value="00000001"/></PatchList>
    </Metadata>
</Patch>
)";
    auto manifest = ValidManifest(sha256::SHA256::Hex(xml));
    const auto root = WriteRepository(xml, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("TitleIDs"), std::string::npos);
}

TEST_F(PatchRepositoryTest, SelectorMatchesZeroEntriesRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["games"]["CUSA00001"]["patches"][0]["xml_selector"]["name"] = "Nonexistent";
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("zero Metadata"), std::string::npos);
}

TEST_F(PatchRepositoryTest, SelectorMatchesMultipleEntriesRejected) {
    const std::string xml = R"(
<Patch>
    <TitleID><ID>CUSA00001</ID></TitleID>
    <Metadata Title="Test Game" Name="Dup" Author="Kyo" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="bytes32" Address="400000" Value="00000001"/></PatchList>
    </Metadata>
    <Metadata Title="Test Game" Name="Dup" Author="Kyo" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="bytes32" Address="400004" Value="00000002"/></PatchList>
    </Metadata>
</Patch>
)";
    json manifest = ValidManifest(sha256::SHA256::Hex(xml));
    manifest["games"]["CUSA00001"]["patches"][0]["xml_selector"]["name"] = "Dup";
    const auto root = WriteRepository(xml, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("multiple Metadata"), std::string::npos);
}

TEST_F(PatchRepositoryTest, DuplicateStableIdRejected) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["games"]["CUSA00001"]["patches"].push_back(
        manifest["games"]["CUSA00001"]["patches"][0]);
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto result = PatchRepository::LoadManifest(root / "manifest-v1.json");
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("duplicate stable patch ID"), std::string::npos);
}

TEST_F(PatchRepositoryTest, AppVersionMismatchResolved) {
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "02.00", root);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.game.patches.size(), 1u);
    EXPECT_EQ(result.game.patches[0].compatibility,
              PatchRepository::PatchCompatibility::VersionMismatch);
}

TEST_F(PatchRepositoryTest, GameVersionListDoesNotOverridePatchAppVer) {
    // Regression: the game-level versions list means "the repository covers these game
    // versions", NOT "every patch works with all of them". The XML Metadata AppVer is
    // authoritative, so patch (AppVer 01.09) must be VersionMismatch for an installed 01.00
    // even though 01.00 appears in the game's versions list.
    const std::string xml = R"(
<Patch>
    <TitleID><ID>CUSA00001</ID></TitleID>
    <Metadata Title="Test Game" Name="Performance Patch" Author="Kyo" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="bytes32" Address="400000" Value="00000001"/></PatchList>
    </Metadata>
</Patch>
)";
    json manifest = ValidManifest(sha256::SHA256::Hex(xml));
    manifest["games"]["CUSA00001"]["versions"] = json::array({"01.00", "01.09"});
    manifest["games"]["CUSA00001"]["patches"][0]["app_versions"] = json::array({"01.09"});
    manifest["games"]["CUSA00001"]["patches"][0]["xml_selector"]["app_ver"] = "01.09";
    const auto root = WriteRepository(xml, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.game.patches.size(), 1u);
    EXPECT_EQ(result.game.patches[0].compatibility,
              PatchRepository::PatchCompatibility::VersionMismatch);
}

TEST_F(PatchRepositoryTest, MaskPatchCompatibleAcrossVersions) {
    // mask/mask_jump32 lines are version independent; a version-mismatched patch that
    // contains one stays compatible, matching the Milestone 1 apply semantics.
    const std::string xml = R"(
<Patch>
    <TitleID><ID>CUSA00001</ID></TitleID>
    <Metadata Title="Test Game" Name="Mask Patch" Author="Kyo" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="mask" Address="0000000000000000" Value="0000000000000000" Offset="0"/></PatchList>
    </Metadata>
</Patch>
)";
    json manifest = ValidManifest(sha256::SHA256::Hex(xml));
    manifest["games"]["CUSA00001"]["patches"][0]["name"] = "Mask Patch";
    manifest["games"]["CUSA00001"]["patches"][0]["app_versions"] = json::array({"01.09"});
    manifest["games"]["CUSA00001"]["patches"][0]["xml_selector"]["name"] = "Mask Patch";
    manifest["games"]["CUSA00001"]["patches"][0]["xml_selector"]["app_ver"] = "01.09";
    const auto root = WriteRepository(xml, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    ASSERT_TRUE(result.ok) << result.error;
    ASSERT_EQ(result.game.patches.size(), 1u);
    EXPECT_EQ(result.game.patches[0].compatibility,
              PatchRepository::PatchCompatibility::Compatible);
}

TEST_F(PatchRepositoryTest, ManifestAppVersionsMustAgreeWithXml) {
    // app_versions is metadata and must agree with the XML definition, not override it.
    auto manifest = ValidManifest(sha256::SHA256::Hex(kValidPatch));
    manifest["games"]["CUSA00001"]["patches"][0]["app_versions"] = json::array({"01.09"});
    const auto root = WriteRepository(kValidPatch, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok) << loaded.error;
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("app_versions"), std::string::npos);
}

TEST_F(PatchRepositoryTest, StableIdSurvivesMetadataChanges) {
    // Revision 1: name "Performance Patch".
    const std::string rev1_sha = sha256::SHA256::Hex(kValidPatch);
    json rev1 = ValidManifest(rev1_sha);
    rev1["revision"] = "rev1";
    auto root1 = WriteRepository(kValidPatch, rev1);
    auto loaded1 = PatchRepository::LoadManifest(root1 / "manifest-v1.json");
    ASSERT_TRUE(loaded1.ok);
    auto resolved1 = PatchRepository::ResolveGame(loaded1.manifest, "CUSA00001", "01.00", root1);
    ASSERT_TRUE(resolved1.ok);
    ASSERT_EQ(resolved1.game.patches.size(), 1u);
    EXPECT_EQ(resolved1.game.patches[0].id, "test.performance-patch");

    // Revision 2: same id, changed name/note/PatchVer.
    const std::string rev2_xml = R"xml(
<Patch>
    <TitleID><ID>CUSA00001</ID></TitleID>
    <Metadata Title="Test Game" Name="Performance Patch (perf increase)" Author="Kyo" PatchVer="2.0" AppVer="01.00" AppElf="eboot.bin" Note="updated" isEnabled="true">
        <PatchList><Line Type="bytes32" Address="400000" Value="00000002"/></PatchList>
    </Metadata>
</Patch>
)xml";
    json rev2 = ValidManifest(sha256::SHA256::Hex(rev2_xml));
    rev2["revision"] = "rev2";
    rev2["games"]["CUSA00001"]["patches"][0]["name"] = "Performance Patch (perf increase)";
    rev2["games"]["CUSA00001"]["patches"][0]["patch_version"] = "2.0";
    rev2["games"]["CUSA00001"]["patches"][0]["xml_selector"]["name"] =
        "Performance Patch (perf increase)";
    auto root2 = WriteRepository(rev2_xml, rev2);
    auto loaded2 = PatchRepository::LoadManifest(root2 / "manifest-v1.json");
    ASSERT_TRUE(loaded2.ok);
    auto resolved2 = PatchRepository::ResolveGame(loaded2.manifest, "CUSA00001", "01.00", root2);
    ASSERT_TRUE(resolved2.ok);
    ASSERT_EQ(resolved2.game.patches.size(), 1u);
    EXPECT_EQ(resolved2.game.patches[0].id, "test.performance-patch");
    EXPECT_EQ(resolved2.game.patches[0].name, "Performance Patch (perf increase)");
}

TEST_F(PatchRepositoryTest, LoadsCompletelyOffline) {
    // No network is involved; this exercises the entire parse/resolve path from local files.
    const auto sha = sha256::SHA256::Hex(kValidPatch);
    const auto root = WriteRepository(kValidPatch, ValidManifest(sha));
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto resolved =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    ASSERT_TRUE(resolved.ok);
    ASSERT_EQ(resolved.game.patches.size(), 1u);
    EXPECT_EQ(resolved.game.patches[0].compatibility,
              PatchRepository::PatchCompatibility::Compatible);
}

TEST_F(PatchRepositoryTest, UnsupportedPatchTypeRejected) {
    const std::string xml = R"(
<Patch>
    <TitleID><ID>CUSA00001</ID></TitleID>
    <Metadata Title="Test Game" Name="Performance Patch" Author="Kyo" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
        <PatchList><Line Type="totally-unknown" Address="400000" Value="00000001"/></PatchList>
    </Metadata>
</Patch>
)";
    auto manifest = ValidManifest(sha256::SHA256::Hex(xml));
    const auto root = WriteRepository(xml, manifest);
    const auto loaded = PatchRepository::LoadManifest(root / "manifest-v1.json");
    ASSERT_TRUE(loaded.ok);
    const auto result =
        PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00", root);
    EXPECT_FALSE(result.ok);
    EXPECT_NE(result.error.find("unsupported patch type"), std::string::npos);
}

TEST(Sha256Test, KnownAnswerEmpty) {
    // FIPS 180-4 test vector: SHA-256 of the empty string.
    EXPECT_EQ(sha256::SHA256::Hex(""),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(Sha256Test, KnownAnswerAbc) {
    // FIPS 180-4 test vector: SHA-256 of "abc".
    EXPECT_EQ(sha256::SHA256::Hex("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST(Sha256Test, KnownAnswerAcrossChunkBoundaries) {
    // The hasher feeds the file in 64 KiB chunks in production; verify that a 128-byte input
    // split across two 64-byte transform calls still matches the single-shot digest.
    const std::string data(128, 'a');
    sha256::SHA256 hasher;
    hasher.update(data.data(), 64);
    hasher.update(data.data() + 64, 64);
    EXPECT_EQ(sha256::SHA256::Hex(hasher.final()), sha256::SHA256::Hex(data));
}
