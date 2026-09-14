module;

export module Examples.InfiniteRunner.Game;

import std;

export import VulkanEngine.GameEngine;
import VulkanShared.CallbackList;

export namespace Examples::InfiniteRunner::Game {

// Infinite runner built entirely on the game layer.
//
// A fixed pool of wall entities scrolls toward a stationary player cube. Each
// wall is two blocks with a randomly placed horizontal gap; the player must
// line up with the gap to pass. There is no engine collision or entity
// destruction, so collision is plain AABB math and walls are recycled in place.
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

    // One wall: two blocks leaving the gap [gap_left_x, gap_right_x] open.
    struct Wall {
        VulkanEngine::Components::Transform* left = nullptr;
        VulkanEngine::Components::Transform* right = nullptr;
        float gap_left_x = 0.0f;  // max x of the left block
        float gap_right_x = 0.0f; // min x of the right block
        float z = 0.0f;
        bool passed = false;
    };

    VulkanEngine::Components::Transform* CreateCubeEntity(float x, float y, float z,
                                                          float sx, float sy, float sz,
                                                          VulkanEngine::MaterialManager::MaterialRef material);

    void RandomizeWall(Wall& wall);
    void ApplyWallTransform(Wall& wall);
    void ResetWalls();
    void ResetRun();
    void UpdatePlayer(const VulkanEngine::Application::ApplicationContext& ctx, float delta_time);
    void UpdateWalls(float delta_time);
    bool PlayerHitsAnyWall() const;

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
    std::vector<Wall> walls_{};

    VulkanEngine::Input::ActionHandle move_left_a_{};
    VulkanEngine::Input::ActionHandle move_left_arrow_{};
    VulkanEngine::Input::ActionHandle move_right_d_{};
    VulkanEngine::Input::ActionHandle move_right_arrow_{};
    VulkanEngine::Input::ActionHandle restart_handle_{};

    float player_x_ = 0.0f;
    int score_ = 0;
    bool game_over_ = false;
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace Examples::InfiniteRunner::Game
