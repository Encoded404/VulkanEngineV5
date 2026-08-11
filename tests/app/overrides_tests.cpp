#include <gtest/gtest.h>

import App.Overrides;

namespace {

using App::Overrides::ApplyOverride;
using App::Overrides::DescribeEffective;
using App::Overrides::GenerateOverridesHelp;
using App::Overrides::GplPolicy;
using App::Overrides::GplStructurePolicy;
using App::Overrides::kGplChoices;
using App::Overrides::kGplStructureChoices;
using App::Overrides::kOverrideSpecs;
using App::Overrides::OverrideError;
using App::Overrides::Overrides;
using App::Overrides::OverrideSpec;

const OverrideSpec* FindSpec(std::string_view key) {
    for (const auto& spec : kOverrideSpecs) {
        if (spec.key == key) return &spec;
    }
    return nullptr;
}

TEST(OverridesTest, GplParsesEveryChoice) {
    for (const auto& choice : kGplChoices) {
        Overrides o;
        ApplyOverride(std::string{"gpl="} + std::string{choice.label}, o);
        const auto* spec = FindSpec("gpl");
        ASSERT_NE(spec, nullptr);
        EXPECT_EQ(spec->describe(o), choice.label);
    }
}

TEST(OverridesTest, GplRejectsUnknownValue) {
    Overrides o;
    try {
        ApplyOverride("gpl=banana", o);
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("banana"), std::string::npos) << msg;
        EXPECT_NE(msg.find("auto|force|off"), std::string::npos) << msg;
    }
}

TEST(OverridesTest, GplRejectsEmptyValue) {
    Overrides o;
    EXPECT_THROW(ApplyOverride("gpl=", o), OverrideError);
}

TEST(OverridesTest, GplLastWins) {
    Overrides o;
    ApplyOverride("gpl=auto", o);
    ApplyOverride("gpl=force", o);
    EXPECT_EQ(o.gpl_policy, GplPolicy::Forced);
}

TEST(OverridesTest, GplStructureParsesEveryChoice) {
    for (const auto& choice : kGplStructureChoices) {
        Overrides o;
        ApplyOverride(std::string{"gpl.structure="} + std::string{choice.label}, o);
        const auto* spec = FindSpec("gpl.structure");
        ASSERT_NE(spec, nullptr);
        EXPECT_EQ(spec->describe(o), choice.label);
    }
}

TEST(OverridesTest, GplStructureRejectsUnknownValue) {
    Overrides o;
    try {
        ApplyOverride("gpl.structure=banana", o);
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("banana"), std::string::npos) << msg;
        EXPECT_NE(msg.find("auto|split|combined"), std::string::npos) << msg;
    }
}

TEST(OverridesTest, GplStructureRejectsEmptyValue) {
    Overrides o;
    EXPECT_THROW(ApplyOverride("gpl.structure=", o), OverrideError);
}

TEST(OverridesTest, GplStructureLastWins) {
    Overrides o;
    ApplyOverride("gpl.structure=auto", o);
    ApplyOverride("gpl.structure=split", o);
    EXPECT_EQ(o.gpl_structure, GplStructurePolicy::Split);
}

TEST(OverridesTest, ExtDisableAppendsAndIsRepeatable) {
    Overrides o;
    ApplyOverride("ext.disable=VK_EXT_debug_utils", o);
    ApplyOverride("ext.disable=VK_EXT_graphics_pipeline_library", o);
    ASSERT_EQ(o.force_disabled_extensions.size(), 2u);
    EXPECT_EQ(o.force_disabled_extensions[0], "VK_EXT_debug_utils");
    EXPECT_EQ(o.force_disabled_extensions[1], "VK_EXT_graphics_pipeline_library");
}

TEST(OverridesTest, ExtDisableRejectsEmptyValue) {
    Overrides o;
    EXPECT_THROW(ApplyOverride("ext.disable=", o), OverrideError);
}

TEST(OverridesTest, UnknownKeyRejected) {
    Overrides o;
    try {
        ApplyOverride("nope=x", o);
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
        ApplyOverride("gpl", o);
        FAIL() << "expected OverrideError";
    } catch (const OverrideError& e) {
        const std::string msg{e.what()};
        EXPECT_NE(msg.find("gpl"), std::string::npos) << msg;
        EXPECT_NE(msg.find("="), std::string::npos) << msg;
    }
}

TEST(OverridesTest, DefaultsMatchTypedInitializers) {
    Overrides o;
    const auto* gpl = FindSpec("gpl");
    const auto* gpl_structure = FindSpec("gpl.structure");
    const auto* ext = FindSpec("ext.disable");
    ASSERT_NE(gpl, nullptr);
    ASSERT_NE(gpl_structure, nullptr);
    ASSERT_NE(ext, nullptr);
    EXPECT_EQ(gpl->describe(o), "auto");
    EXPECT_EQ(gpl_structure->describe(o), "auto");
    EXPECT_EQ(ext->describe(o), "none");
}

TEST(OverridesTest, RoundTripIdentityOverChoices) {
    for (const auto& choice : kGplChoices) {
        Overrides o;
        ApplyOverride(std::string{"gpl="} + std::string{choice.label}, o);
        const auto* spec = FindSpec("gpl");
        ASSERT_NE(spec, nullptr);
        EXPECT_EQ(spec->describe(o), choice.label);
    }
}

TEST(OverridesTest, IsolationBetweenKeys) {
    Overrides o;
    ApplyOverride("gpl=force", o);
    EXPECT_TRUE(o.force_disabled_extensions.empty());
    ApplyOverride("ext.disable=VK_EXT_debug_utils", o);
    EXPECT_EQ(o.gpl_policy, GplPolicy::Forced);
}

TEST(OverridesTest, HelpGeneration) {
    const std::string help = GenerateOverridesHelp(Overrides{});
    for (const auto& spec : kOverrideSpecs) {
        EXPECT_FALSE(spec.summary.empty()) << "summary must be non-empty for key " << spec.key;
        EXPECT_NE(help.find(spec.key), std::string::npos) << "help must mention key " << spec.key;
        const std::string default_line = "default: " + spec.describe(Overrides{});
        EXPECT_NE(help.find(default_line), std::string::npos)
            << "help must show the programmatic default for key " << spec.key;
    }
    for (const auto& choice : kGplChoices) {
        EXPECT_NE(help.find(choice.label), std::string::npos)
            << "help must list choice label " << choice.label;
        EXPECT_NE(help.find(choice.description), std::string::npos)
            << "help must list choice description for " << choice.label;
    }
    for (const auto& choice : kGplStructureChoices) {
        EXPECT_NE(help.find(choice.label), std::string::npos)
            << "help must list choice label " << choice.label;
        EXPECT_NE(help.find(choice.description), std::string::npos)
            << "help must list choice description for " << choice.label;
    }
}

TEST(OverridesTest, RegistrySelfConsistency) {
    Overrides o;
    for (const auto& spec : kOverrideSpecs) {
        if (spec.key == "gpl") {
            EXPECT_NO_THROW(ApplyOverride(std::string{"gpl="} + std::string{kGplChoices[0].label}, o));
        } else if (spec.key == "gpl.structure") {
            EXPECT_NO_THROW(ApplyOverride(std::string{"gpl.structure="} + std::string{kGplStructureChoices[0].label}, o));
        } else if (spec.key == "ext.disable") {
            EXPECT_NO_THROW(ApplyOverride("ext.disable=VK_EXT_graphics_pipeline_library", o));
        } else {
            FAIL() << "unexpected registry key " << spec.key;
        }
    }
}

TEST(OverridesTest, DescribeEffectivePristine) {
    const std::string effective = DescribeEffective(Overrides{});
    EXPECT_NE(effective.find("ext.disable=none"), std::string::npos) << effective;
    EXPECT_NE(effective.find("gpl=auto"), std::string::npos) << effective;
    EXPECT_NE(effective.find("gpl.structure=auto"), std::string::npos) << effective;
}

TEST(OverridesTest, DescribeEffectiveAfterOverrides) {
    Overrides o;
    ApplyOverride("gpl=force", o);
    ApplyOverride("gpl.structure=split", o);
    ApplyOverride("ext.disable=VK_EXT_debug_utils", o);
    const std::string effective = DescribeEffective(o);
    EXPECT_NE(effective.find("gpl=force"), std::string::npos) << effective;
    EXPECT_NE(effective.find("gpl.structure=split"), std::string::npos) << effective;
    EXPECT_NE(effective.find("VK_EXT_debug_utils"), std::string::npos) << effective;
}

}  // namespace
