module;

export module Examples.TextDemo.Game;

import std;

export import VulkanEngine.GameEngine;
export import Examples.TextDemo.Config;
import VulkanShared.CallbackList;

import VulkanEngine.Components.Text;
import VulkanEngine.Components.Transform;
import VulkanEngine.Text.Blob;
import VulkanEngine.Text.Font;
import VulkanEngine.Text.GlyphAtlasGpu;
import VulkanEngine.Text.Msdf;

export namespace Examples::TextDemo {

// Documentation by example for both text paths.
//
// It draws screen-space text through the text system's single submission seam
// (TextSystem::SubmitScreenText, which shapes, wraps and queues) and spawns
// world-space text entities that the engine's depth-tested WorldTextPass draws
// against scene geometry. The world backend -- MSDF or Slug -- is chosen once,
// on RendererConfig, because that is where the engine's world-text pass is
// built; see TextDemoConfig.
class TextDemoGame {
public:
    TextDemoGame(VulkanEngine::SceneRenderer::TextBackend backend,
                 const std::filesystem::path& executable_path);
    ~TextDemoGame();

    TextDemoGame(const TextDemoGame&) = delete;
    TextDemoGame& operator=(const TextDemoGame&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const {
        return hooks_;
    }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    // One spawned world-text entity. Text and Transform are data components, so
    // the builder re-fetches them by EntityId each frame instead of caching
    // pointers that a later structural change would invalidate.
    struct WorldTextEntity {
        VulkanEngine::EntityId entity{};
    };

    VulkanEngine::SceneRenderer::TextBackend backend_;
    std::filesystem::path exe_dir_;
    VulkanEngine::Application::ApplicationHooks hooks_{};

    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};

    VulkanEngine::GameEngine engine_game_{};

    // The one face both paths use. Loaded through GameEngine::LoadFont, which
    // registers it with the text system and (under hot reload) watches it.
    std::shared_ptr<const VulkanEngine::Text::FontFace> face_{};

    // The MSDF world backend owns a generator and its own RGBA8 atlas; the Slug
    // backend owns neither an atlas nor a size, only the design-unit encoder.
    // The pass owns the blob storage buffer the Slug offsets index.
    VulkanEngine::Text::MsdfGenerator msdf_{};
    VulkanEngine::Text::GlyphAtlasGpu world_atlas_{};
    VulkanEngine::Text::GlyphBlobEncoder slug_encoder_{};
    VulkanEngine::Text::MsdfConfig msdf_config_{};

    std::vector<WorldTextEntity> world_text_entities_{};
    // One-shot diagnostic: the glyph-quad count the first frame actually queued,
    // which is what proves the selected backend built a batch.
    bool reported_queue_ = false;
};

} // namespace Examples::TextDemo
