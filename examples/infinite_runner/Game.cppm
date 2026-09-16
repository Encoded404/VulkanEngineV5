module;

export module Examples.InfiniteRunner.Game;

import std;

export import VulkanEngine.GameEngine;
import VulkanShared.CallbackList;
import Examples.InfiniteRunner.Wall;

export namespace Examples::InfiniteRunner::Game {

// Infinite runner built entirely on the game layer.
//
// A fixed pool of wall entities scrolls toward a stationary player cube. Each
// wall is two blocks with a randomly placed horizontal gap; the player must
// line up with the gap to pass. There is no engine collision or entity
// destruction: walls are recycled in place, and the player is tested both as a
// discrete box against the block geometry and as a ray swept between frames.
class Game {
public:
    explicit Game(const std::filesystem::path& executable_path);
    ~Game();

    Game(const Game&) = delete;
    Game& operator=(const Game&) = delete;

    [[nodiscard]] const VulkanEngine::Application::ApplicationHooks& GetHooks() const { return hooks_; }

private:
    bool OnSetup(VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameUpdate(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnFrameRender(const VulkanEngine::Application::ApplicationContext& ctx);
    void OnShutdown(VulkanEngine::Application::ApplicationContext& ctx);

    // A wall's simulated state plus the render handles it drives. The value
    // type owns the geometry; the game owns the ECS plumbing.
    struct WallSlot {
        Wall wall{};
        VulkanEngine::Components::Transform* left = nullptr;
        VulkanEngine::Components::Transform* right = nullptr;
    };

    VulkanEngine::Components::Transform* CreateCubeEntity(float x, float y, float z,
                                                          float sx, float sy, float sz,
                                                          VulkanEngine::MaterialManager::MaterialRef material);

    void RandomizeWall(Wall& wall);
    void ApplyWallTransform(WallSlot& slot);
    void ResetWalls();
    void ResetRun();
    void UpdatePlayer(const VulkanEngine::Application::ApplicationContext& ctx, float delta_time);
    // Integrates every wall and sweeps the player against their pre-move boxes.
    // Owns the before/after pair explicitly, so the collision never depends on
    // where an earlier update left shared state. Returns true on a hit.
    bool StepWalls(float delta_time, float player_x_before, float player_dx);
    // Discrete box-vs-box overlap at the settled end-of-frame positions.
    bool PlayerOverlapsAnyWall() const;

    VulkanEngine::Application::ApplicationHooks hooks_{};

    VulkanShared::ScopedHandle<bool(VulkanEngine::Application::ApplicationContext&)> setup_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_update_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> frame_render_token_{};
    VulkanShared::ScopedHandle<void(VulkanEngine::Application::ApplicationContext&)> shutdown_token_{};
    VulkanShared::ScopedHandle<void()> imgui_draw_handle_{};

    std::filesystem::path exe_dir_{};
    VulkanEngine::GameEngine engine_game_{};

    std::uint32_t cube_mesh_id_ = 0;
    VulkanEngine::MaterialManager::MaterialRef player_material_{};
    VulkanEngine::MaterialManager::MaterialRef wall_material_{};
    VulkanEngine::MaterialManager::MaterialRef floor_material_{};
    // Solid-colour textures are uploaded to the bindless heap immediately; keep
    // the CPU resources alive for the lifetime of the run.
    std::vector<std::shared_ptr<VulkanEngine::TextureResource>> textures_{};

    VulkanEngine::Components::Camera* camera_ = nullptr;
    VulkanEngine::Components::Transform* player_transform_ = nullptr;
    std::vector<WallSlot> walls_{};

    VulkanEngine::Input::Action<1> move_{};
    VulkanEngine::Input::ActionHandle restart_handle_{};

    float player_x_ = 0.0f;
    int score_ = 0;
    bool game_over_ = false;
    std::mt19937 rng_{std::random_device{}()};
    float prevGapCenter_ = 0.0f;
};

} // namespace Examples::InfiniteRunner::Game
