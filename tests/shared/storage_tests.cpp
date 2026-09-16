#include <gtest/gtest.h>

import std;

import VulkanShared.Storage;
import VulkanShared.UserPaths;

namespace {

using VulkanShared::Storage::ErrorCode;
using VulkanShared::Storage::Options;
using VulkanShared::Storage::Storage;
using VulkanShared::UserPaths::Roots;

class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto nonce = static_cast<unsigned>(std::random_device{}());
        path_ = std::filesystem::temp_directory_path() /
                std::format("vkengine_storage_{}_{}", nonce, counter.fetch_add(1));
        std::error_code ec;
        std::filesystem::create_directories(path_, ec);
    }

    ~ScratchDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;

    [[nodiscard]] const std::filesystem::path& Path() const { return path_; }

private:
    std::filesystem::path path_;
};

// Durability is off so the suite does not pay a disk flush per write; the
// atomicity and error paths under test are independent of it.
Options TestOptions() {
    Options options{};
    options.durable_writes = false;
    return options;
}

Roots RootsUnder(const std::filesystem::path& base) {
    return Roots{.persistent = base, .cache = base / "cache", .log = base / "logs"};
}

std::expected<Storage, VulkanShared::Storage::Error> MakeStorage(const std::filesystem::path& base) {
    return Storage::CreateWithRoots(RootsUnder(base), TestOptions());
}

// Every write stages into "<name>.<pid>.tmp"; a leftover means a failure path
// leaked its staging file.
bool HasStagingFiles(const std::filesystem::path& dir) {
    std::error_code ec;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
        if (entry.path().filename().string().find(".tmp") != std::string::npos) {
            return true;
        }
    }
    return false;
}

TEST(Storage, CreatesTheWholeTreeUpFront) {
    ScratchDir scratch;
    const auto base = scratch.Path() / "user";

    auto storage = MakeStorage(base);
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();
    EXPECT_TRUE(std::filesystem::is_directory(base));
    EXPECT_TRUE(std::filesystem::is_directory(base / "cache"));
    EXPECT_TRUE(std::filesystem::is_directory(base / "logs"));
    EXPECT_TRUE(std::filesystem::is_directory(base / "saves"));
}

TEST(Storage, MissingConfigIsNotFound) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    const auto config = storage->ReadConfig();
    ASSERT_FALSE(config.has_value());
    EXPECT_EQ(config.error().code, ErrorCode::NotFound);
}

TEST(Storage, ConfigRoundTrips) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    const std::string json = R"({"window":{"width":1280},"version":2})";
    ASSERT_TRUE(storage->WriteConfig(json).has_value());

    const auto read = storage->ReadConfig();
    ASSERT_TRUE(read.has_value()) << read.error().ToString();
    EXPECT_EQ(*read, json);
    EXPECT_FALSE(HasStagingFiles(scratch.Path()));
}

TEST(Storage, EmptyConfigIsReadableAndNotEmptyAnError) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    ASSERT_TRUE(storage->WriteConfig("").has_value());
    const auto read = storage->ReadConfig();
    ASSERT_TRUE(read.has_value()) << read.error().ToString();
    EXPECT_TRUE(read->empty());
}

TEST(Storage, OverwritingConfigKeepsThePreviousContentsAsABackup) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    ASSERT_TRUE(storage->WriteConfig(R"({"version":1})").has_value());
    ASSERT_TRUE(storage->WriteConfig(R"({"version":2})").has_value());

    const auto current = storage->ReadConfig();
    ASSERT_TRUE(current.has_value());
    EXPECT_EQ(*current, R"({"version":2})");

    std::ifstream backup(storage->ConfigBackupPath());
    ASSERT_TRUE(backup.is_open());
    const std::string contents{std::istreambuf_iterator<char>(backup),
                              std::istreambuf_iterator<char>()};
    EXPECT_EQ(contents, R"({"version":1})");
}

TEST(Storage, OversizedConfigIsRejectedOnRead) {
    ScratchDir scratch;
    Options options = TestOptions();
    options.max_config_bytes = 8;

    auto storage = Storage::CreateWithRoots(RootsUnder(scratch.Path()), options);
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();
    ASSERT_TRUE(storage->WriteConfig(R"({"much":"longer than eight bytes"})").has_value());

    const auto read = storage->ReadConfig();
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code, ErrorCode::TooLarge);
}

TEST(Storage, SaveRoundTripsBinaryData) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    const std::vector<std::byte> payload{std::byte{0x00}, std::byte{0xff}, std::byte{0x10},
                                         std::byte{0x00}};
    ASSERT_TRUE(storage->WriteSave("slot1", payload).has_value());

    const auto read = storage->ReadSave("slot1");
    ASSERT_TRUE(read.has_value()) << read.error().ToString();
    EXPECT_EQ(*read, payload);
    EXPECT_EQ(storage->SavePath("slot1"), scratch.Path() / "saves" / "slot1");
    EXPECT_FALSE(HasStagingFiles(scratch.Path()));
}

TEST(Storage, MissingSaveIsNotFound) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    const auto read = storage->ReadSave("nope");
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code, ErrorCode::NotFound);
}

TEST(Storage, SaveNamesThatEscapeTheSaveDirectoryAreRejected) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    const auto outside = scratch.Path() / "escaped";
    const std::vector<std::byte> payload{std::byte{0x01}};

    for (const std::string_view name : {"", ".", "..", "../escaped", "a/b", "a\\b", "/abs"}) {
        const auto written = storage->WriteSave(name, payload);
        ASSERT_FALSE(written.has_value()) << "name accepted: " << name;
        EXPECT_EQ(written.error().code, ErrorCode::InvalidName) << "name: " << name;

        const auto read = storage->ReadSave(name);
        ASSERT_FALSE(read.has_value());
        EXPECT_EQ(read.error().code, ErrorCode::InvalidName);

        EXPECT_TRUE(storage->SavePath(name).empty());
    }

    // Nothing may have been created outside the saves directory.
    EXPECT_FALSE(std::filesystem::exists(outside));
    EXPECT_FALSE(std::filesystem::exists(scratch.Path() / "escaped"));
}

TEST(Storage, CacheRoundTripsAndMissingCacheIsNotFound) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    const auto missing = storage->ReadCache("pipeline_cache_abc.bin");
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);

    const std::vector<std::byte> blob{std::byte{0xde}, std::byte{0xad}};
    storage->WriteCache("pipeline_cache_abc.bin", blob);

    const auto read = storage->ReadCache("pipeline_cache_abc.bin");
    ASSERT_TRUE(read.has_value()) << read.error().ToString();
    EXPECT_EQ(*read, blob);
}

TEST(Storage, CacheWritesCannotFailButInvalidNamesDoNothing) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    // Nothing to observe beyond "this returns and does not touch the parent":
    // the contract is that a cache write is allowed to do nothing at all.
    const std::vector<std::byte> blob{std::byte{0x01}};
    storage->WriteCache("../escape", blob);
    storage->WriteCache("", blob);

    EXPECT_FALSE(std::filesystem::exists(scratch.Path() / "escape"));
    EXPECT_FALSE(HasStagingFiles(scratch.Path()));
}

TEST(Storage, ResetRemovesSettingsAndSavesButKeepsTheCache) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    ASSERT_TRUE(storage->WriteConfig(R"({"version":1})").has_value());
    const std::vector<std::byte> save{std::byte{0x2a}};
    ASSERT_TRUE(storage->WriteSave("slot1", save).has_value());
    const std::vector<std::byte> cached{std::byte{0x99}};
    storage->WriteCache("pipeline_cache_abc.bin", cached);

    ASSERT_TRUE(storage->ResetUserData().has_value());

    const auto config = storage->ReadConfig();
    ASSERT_FALSE(config.has_value());
    EXPECT_EQ(config.error().code, ErrorCode::NotFound);

    const auto read_save = storage->ReadSave("slot1");
    ASSERT_FALSE(read_save.has_value());
    EXPECT_EQ(read_save.error().code, ErrorCode::NotFound);

    // The whole point of a separate cache root: a user-data reset must not cost
    // a full pipeline-cache rebuild.
    const auto cache = storage->ReadCache("pipeline_cache_abc.bin");
    ASSERT_TRUE(cache.has_value()) << cache.error().ToString();
    EXPECT_EQ(*cache, cached);

    // The object is still usable afterwards.
    ASSERT_TRUE(storage->WriteConfig(R"({"version":2})").has_value());
    const auto again = storage->ReadConfig();
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(*again, R"({"version":2})");
}

TEST(Storage, ResetOnAFreshInstallSucceeds) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    // Nothing has been written yet; resetting must not report a failure.
    EXPECT_TRUE(storage->ResetUserData().has_value());
}

TEST(Storage, ResetLeavesFilesItDoesNotOwnAlone) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    ASSERT_TRUE(storage->WriteConfig(R"({"version":1})").has_value());
    const std::vector<std::byte> save{std::byte{0x2a}};
    ASSERT_TRUE(storage->WriteSave("slot1", save).has_value());

    // A base directory chosen with --user-dir (or portable mode) is a directory
    // the user picked; anything in it that storage did not create must survive.
    const auto stray = scratch.Path() / "notes.txt";
    std::ofstream(stray).put('x');

    ASSERT_TRUE(storage->ResetUserData().has_value());

    EXPECT_FALSE(std::filesystem::exists(storage->ConfigPath()));
    EXPECT_TRUE(std::filesystem::exists(stray));
}

TEST(Storage, ConcurrentWritesToOneFileLeaveAValidFile) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    // Two writers racing on one target is the case a fixed ".tmp" name corrupts:
    // one process renames the other's half-written buffer into place. Staging
    // names are per-process, so the surviving file must be one of the two whole
    // payloads.
    const std::string a(4096, 'a');
    const std::string b(4096, 'b');

    std::thread first([&] { static_cast<void>(storage->WriteConfig(a)); });
    std::thread second([&] { static_cast<void>(storage->WriteConfig(b)); });
    first.join();
    second.join();

    const auto read = storage->ReadConfig();
    ASSERT_TRUE(read.has_value()) << read.error().ToString();
    EXPECT_TRUE(*read == a || *read == b) << "file is a blend of both writers";
    EXPECT_EQ(read->size(), 4096u);
    EXPECT_FALSE(HasStagingFiles(scratch.Path()));
}

TEST(Storage, ReadRejectsADirectoryWhereAFileIsExpected) {
    ScratchDir scratch;
    auto storage = MakeStorage(scratch.Path());
    ASSERT_TRUE(storage.has_value()) << storage.error().ToString();

    // A directory named like the config file must be reported, not opened.
    std::error_code ec;
    std::filesystem::create_directories(storage->ConfigPath(), ec);
    ASSERT_FALSE(ec);

    const auto read = storage->ReadConfig();
    ASSERT_FALSE(read.has_value());
    EXPECT_EQ(read.error().code, ErrorCode::ReadFailed);
}

} // namespace
