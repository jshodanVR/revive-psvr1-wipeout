// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "common/memory_patcher.h"
#include "common/patch_repository.h"
#include "common/patch_state.h"
#include "common/sha256.h"
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
        temp_path = fs::temp_directory_path() / ("shadps4_state_test_" + std::to_string(ns) + "_" +
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

struct XmlPatchSpec {
    std::string name;
    std::string author;
};

struct ManifestPatchSpec {
    std::string id;
    std::string name;
    std::string author;
};

// Multi-metadata XML fixture. Every entry is AppVer 01.00 and uses a distinct fixed address so
// the resolve/apply paths see ordinary (non-mask) patches.
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

json MakeManifest(const std::string& revision, const std::vector<ManifestPatchSpec>& specs) {
    json root;
    root["schema"] = 1;
    root["repository_id"] = "bachata-official";
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

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Builds the identity map from resolved manifest selectors so ApplyManagedPatches resolves
// repository IDs (the same data the native integration will use at launch).
MemoryPatcher::PatchIdentityMap IdentityMapFromGame(const PatchRepository::ResolvedGame& game) {
    MemoryPatcher::PatchIdentityMap map;
    for (const auto& patch : game.patches) {
        const auto& def = patch.definition;
        map[MemoryPatcher::CanonicalPatchSelectorKey(def.title, def.name, def.author,
                                                     def.app_version, def.app_elf)] =
            patch.id;
    }
    return map;
}

} // namespace

class PatchUserStateTest : public ::testing::Test {
protected:
    void SetUp() override {
        temp_dir = std::make_unique<TempDir>();
        Common::Singleton<PSF>::Instance()->AddString("APP_VER", "01.00");
        MemoryPatcher::ResetAppliedXmlFiles();
    }
    void TearDown() override {
        MemoryPatcher::g_eboot_address = 0;
        MemoryPatcher::g_eboot_image_size = 0;
        MemoryPatcher::ResetAppliedXmlFiles();
        temp_dir.reset();
    }

    // Writes a self-consistent repository revision: XML containing exactly the metadata the
    // manifest references (plus any extra unreferenced entries), then the manifest hashing it.
    fs::path WriteRevision(const std::string& revision,
                           const std::vector<XmlPatchSpec>& xml_specs,
                           const std::vector<ManifestPatchSpec>& manifest_specs) {
        const fs::path root = temp_dir->path() / revision;
        fs::create_directories(root / "PATCHES");
        const auto xml = PatchXml(xml_specs);
        const fs::path xml_path = root / "PATCHES" / "Test.xml";
        std::ofstream out(xml_path, std::ios::binary);
        out << xml;
        auto manifest = MakeManifest(revision, manifest_specs);
        manifest["games"]["CUSA00001"]["sha256"] = sha256::SHA256::Hex(xml);
        WriteJson(root / "manifest-v1.json", manifest);
        return root;
    }

    PatchRepository::ResolvedGame Resolve(const fs::path& repository_root) const {
        const auto loaded = PatchRepository::LoadManifest(repository_root / "manifest-v1.json");
        EXPECT_TRUE(loaded.ok) << loaded.error;
        const auto resolved = PatchRepository::ResolveGame(loaded.manifest, "CUSA00001", "01.00",
                                                           repository_root);
        EXPECT_TRUE(resolved.ok) << resolved.error;
        return resolved.game;
    }

    PatchUserState::State LoadedState(const fs::path& state_path) const {
        const auto res = PatchUserState::Load(state_path);
        EXPECT_TRUE(res.ok) << res.error;
        return res.state;
    }

    static PatchUserState::SelectionStatus StatusOf(const PatchUserState::EffectiveSelection& sel,
                                                    const std::string& id) {
        for (const auto& entry : sel.entries) {
            if (entry.id == id) {
                return entry.status;
            }
        }
        return PatchUserState::SelectionStatus::Unavailable;
    }

    std::unique_ptr<TempDir> temp_dir;
};

// Shared patch specs for the update-stability fixtures.
static const XmlPatchSpec kPatchA = {"Patch A", "author-a"};
static const XmlPatchSpec kPatchB = {"Patch B", "author-b"};
static const XmlPatchSpec kPatchBPrime = {"Patch B Prime", "author-b"};
static const XmlPatchSpec kPatchC = {"Patch C", "author-c"};
static const XmlPatchSpec kPatchD = {"Patch D", "author-d"};

static const ManifestPatchSpec kManifestA = {"patch.a", "Patch A", "author-a"};
static const ManifestPatchSpec kManifestB = {"patch.b", "Patch B", "author-b"};
static const ManifestPatchSpec kManifestBPrime = {"patch.b.prime", "Patch B Prime", "author-b"};
static const ManifestPatchSpec kManifestC = {"patch.c", "Patch C", "author-c"};
static const ManifestPatchSpec kManifestD = {"patch.d", "Patch D", "author-d"};

TEST_F(PatchUserStateTest, ValidStateLoads) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.a", "patch.c"};
    state.disabled_patch_ids = {"patch.b"};
    state.selected_preset = std::nullopt;
    state.last_seen_repository_revision = "2026.08.19.1";
    ASSERT_TRUE(PatchUserState::Save(state_path, state));

    const auto res = PatchUserState::Load(state_path);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_TRUE(res.file_found);
    EXPECT_EQ(res.state.serial, "CUSA00001");
    EXPECT_EQ(res.state.repository_id, "bachata-official");
    EXPECT_EQ(res.state.enabled_patch_ids, std::vector<std::string>({"patch.a", "patch.c"}));
    EXPECT_EQ(res.state.disabled_patch_ids, std::vector<std::string>({"patch.b"}));
    EXPECT_FALSE(res.state.selected_preset.has_value());
    EXPECT_EQ(res.state.last_seen_repository_revision, "2026.08.19.1");
}

TEST_F(PatchUserStateTest, MissingStateReturnsDefault) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    const auto res = PatchUserState::Load(state_path);
    ASSERT_TRUE(res.ok) << res.error;
    EXPECT_FALSE(res.file_found);
    EXPECT_EQ(res.state.schema, PatchUserState::kSupportedSchema);
    EXPECT_TRUE(res.state.enabled_patch_ids.empty());
    EXPECT_TRUE(res.state.disabled_patch_ids.empty());
    EXPECT_FALSE(res.state.selected_preset.has_value());
}

TEST_F(PatchUserStateTest, MalformedStateFailsSafely) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    fs::create_directories(state_path.parent_path());
    std::ofstream(state_path) << "{ not valid json !!!";
    const auto res = PatchUserState::Load(state_path);
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("malformed"), std::string::npos);
    // The returned default lets callers continue without crashing.
    EXPECT_TRUE(res.state.enabled_patch_ids.empty());
}

TEST_F(PatchUserStateTest, UnsupportedStateSchemaRejected) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    fs::create_directories(state_path.parent_path());
    json root;
    root["schema"] = 2;
    root["serial"] = "CUSA00001";
    root["repository_id"] = "bachata-official";
    WriteJson(state_path, root);
    const auto res = PatchUserState::Load(state_path);
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("unsupported state schema"), std::string::npos);
}

TEST_F(PatchUserStateTest, SerialMismatchRejected) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    fs::create_directories(state_path.parent_path());
    json root;
    root["schema"] = 1;
    root["serial"] = "CUSA00002";
    root["repository_id"] = "bachata-official";
    WriteJson(state_path, root);
    const auto res = PatchUserState::Load(state_path);
    EXPECT_FALSE(res.ok);
    EXPECT_NE(res.error.find("does not match file"), std::string::npos);
}

TEST_F(PatchUserStateTest, DuplicateEnabledIdsNormalizedOrRejected) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    fs::create_directories(state_path.parent_path());
    json root;
    root["schema"] = 1;
    root["serial"] = "CUSA00001";
    root["repository_id"] = "bachata-official";
    root["enabled_patch_ids"] = {"patch.a", "patch.b", "patch.a", "patch.b"};
    WriteJson(state_path, root);
    const auto res = PatchUserState::Load(state_path);
    ASSERT_TRUE(res.ok) << res.error;
    // Normalized deterministically: unique, first-occurrence order preserved.
    EXPECT_EQ(res.state.enabled_patch_ids, std::vector<std::string>({"patch.a", "patch.b"}));
}

TEST_F(PatchUserStateTest, SameIdInEnabledAndDisabledHandledDeterministically) {
    const fs::path repository_root =
        WriteRevision("r1", {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto game = Resolve(repository_root);

    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.a"};
    state.disabled_patch_ids = {"patch.a", "patch.b"};

    // Explicitly disabled wins over enabled, deterministically.
    const auto sel = PatchUserState::ResolveEffectiveSelection(game, state, {});
    EXPECT_EQ(StatusOf(sel, "patch.a"), PatchUserState::SelectionStatus::Disabled);
    EXPECT_EQ(StatusOf(sel, "patch.b"), PatchUserState::SelectionStatus::Disabled);
    EXPECT_TRUE(sel.apply_ids.empty());
}

TEST_F(PatchUserStateTest, StateSaveIsAtomic) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.a"};

    ASSERT_TRUE(PatchUserState::Save(state_path, state));
    EXPECT_TRUE(fs::exists(state_path));
    // No temp artifact left behind.
    EXPECT_FALSE(fs::exists(state_path.string() + ".tmp"));
    EXPECT_EQ(LoadedState(state_path).enabled_patch_ids,
              std::vector<std::string>({"patch.a"}));

    // Overwrite in place: content is replaced, still no temp artifact, still valid.
    state.enabled_patch_ids = {"patch.c"};
    ASSERT_TRUE(PatchUserState::Save(state_path, state));
    EXPECT_EQ(LoadedState(state_path).enabled_patch_ids,
              std::vector<std::string>({"patch.c"}));
    EXPECT_FALSE(fs::exists(state_path.string() + ".tmp"));
}

TEST_F(PatchUserStateTest, StateRoundTripPreservesSelection) {
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.selected_preset = "mobile-performance";
    state.enabled_patch_ids = {"patch.a", "patch.c"};
    state.disabled_patch_ids = {"patch.b"};
    state.last_seen_repository_revision = "2026.08.19.1";
    ASSERT_TRUE(PatchUserState::Save(state_path, state));

    const auto loaded = LoadedState(state_path);
    EXPECT_EQ(loaded.repository_id, "bachata-official");
    ASSERT_TRUE(loaded.selected_preset.has_value());
    EXPECT_EQ(*loaded.selected_preset, "mobile-performance");
    EXPECT_EQ(loaded.enabled_patch_ids, state.enabled_patch_ids);
    EXPECT_EQ(loaded.disabled_patch_ids, state.disabled_patch_ids);
    EXPECT_EQ(loaded.last_seen_repository_revision, "2026.08.19.1");
}

TEST_F(PatchUserStateTest, RepositoryRevisionChangePreservesEnabledIds) {
    // r1 -> r2 only changes the revision and adds a patch. The state file must not be
    // rewritten, and the enabled IDs must resolve identically.
    const fs::path r1 = WriteRevision("r1", {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const fs::path r2 = WriteRevision("r2", {kPatchA, kPatchB, kPatchD},
                                      {kManifestA, kManifestB, kManifestD});
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";

    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.a"};
    state.disabled_patch_ids = {"patch.b"};
    state.last_seen_repository_revision = "2026.08.19.1";
    ASSERT_TRUE(PatchUserState::Save(state_path, state));
    const std::string before = ReadAll(state_path);

    const auto game_r2 = Resolve(r2);
    const auto sel = PatchUserState::ResolveEffectiveSelection(game_r2, state, {});
    EXPECT_EQ(StatusOf(sel, "patch.a"), PatchUserState::SelectionStatus::Enabled);
    EXPECT_EQ(StatusOf(sel, "patch.b"), PatchUserState::SelectionStatus::Disabled);
    EXPECT_EQ(StatusOf(sel, "patch.d"), PatchUserState::SelectionStatus::DefaultOff);
    EXPECT_EQ(sel.apply_ids, std::unordered_set<std::string>({"patch.a"}));

    // Resolution is pure: the repository update did not rewrite the user's choices.
    EXPECT_EQ(ReadAll(state_path), before);
}

TEST_F(PatchUserStateTest, RemovedPatchIdDoesNotCrash) {
    const fs::path repository_root = WriteRevision("r2", {kPatchA, kPatchB}, {kManifestA});
    const auto game = Resolve(repository_root);

    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.c"}; // absent from r2
    state.disabled_patch_ids = {"patch.gone"};

    const auto sel = PatchUserState::ResolveEffectiveSelection(game, state, {});
    EXPECT_EQ(StatusOf(sel, "patch.c"), PatchUserState::SelectionStatus::Unavailable);
    EXPECT_EQ(StatusOf(sel, "patch.gone"), PatchUserState::SelectionStatus::Unavailable);
    EXPECT_TRUE(sel.apply_ids.empty());
    EXPECT_EQ(StatusOf(sel, "patch.a"), PatchUserState::SelectionStatus::DefaultOff);
}

TEST_F(PatchUserStateTest, ReintroducedPatchIdRecoversPriorSelection) {
    // r1 has C and the user enables it; r2 removes C; r3 restores C under the same stable ID.
    const fs::path r1 = WriteRevision("r1", {kPatchA, kPatchB, kPatchC},
                                      {kManifestA, kManifestB, kManifestC});
    const fs::path r3 =
        WriteRevision("r3", {kPatchA, kPatchB, kPatchC}, {kManifestA, kManifestB, kManifestC});

    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.c"};
    ASSERT_TRUE(PatchUserState::Save(temp_dir->path() / "state" / "CUSA00001.json", state));

    const auto sel = PatchUserState::ResolveEffectiveSelection(Resolve(r1), state, {});
    EXPECT_EQ(StatusOf(sel, "patch.c"), PatchUserState::SelectionStatus::Enabled);
    EXPECT_TRUE(sel.apply_ids.contains("patch.c"));

    // r3 restores C with the same stable ID; the state still carries C, so it is enabled again.
    const auto sel_r3 = PatchUserState::ResolveEffectiveSelection(Resolve(r3), state, {});
    EXPECT_EQ(StatusOf(sel_r3, "patch.c"), PatchUserState::SelectionStatus::Enabled);
    EXPECT_TRUE(sel_r3.apply_ids.contains("patch.c"));
}

TEST_F(PatchUserStateTest, RepositoryIdMismatchDoesNotApply) {
    // State written for one repository must never drive selection for another, even if both
    // happen to contain the same patch ID.
    const fs::path repository_root = WriteRevision("r1", {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto game = Resolve(repository_root);
    ASSERT_EQ(game.repository_id, "bachata-official");

    auto foreign_state = PatchUserState::Default("CUSA00001", "shadps4-official");
    foreign_state.enabled_patch_ids = {"patch.a"};

    const auto sel = PatchUserState::ResolveEffectiveSelection(game, foreign_state, {});
    EXPECT_TRUE(sel.repository_mismatch);
    EXPECT_TRUE(sel.entries.empty());
    EXPECT_TRUE(sel.apply_ids.empty());
}

TEST_F(PatchUserStateTest, RepositoryIdMatchResolvesNormally) {
    const fs::path repository_root = WriteRevision("r1", {kPatchA, kPatchB}, {kManifestA, kManifestB});
    const auto game = Resolve(repository_root);

    auto state = PatchUserState::Default("CUSA00001", game.repository_id);
    state.enabled_patch_ids = {"patch.a"};
    state.disabled_patch_ids = {"patch.b"};

    const auto sel = PatchUserState::ResolveEffectiveSelection(game, state, {});
    EXPECT_FALSE(sel.repository_mismatch);
    EXPECT_EQ(StatusOf(sel, "patch.a"), PatchUserState::SelectionStatus::Enabled);
    EXPECT_EQ(StatusOf(sel, "patch.b"), PatchUserState::SelectionStatus::Disabled);
    EXPECT_EQ(sel.apply_ids, std::unordered_set<std::string>({"patch.a"}));
}

TEST_F(PatchUserStateTest, MissingStateCanBindToCurrentRepository) {
    // A missing state file becomes a fresh default bound to the currently selected repository.
    // Bound but empty: no managed patches enabled, resolves normally against that repository.
    const fs::path repository_root = WriteRevision("r1", {kPatchA, kPatchB, kPatchC},
                                                   {kManifestA, kManifestB, kManifestC});
    const auto game = Resolve(repository_root);

    auto state = PatchUserState::Default("CUSA00001", game.repository_id);
    EXPECT_EQ(state.repository_id, "bachata-official");
    EXPECT_TRUE(state.enabled_patch_ids.empty());
    EXPECT_TRUE(state.disabled_patch_ids.empty());

    const auto sel = PatchUserState::ResolveEffectiveSelection(game, state, {});
    EXPECT_FALSE(sel.repository_mismatch);
    EXPECT_TRUE(sel.apply_ids.empty());
    for (const auto& entry : sel.entries) {
        EXPECT_EQ(entry.status, PatchUserState::SelectionStatus::DefaultOff);
    }
}

TEST_F(PatchUserStateTest, NoStateMeansNoManagedPatchesEnabled) {
    const fs::path repository_root = WriteRevision("r1", {kPatchA, kPatchB, kPatchC},
                                                   {kManifestA, kManifestB, kManifestC});
    const auto game = Resolve(repository_root);
    // Missing state -> default bound to the current repository; nothing enabled.
    const auto state = PatchUserState::Default("CUSA00001", game.repository_id);

    const auto sel = PatchUserState::ResolveEffectiveSelection(game, state, {});
    EXPECT_TRUE(sel.apply_ids.empty());
    for (const auto& entry : sel.entries) {
        EXPECT_EQ(entry.status, PatchUserState::SelectionStatus::DefaultOff);
    }

    // Feeding the empty effective set through the native seam applies nothing.
    const auto result = MemoryPatcher::ApplyManagedPatches(
        repository_root / "PATCHES" / "Test.xml", sel.apply_ids,
        IdentityMapFromGame(game));
    EXPECT_TRUE(result.ok);
    for (const auto& record : result.records) {
        EXPECT_NE(record.status, MemoryPatcher::PatchApplyStatus::Applied);
    }
}

TEST_F(PatchUserStateTest, EffectiveSelectionFeedsApplyManagedPatches) {
    const fs::path repository_root = WriteRevision("r1", {kPatchA, kPatchB, kPatchC},
                                                   {kManifestA, kManifestB, kManifestC});
    const auto game = Resolve(repository_root);

    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.a"};
    state.disabled_patch_ids = {"patch.b"};

    const auto sel = PatchUserState::ResolveEffectiveSelection(game, state, {});
    EXPECT_EQ(sel.apply_ids, std::unordered_set<std::string>({"patch.a"}));

    // Safe writable buffer standing in for the loaded eboot image (fixed-address patches use
    // Address=0x400000 so the (base + offset - 0x400000) arithmetic lands at buffer start).
    std::vector<u8> backing(4096, 0x90);
    MemoryPatcher::g_eboot_address = reinterpret_cast<uintptr_t>(backing.data());
    MemoryPatcher::g_eboot_image_size = backing.size();

    const auto result = MemoryPatcher::ApplyManagedPatches(
        repository_root / "PATCHES" / "Test.xml", sel.apply_ids,
        IdentityMapFromGame(game));
    ASSERT_TRUE(result.ok);

    bool applied_a = false;
    bool skipped_b = false;
    bool skipped_c = false;
    for (const auto& record : result.records) {
        if (record.id == "patch.a" && record.status == MemoryPatcher::PatchApplyStatus::Applied) {
            applied_a = true;
        }
        if (record.id == "patch.b" && record.status == MemoryPatcher::PatchApplyStatus::Disabled) {
            skipped_b = true;
        }
        if (record.id == "patch.c" && record.status == MemoryPatcher::PatchApplyStatus::Disabled) {
            skipped_c = true;
        }
    }
    EXPECT_TRUE(applied_a);
    EXPECT_TRUE(skipped_b);
    EXPECT_TRUE(skipped_c);
}

TEST_F(PatchUserStateTest, UpdateStability_AvoidsUnavailableRestoresReintroduced) {
    // Repository r1: A, B, C. State: enabled = {A, C}, disabled = {B}.
    const fs::path r1 = WriteRevision("r1", {kPatchA, kPatchB, kPatchC},
                                      {kManifestA, kManifestB, kManifestC});
    // r2: A unchanged, B renamed, C temporarily removed, D added.
    const fs::path r2 = WriteRevision("r2", {kPatchA, kPatchBPrime, kPatchD},
                                      {kManifestA, kManifestBPrime, kManifestD});
    // r3: C comes back under the same stable ID.
    const fs::path r3 = WriteRevision("r3", {kPatchA, kPatchBPrime, kPatchC, kPatchD},
                                      {kManifestA, kManifestBPrime, kManifestC, kManifestD});

    auto state = PatchUserState::Default("CUSA00001");
    state.repository_id = "bachata-official";
    state.enabled_patch_ids = {"patch.a", "patch.c"};
    state.disabled_patch_ids = {"patch.b"};
    const fs::path state_path = temp_dir->path() / "state" / "CUSA00001.json";
    ASSERT_TRUE(PatchUserState::Save(state_path, state));
    const std::string before = ReadAll(state_path);

    // Effective selection against r2, with the state file untouched.
    const auto sel_r2 = PatchUserState::ResolveEffectiveSelection(Resolve(r2), state, {});
    EXPECT_EQ(StatusOf(sel_r2, "patch.a"), PatchUserState::SelectionStatus::Enabled);
    EXPECT_EQ(StatusOf(sel_r2, "patch.b"), PatchUserState::SelectionStatus::Unavailable);
    EXPECT_EQ(StatusOf(sel_r2, "patch.b.prime"), PatchUserState::SelectionStatus::DefaultOff);
    EXPECT_EQ(StatusOf(sel_r2, "patch.c"), PatchUserState::SelectionStatus::Unavailable);
    EXPECT_EQ(StatusOf(sel_r2, "patch.d"), PatchUserState::SelectionStatus::DefaultOff);
    EXPECT_EQ(sel_r2.apply_ids, std::unordered_set<std::string>({"patch.a"}));
    EXPECT_EQ(ReadAll(state_path), before);

    // r3 restores C with the same stable ID -> the user's selection becomes enabled again.
    const auto sel_r3 = PatchUserState::ResolveEffectiveSelection(Resolve(r3), state, {});
    EXPECT_EQ(StatusOf(sel_r3, "patch.c"), PatchUserState::SelectionStatus::Enabled);
    EXPECT_EQ(StatusOf(sel_r3, "patch.b"), PatchUserState::SelectionStatus::Unavailable);
    EXPECT_EQ(sel_r3.apply_ids,
              (std::unordered_set<std::string>{"patch.a", "patch.c"}));
    EXPECT_EQ(ReadAll(state_path), before);
}