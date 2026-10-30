#include <gtest/gtest.h>

import std;

import Examples.TextDemo.Config;

namespace {

using Examples::TextDemo::ParseTextBackend;
using Examples::TextDemo::TextBackendName;
using VulkanEngine::SceneRenderer::TextBackend;

// The example's device-free surface tested here is the backend spelling:
// parsing and round-tripping. Mapping the parsed value onto RendererConfig and
// the world-text pass is a field assignment, so the engine's own world-text
// tests cover the routing.

TEST(TextDemoConfigTest, ParsesBackendNamesCaseInsensitively) {
    EXPECT_EQ(ParseTextBackend("msdf"), TextBackend::Msdf);
    EXPECT_EQ(ParseTextBackend("MSDF"), TextBackend::Msdf);
    EXPECT_EQ(ParseTextBackend("slug"), TextBackend::Slug);
    EXPECT_EQ(ParseTextBackend("Slug"), TextBackend::Slug);

    // An unknown spelling is rejected rather than silently resolved to the
    // default: the backend is the point of the run.
    EXPECT_FALSE(ParseTextBackend("").has_value());
    EXPECT_FALSE(ParseTextBackend("sdf").has_value());
    EXPECT_FALSE(ParseTextBackend(" slug ").has_value());
    EXPECT_FALSE(ParseTextBackend("msdf2").has_value());
}

TEST(TextDemoConfigTest, BackendNamesRoundTrip) {
    // Whatever ParseTextBackend accepts must be what TextBackendName prints, so
    // the two spellings cannot drift apart.
    for (const TextBackend backend : {TextBackend::Msdf, TextBackend::Slug}) {
        const auto parsed = ParseTextBackend(TextBackendName(backend));
        ASSERT_TRUE(parsed.has_value());
        EXPECT_EQ(*parsed, backend);
    }
}

} // namespace
