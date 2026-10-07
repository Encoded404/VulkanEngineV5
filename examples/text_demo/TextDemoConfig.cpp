module;

module Examples.TextDemo.Config;

import std;

namespace Examples::TextDemo {

namespace {

// ASCII-only lowering. The names are CLI tokens, not user text, so this avoids
// a locale-dependent std::tolower for no benefit.
[[nodiscard]] std::string ToLowerAscii(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char c : text) {
        const char lower =
            (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        lowered.push_back(lower);
    }
    return lowered;
}

} // namespace

std::optional<VulkanEngine::SceneRenderer::TextBackend> ParseTextBackend(
    std::string_view name) {
    const std::string lowered = ToLowerAscii(name);
    if (lowered == "msdf") {
        return VulkanEngine::SceneRenderer::TextBackend::Msdf;
    }
    if (lowered == "slug") {
        return VulkanEngine::SceneRenderer::TextBackend::Slug;
    }
    return std::nullopt;
}

std::string_view TextBackendName(VulkanEngine::SceneRenderer::TextBackend backend) noexcept {
    switch (backend) {
        case VulkanEngine::SceneRenderer::TextBackend::Slug:
            return "slug";
        case VulkanEngine::SceneRenderer::TextBackend::Msdf:
            break;
    }
    return "msdf";
}

VulkanEngine::Renderer::RendererConfig MakeRendererConfig(
    VulkanEngine::SceneRenderer::TextBackend backend) {
    VulkanEngine::Renderer::RendererConfig config{};
    config.text_backend = backend;
    // The screen-space overlay registers either way; it is a separate pass with
    // its own hinted-A8 pipeline and is not what the world backend selects.
    config.enable_text = true;
    return config;
}

} // namespace Examples::TextDemo
