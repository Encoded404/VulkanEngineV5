#include <gtest/gtest.h>

import std;

import VulkanShared.UserPaths;

namespace {

using VulkanShared::UserPaths::Identity;
using VulkanShared::UserPaths::MapEnv;
using VulkanShared::UserPaths::Options;
using VulkanShared::UserPaths::Resolve;
using VulkanShared::UserPaths::Roots;
using VulkanShared::UserPaths::SanitizeSegment;

// The XDG branch is the one that can be exercised on this host. Apple and
// Windows resolve from different variables; those cases are asserted on the
// platform that owns them and skipped elsewhere.
#if !defined(_WIN32) && !defined(__APPLE__)
constexpr bool kXdgPlatform = true;
#else
constexpr bool kXdgPlatform = false;
#endif

const Identity kIdentity{.org = "MyOrg", .app = "MyApp"};

TEST(UserPaths, XdgVariablesAreUsedWhenAbsolute) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    const auto env = MapEnv({
        {"HOME", "/home/tester"},
        {"XDG_DATA_HOME", "/xdg/data"},
        {"XDG_CACHE_HOME", "/xdg/cache"},
        {"XDG_STATE_HOME", "/xdg/state"},
    });

    const auto roots = Resolve(kIdentity, Options{}, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/xdg/data/MyOrg/MyApp"});
    EXPECT_EQ(roots->cache, std::filesystem::path{"/xdg/cache/MyOrg/MyApp"});
    EXPECT_EQ(roots->log, std::filesystem::path{"/xdg/state/MyOrg/MyApp/logs"});
    EXPECT_EQ(roots->origin, Roots::Origin::Platform);
    EXPECT_TRUE(roots->fallback_reason.empty());
}

TEST(UserPaths, HomeFallbacksFollowTheXdgDefaults) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    const auto env = MapEnv({{"HOME", "/home/tester"}});

    const auto roots = Resolve(kIdentity, Options{}, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/home/tester/.local/share/MyOrg/MyApp"});
    EXPECT_EQ(roots->cache, std::filesystem::path{"/home/tester/.cache/MyOrg/MyApp"});
    EXPECT_EQ(roots->log, std::filesystem::path{"/home/tester/.local/state/MyOrg/MyApp/logs"});
}

TEST(UserPaths, RelativeXdgValuesAreIgnored) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    // The XDG spec requires a relative value to be ignored, not resolved
    // against the working directory. All three variables are set to garbage.
    const auto env = MapEnv({
        {"HOME", "/home/tester"},
        {"XDG_DATA_HOME", "relative/data"},
        {"XDG_CACHE_HOME", "relative/cache"},
        {"XDG_STATE_HOME", ""},
    });

    const auto roots = Resolve(kIdentity, Options{}, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/home/tester/.local/share/MyOrg/MyApp"});
    EXPECT_EQ(roots->cache, std::filesystem::path{"/home/tester/.cache/MyOrg/MyApp"});
    EXPECT_EQ(roots->log, std::filesystem::path{"/home/tester/.local/state/MyOrg/MyApp/logs"});
}

TEST(UserPaths, NoHomeIsAnError) {
    const auto env = MapEnv({});
    const auto roots = Resolve(kIdentity, Options{}, env);
    EXPECT_FALSE(roots.has_value());
}

TEST(UserPaths, EmptyAppIdentityIsAnError) {
    const auto env = MapEnv({{"HOME", "/home/tester"}});
    const auto roots = Resolve(Identity{.org = "MyOrg", .app = ""}, Options{}, env);
    EXPECT_FALSE(roots.has_value());
}

class ScratchDir {
public:
    ScratchDir() {
        static std::atomic<unsigned> counter{0};
        const auto nonce = static_cast<unsigned>(std::random_device{}());
        path_ = std::filesystem::temp_directory_path() /
                std::format("vkengine_userpaths_{}_{}", nonce, counter.fetch_add(1));
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

TEST(UserPaths, ExplicitBaseDirectoryWinsAndIsCreated) {
    ScratchDir scratch;
    const auto base = scratch.Path() / "nested" / "user";

    Options options{};
    options.base_dir = base;

    // Deliberately reachable only through the override, so a fallback would be
    // visible as a different path rather than as an error.
    const auto env = MapEnv({{"HOME", "/nonexistent"}});

    const auto roots = Resolve(kIdentity, options, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->persistent, base);
    EXPECT_EQ(roots->cache, base / "cache");
    EXPECT_EQ(roots->log, base / "logs");
    EXPECT_EQ(roots->origin, Roots::Origin::Override);
    EXPECT_TRUE(std::filesystem::is_directory(base));
}

TEST(UserPaths, EnvironmentUserDirectoryIsUsedAndLosesToTheOption) {
    if (!kXdgPlatform) GTEST_SKIP() << "paths in this test are POSIX-shaped";

    ScratchDir scratch;
    const auto from_env = scratch.Path() / "from_env";
    const auto from_option = scratch.Path() / "from_option";

    const auto env = MapEnv({
        {"HOME", "/home/tester"},
        {"VKENGINE_USER_DIR", from_env.string()},
    });

    const auto via_env = Resolve(kIdentity, Options{}, env);
    ASSERT_TRUE(via_env.has_value()) << via_env.error();
    EXPECT_EQ(via_env->persistent, from_env);
    EXPECT_EQ(via_env->origin, Roots::Origin::Override);

    Options options{};
    options.base_dir = from_option;
    const auto via_option = Resolve(kIdentity, options, env);
    ASSERT_TRUE(via_option.has_value()) << via_option.error();
    EXPECT_EQ(via_option->persistent, from_option);
}

TEST(UserPaths, RelativeUserDirectoryEnvironmentVariableIsIgnored) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    const auto env = MapEnv({
        {"HOME", "/home/tester"},
        {"VKENGINE_USER_DIR", "portable/relative"},
    });

    const auto roots = Resolve(kIdentity, Options{}, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->origin, Roots::Origin::Platform);
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/home/tester/.local/share/MyOrg/MyApp"});
}

TEST(UserPaths, PortableMarkerSwitchesToTheExecutableDirectory) {
    ScratchDir scratch;
    std::ofstream(scratch.Path() / "portable.txt").put('1');

    Options options{};
    options.executable_dir = scratch.Path();

    const auto env = MapEnv({{"HOME", "/home/tester"}});
    const auto roots = Resolve(kIdentity, options, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->persistent, scratch.Path());
    EXPECT_EQ(roots->cache, scratch.Path() / "cache");
    EXPECT_EQ(roots->log, scratch.Path() / "logs");
    EXPECT_EQ(roots->origin, Roots::Origin::Portable);
}

TEST(UserPaths, ForcePortableWorksWithoutAMarker) {
    ScratchDir scratch;
    Options options{};
    options.executable_dir = scratch.Path();
    options.force_portable = true;

    const auto roots = Resolve(kIdentity, options, MapEnv({{"HOME", "/home/tester"}}));
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->origin, Roots::Origin::Portable);
}

TEST(UserPaths, DisablePortableIgnoresTheMarker) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    ScratchDir scratch;
    std::ofstream(scratch.Path() / "portable.txt").put('1');

    Options options{};
    options.executable_dir = scratch.Path();
    options.disable_portable = true;

    const auto roots = Resolve(kIdentity, options, MapEnv({{"HOME", "/home/tester"}}));
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->origin, Roots::Origin::Platform);
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/home/tester/.local/share/MyOrg/MyApp"});
}

TEST(UserPaths, UnwritablePortableDirectoryFallsBackToThePlatformDefault) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    ScratchDir scratch;
    std::ofstream(scratch.Path() / "portable.txt").put('1');
    // A regular file where the probe needs a directory is the portable way to
    // make the tree uncreatable without depending on permissions or on the test
    // not running as root.
    std::ofstream(scratch.Path() / "cache").put('x');

    Options options{};
    options.executable_dir = scratch.Path();

    const auto roots = Resolve(kIdentity, options, MapEnv({{"HOME", "/home/tester"}}));
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->origin, Roots::Origin::Platform);
    EXPECT_FALSE(roots->fallback_reason.empty());
    EXPECT_NE(roots->persistent, scratch.Path());
}

TEST(UserPaths, IdentitySegmentsAreSanitizedBeforeUse) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    const auto env = MapEnv({{"HOME", "/home/tester"}});
    const auto roots = Resolve(Identity{.org = "My Company!", .app = "Game/Two"}, Options{}, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    // The separator cannot survive as one, or the app name would escape the
    // org directory.
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/home/tester/.local/share/My Company_/Game_Two"});
}

TEST(UserPaths, EmptyOrgOmitsTheOrgSegment) {
    if (!kXdgPlatform) GTEST_SKIP() << "XDG resolution is Linux/BSD only";

    // Matches the convention every platform library uses: an unset organization
    // means "no org directory", not an empty path component.
    const auto env = MapEnv({{"HOME", "/home/tester"}});
    const auto roots = Resolve(Identity{.org = "", .app = "MyApp"}, Options{}, env);
    ASSERT_TRUE(roots.has_value()) << roots.error();
    EXPECT_EQ(roots->persistent, std::filesystem::path{"/home/tester/.local/share/MyApp"});
    EXPECT_EQ(roots->log, std::filesystem::path{"/home/tester/.local/state/MyApp/logs"});
}

TEST(SanitizeSegment, KeepsUsableCharactersAndRejectsTraversal) {
    EXPECT_EQ(SanitizeSegment("MyCompany"), "MyCompany");
    EXPECT_EQ(SanitizeSegment("My App 2"), "My App 2");
    EXPECT_EQ(SanitizeSegment("my-app_2.0"), "my-app_2.0");
    EXPECT_EQ(SanitizeSegment("a/b"), "a_b");
    EXPECT_EQ(SanitizeSegment("a\\b"), "a_b");
    EXPECT_EQ(SanitizeSegment("a:b*c?"), "a_b_c_");
    EXPECT_EQ(SanitizeSegment(""), "");
    // Only dots leaves a path component that means "parent".
    EXPECT_EQ(SanitizeSegment(".."), "");
    EXPECT_EQ(SanitizeSegment("..."), "");
}

TEST(UserPaths, Utf8RoundTrips) {
    const std::string text = "/home/tester/Application Support/Ünïcode";
    EXPECT_EQ(VulkanShared::UserPaths::ToUtf8(VulkanShared::UserPaths::FromUtf8(text)), text);
}

} // namespace
