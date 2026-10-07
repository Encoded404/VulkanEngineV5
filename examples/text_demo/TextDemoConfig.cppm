module;

export module Examples.TextDemo.Config;

import std;

// Re-exported so an importer of this module can name the types in the
// signatures below (TextBackend and RendererConfig) without importing the
// renderer itself.
export import VulkanEngine.Renderer;

export namespace Examples::TextDemo {

// The text example's device-free configuration surface.
//
// Everything here is pure: it maps a command-line string onto the engine's
// world-text backend selector and onto the RendererConfig the example boots
// with. It lives in its own module -- and its own static library -- so the
// example and the device-free test import one definition of the mapping, the
// way examples/custom_pass shares its pass graph.

// Parses the --text-backend value. Case-insensitive, so "MSDF" and "slug" both
// work. Returns nullopt for anything else: the caller rejects the run with a
// message rather than silently booting the default backend, because "which
// rasterizer drew this" is the whole point of the example.
[[nodiscard]] std::optional<VulkanEngine::SceneRenderer::TextBackend> ParseTextBackend(
    std::string_view name);

// The canonical spelling of `backend`, which is what ParseTextBackend accepts.
[[nodiscard]] std::string_view TextBackendName(
    VulkanEngine::SceneRenderer::TextBackend backend) noexcept;

// The renderer configuration the example runs with.
//
// The backend is the only field the choice changes, and it changes it on
// RendererConfig rather than per component because the engine's world-text
// built-in owns exactly one graphics pipeline: the selector picks which shader
// pair (and which instance layout) that pass is built with. Screen-space text
// is unaffected -- it is always the hinted greyscale overlay -- so `enable_text`
// stays on either way.
[[nodiscard]] VulkanEngine::Renderer::RendererConfig MakeRendererConfig(
    VulkanEngine::SceneRenderer::TextBackend backend);

} // namespace Examples::TextDemo
