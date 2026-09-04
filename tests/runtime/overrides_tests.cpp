#include <gtest/gtest.h>

import std;

import test_logging;
import Runtime.Overrides;

namespace {

// Install the per-test file logger once for the test binary.
[[maybe_unused]] const bool kLoggerInstalled = [] {
    TestLogging::InstallPerTestFileLogger();
    return true;
}();

// State for the custom-key registration test. OverrideSpec callbacks are
// function pointers (the built-in specs are constexpr), so custom overrides
// carry their state in module/file scope rather than capturing lambdas.
bool g_custom_applied = false;
std::string g_custom_seen;

void OnCustomOverride(std::string_view value, Runtime::Overrides& /*o*/) {
    g_custom_applied = true;
    g_custom_seen = std::string{value};
}

std::string CustomDescribe(const Runtime::Overrides&) {
    return std::string{"?"};
}

std::string CustomValueHint() {
    return std::string{"<custom>"};
}

using Runtime::GplPolicy;
using Runtime::GplStructurePolicy;
using Runtime::kGplChoices;
using Runtime::kGplStructureChoices;
using Runtime::OverrideError;
using Runtime::Overrides;
using Runtime::OverrideSpec;

TEST(OverridesTest, GplParsesEveryChoice) {
    for (const auto& choice : kGplChoices) {
        Overrides o;
        o.Apply(std::string{"gpl="} + std::string{choice.label});
        EXPECT_EQ(o.gpl_policy, choice.value);
    }
}

TEST(OverridesTest, GplRejectsUnknownValue) {
    Overrides o;
    try {
        o.Apply("gpl=banana");
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("banana"), std::string::npos) << msg;
        EXPECT_NE(msg.find("auto|force|off"), std::string::npos) << msg;
    }
}

TEST(OverridesTest, GplRejectsEmptyValue) {
    Overrides o;
    EXPECT_THROW(o.Apply("gpl="), OverrideError);
}

TEST(OverridesTest, GplLastWins) {
    Overrides o;
    o.Apply("gpl=auto");
    o.Apply("gpl=force");
    EXPECT_EQ(o.gpl_policy, GplPolicy::Forced);
}

TEST(OverridesTest, GplStructureParsesEveryChoice) {
    for (const auto& choice : kGplStructureChoices) {
        Overrides o;
        o.Apply(std::string{"gpl.structure="} + std::string{choice.label});
        EXPECT_EQ(o.gpl_structure, choice.value);
    }
}

TEST(OverridesTest, GplStructureRejectsUnknownValue) {
    Overrides o;
    try {
        o.Apply("gpl.structure=banana");
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("banana"), std::string::npos) << msg;
        EXPECT_NE(msg.find("auto|split|combined"), std::string::npos) << msg;
    }
}

TEST(OverridesTest, GplStructureRejectsEmptyValue) {
    Overrides o;
    EXPECT_THROW(o.Apply("gpl.structure="), OverrideError);
}

TEST(OverridesTest, GplStructureLastWins) {
    Overrides o;
    o.Apply("gpl.structure=auto");
    o.Apply("gpl.structure=split");
    EXPECT_EQ(o.gpl_structure, GplStructurePolicy::Split);
}

TEST(OverridesTest, ExtDisableAppendsAndIsRepeatable) {
    Overrides o;
    o.Apply("ext.disable=VK_EXT_debug_utils");
    o.Apply("ext.disable=VK_EXT_graphics_pipeline_library");
    ASSERT_EQ(o.force_disabled_extensions.size(), 2u);
    EXPECT_EQ(o.force_disabled_extensions[0], "VK_EXT_debug_utils");
    EXPECT_EQ(o.force_disabled_extensions[1], "VK_EXT_graphics_pipeline_library");
}

TEST(OverridesTest, ExtDisableRejectsEmptyValue) {
    Overrides o;
    EXPECT_THROW(o.Apply("ext.disable="), OverrideError);
}

TEST(OverridesTest, UnknownKeyRejected) {
    Overrides o;
    try {
        o.Apply("nope=x");
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("nope"), std::string::npos) << msg;
        EXPECT_NE(msg.find("ext.disable"), std::string::npos) << msg;
        EXPECT_NE(msg.find("gpl"), std::string::npos) << msg;
    }
}

TEST(OverridesTest, MissingEqualsRejected) {
    Overrides o;
    try {
        o.Apply("gpl");
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("gpl"), std::string::npos) << msg;
        EXPECT_NE(msg.find("="), std::string::npos) << msg;
    }
}

TEST(OverridesTest, DefaultsMatchTypedInitializers) {
    Overrides o;
    EXPECT_EQ(o.gpl_policy, GplPolicy::Auto);
    EXPECT_EQ(o.gpl_structure, GplStructurePolicy::Auto);
    EXPECT_TRUE(o.force_disabled_extensions.empty());
}

TEST(OverridesTest, IsolationBetweenKeys) {
    Overrides o;
    o.Apply("gpl=force");
    EXPECT_TRUE(o.force_disabled_extensions.empty());
    o.Apply("ext.disable=VK_EXT_debug_utils");
    EXPECT_EQ(o.gpl_policy, GplPolicy::Forced);
}

TEST(OverridesTest, HelpGeneration) {
    const std::string help = Overrides{}.GenerateHelp();
    EXPECT_NE(help.find("ext.disable"), std::string::npos) << help;
    EXPECT_NE(help.find("gpl"), std::string::npos) << help;
    EXPECT_NE(help.find("gpl.structure"), std::string::npos) << help;
    EXPECT_NE(help.find("default: auto"), std::string::npos) << help;
    EXPECT_NE(help.find("default: none"), std::string::npos) << help;
    for (const auto& choice : kGplChoices) {
        EXPECT_NE(help.find(choice.label), std::string::npos) << help;
        EXPECT_NE(help.find(choice.description), std::string::npos) << help;
    }
    for (const auto& choice : kGplStructureChoices) {
        EXPECT_NE(help.find(choice.label), std::string::npos) << help;
        EXPECT_NE(help.find(choice.description), std::string::npos) << help;
    }
}

TEST(OverridesTest, CustomKeyRegistration) {
    g_custom_applied = false;
    g_custom_seen.clear();

    Overrides o;
    o.Register(OverrideSpec{
        .key = "my.key",
        .summary = "Custom example override",
        .value_hint = &CustomValueHint,
        .apply = &OnCustomOverride,
        .describe = &CustomDescribe,
    });

    o.Apply("my.key=hello");
    EXPECT_TRUE(g_custom_applied);
    EXPECT_EQ(g_custom_seen, "hello");

    EXPECT_NE(o.GenerateHelp().find("my.key"), std::string::npos) << o.GenerateHelp();
    EXPECT_NE(o.DescribeEffective().find("my.key=?"), std::string::npos) << o.DescribeEffective();
}

TEST(OverridesTest, CustomKeyRejectsUnknownValueWithExtrasListed) {
    Overrides o;
    o.Register(OverrideSpec{
        .key = "my.key",
        .summary = "Custom example override",
        .value_hint = &CustomValueHint,
        .apply = &OnCustomOverride,
        .describe = &CustomDescribe,
    });
    try {
        o.Apply("other=x");
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("other"), std::string::npos) << msg;
        EXPECT_NE(msg.find("my.key"), std::string::npos) << msg;
    }
}

TEST(OverridesTest, DescribeEffectivePristine) {
    const std::string effective = Overrides{}.DescribeEffective();
    EXPECT_NE(effective.find("ext.disable=none"), std::string::npos) << effective;
    EXPECT_NE(effective.find("gpl=auto"), std::string::npos) << effective;
    EXPECT_NE(effective.find("gpl.structure=auto"), std::string::npos) << effective;
}

TEST(OverridesTest, DescribeEffectiveAfterOverrides) {
    Overrides o;
    o.Apply("gpl=force");
    o.Apply("gpl.structure=split");
    o.Apply("ext.disable=VK_EXT_debug_utils");
    const std::string effective = o.DescribeEffective();
    EXPECT_NE(effective.find("gpl=force"), std::string::npos) << effective;
    EXPECT_NE(effective.find("gpl.structure=split"), std::string::npos) << effective;
    EXPECT_NE(effective.find("VK_EXT_debug_utils"), std::string::npos) << effective;
}

}  // namespace