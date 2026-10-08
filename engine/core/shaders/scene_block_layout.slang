// Block geometry for the per-submesh storage-buffer arrays.
//
// MIRRORS VulkanEngine::SceneLimits::kEntriesPerBlock in
// engine/core/render/SceneLimits.cppm. The CPU uses it to size each BlockArray;
// the shaders use it to split a flat submesh index into (block, element).
// Changing one without the other corrupts every block lookup, so keep them
// equal.
//
// This is deliberately separate from MATERIAL_BLOCK_SIZE / LIGHTS_PER_BLOCK:
// submesh geometry and material/light geometry must be independently
// changeable.

static const uint kEntriesPerBlock = 256u;
