// Shared technique packing / flag constants. MUST match the C++ side:
// TechniquePacking and TECHNIQUE_FLAG_* (BaseTechnique.cppm, SceneRenderer.cppm).
//
// This header is leaf-only (it includes nothing) and has no entry point: it is
// never compiled on its own and is not part of the _engine_shaders list, only
// of COMPILER_DEPENDS so every dependent shader rebuilds when it changes.
//
// Note: kDepthEps deliberately lives in hiz_helpers.slang (which owns the
// near-pole helpers that use it), not here — the pre_cull/occlusion_cull
// translation units include both headers, so a copy here would be a
// redefinition.

static const uint kTechniqueBits = 14;
static const uint kTechniqueMask = (1u << kTechniqueBits) - 1;
static const uint kCmdWords = 5; // VkDrawIndexedIndirectCommand words

// 0 = monolithic (compact index copy), 1 = MID (command emission).
// Declared once here so constant_id 0 is claimed by a single definition.
[[vk::constant_id(0)]] const uint kCompactionMode = 0;

static const uint kFlagDepthPass         = 1u << 0;
static const uint kFlagReceivesOcclusion = 1u << 1;
static const uint kFlagCollect           = 1u << 2;
static const uint kFlagOccluderSafe      = 1u << 3;

// CullEntry.flags bit 0: submesh selected as an occluder this frame (set by
// occluder_select, honored by pre_cull).
static const uint kOccluderBit = 1u << 0;
