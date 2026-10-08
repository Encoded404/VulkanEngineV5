module;

export module VulkanEngine.Components.Text;

import std;

import VulkanEngine.ECS.ComponentRegistry;

export namespace VulkanEngine::Components {

// Horizontal placement of each laid-out line inside its box. Mirrors the text
// layer's TextAlign, kept as its own enum so the component does not have to
// import the shaping/layout stack.
enum class TextAlign : std::uint8_t {
    Left,
    Center,
    Right,
};

// Whether the string is wrapped to max_width.
enum class TextWrap : std::uint8_t {
    // Only explicit newlines break lines; max_width is ignored.
    None,
    // Greedy UAX#14 word wrapping at max_width.
    Word,
};

// World-space text on an entity.
//
// Data component: a plain description a gather pass reads. It names the font by
// id and gives the string, the size and the look, and the world placement comes
// from the entity's Transform. Nothing here resolves a face, shapes a run or
// owns glyphs -- that is the text system's job -- so the component stays
// metadata and the same component can be tested without a font, a device, or a
// render graph.
//
// `world_height` is the em size in *local* units (the Transform's scale is
// applied on top), which is why it is a height and not a pixel size: world text
// has no pixels until a frame projects it.
struct Text {
    // NOLINTBEGIN(misc-non-private-member-variables-in-classes)
    // The font registry id; 0 means "not set" and is skipped by a gather pass.
    std::uint32_t font_id = 0;
    // Which face of the font, for a container with several.
    std::uint32_t face_index = 0;
    std::string content;
    // Em size in local units before the Transform's scale.
    float world_height = 1.0f;
    // Straight-alpha colour, multiplied into the generated coverage.
    std::array<float, 4> color{1.0f, 1.0f, 1.0f, 1.0f};
    TextAlign align = TextAlign::Left;
    TextWrap wrap = TextWrap::None;
    // Wrapping width in local units; 0 means "do not wrap".
    float max_width = 0.0f;
    // NOLINTEND(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::Components
