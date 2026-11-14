#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gtest/gtest.h>

import std;

import VulkanEngine.Components.Camera;
import Examples.Explorer.Camera;
import Examples.Explorer.Download;
import Examples.Explorer.Math;

namespace {

using namespace Examples::Explorer;

TEST(ExplorerMathTest, DirectionAngleRoundTrip) {
    for (const float yaw : {-170.0f, -90.0f, 0.0f, 45.0f, 135.0f}) {
        for (const float pitch : {-80.0f, -30.0f, 0.0f, 25.0f, 70.0f}) {
            const glm::vec3 direction = DirectionFromAngles(yaw, pitch);
            EXPECT_NEAR(glm::length(direction), 1.0f, 1e-5f);

            // yaw has a +/-180 ambiguity, so round-trip through the vector.
            const glm::vec2 angles = AnglesFromDirection(direction);
            const glm::vec3 rebuilt = DirectionFromAngles(angles.x, angles.y);
            EXPECT_NEAR(glm::length(rebuilt - direction), 0.0f, 1e-4f);
        }
    }
}

TEST(ExplorerMathTest, OrbitAngleRoundTrip) {
    for (const float yaw : {-180.0f, -45.0f, 0.0f, 60.0f, 179.0f}) {
        for (const float pitch : {-60.0f, -10.0f, 0.0f, 40.0f}) {
            const glm::vec3 offset = OrbitOffsetFromAngles(yaw, pitch);
            EXPECT_NEAR(glm::length(offset), 1.0f, 1e-5f);

            // yaw has a +/-180 ambiguity, so round-trip through the vector.
            const glm::vec2 angles = OrbitAnglesFromOffset(offset);
            const glm::vec3 rebuilt = OrbitOffsetFromAngles(angles.x, angles.y);
            EXPECT_NEAR(glm::length(rebuilt - offset), 0.0f, 1e-4f);
        }
    }
}

TEST(ExplorerMathTest, BoundsCenterAndRadius) {
    const Bounds bounds{glm::vec3{-1.0f, -1.0f, -1.0f}, glm::vec3{3.0f, 1.0f, 1.0f}, true};
    const glm::vec3 center = bounds.Center();
    EXPECT_NEAR(center.x, 1.0f, 1e-5f);
    EXPECT_NEAR(center.y, 0.0f, 1e-5f);
    EXPECT_NEAR(center.z, 0.0f, 1e-5f);
    EXPECT_NEAR(bounds.Radius(), std::sqrt(1.0f + 1.0f + 4.0f), 1e-5f);

    // An empty scene still frames with a usable unit radius, never zero.
    const Bounds empty{};
    EXPECT_FALSE(empty.valid);
    EXPECT_NEAR(empty.Radius(), 1.0f, 1e-5f);
    EXPECT_NEAR(empty.Center().x, 0.0f, 1e-5f);
}

TEST(ExplorerMathTest, OpenPathInterpolatesEndpoints) {
    CameraPath path{};
    path.name = "line";
    path.keys = {{glm::vec3{0.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}},
                 {glm::vec3{10.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}},
                 {glm::vec3{20.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}}};

    EXPECT_NEAR(SamplePath(path, 0.0f).position.x, 0.0f, 1e-4f);
    EXPECT_NEAR(SamplePath(path, 1.0f).position.x, 20.0f, 1e-4f);
    EXPECT_NEAR(SamplePath(path, 0.5f).position.x, 10.0f, 1e-3f);

    // Every key leaves the target unset, so the sampler derives a look-ahead
    // target along the tangent: the camera looks down +X.
    const glm::vec3 forward = SamplePath(path, 0.5f).target - SamplePath(path, 0.5f).position;
    EXPECT_GT(forward.x, 0.0f);

    EXPECT_NEAR(ApproximatePathLength(path), 20.0f, 0.1f);
}

TEST(ExplorerMathTest, ClosedPathWraps) {
    CameraPath path{};
    path.name = "square";
    path.closed = true;
    path.keys = {{glm::vec3{1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}},
                 {glm::vec3{0.0f, 0.0f, 1.0f}, glm::vec3{0.0f, 0.0f, 0.0f}},
                 {glm::vec3{-1.0f, 0.0f, 0.0f}, glm::vec3{0.0f, 0.0f, 0.0f}},
                 {glm::vec3{0.0f, 0.0f, -1.0f}, glm::vec3{0.0f, 0.0f, 0.0f}}};
    ASSERT_TRUE(path.Valid());

    const glm::vec3 at_zero = SamplePath(path, 0.0f).position;
    const glm::vec3 at_one = SamplePath(path, 1.0f).position;
    EXPECT_NEAR(glm::length(at_zero - at_one), 0.0f, 1e-4f);
    // A closed loop returns to the first key instead of stopping there.
    EXPECT_NEAR(glm::length(SamplePath(path, 1.25f).position - SamplePath(path, 0.25f).position),
                0.0f, 1e-4f);
}

TEST(ExplorerCameraTest, FrameBoundsPlacesShowcaseCameraOutsideGeometry) {
    ExplorerCamera camera{};
    camera.SetBounds(Bounds{glm::vec3{-2.0f, -1.0f, -2.0f}, glm::vec3{2.0f, 1.0f, 2.0f}, true});
    camera.SetMode(CameraMode::Showcase);
    camera.FrameBounds();

    VulkanEngine::Components::Camera engine_camera{};
    camera.ApplyTo(engine_camera);

    const glm::vec3 center{0.0f, 0.0f, 0.0f};
    const float distance = glm::length(engine_camera.position - center);
    EXPECT_GT(distance, 2.0f);
    EXPECT_LT(distance, 100.0f);
    // It looks at the scene center, so the forward direction points inward.
    EXPECT_GT(glm::dot(engine_camera.forward, glm::normalize(center - engine_camera.position)),
              0.99f);
    EXPECT_TRUE(engine_camera.projection_mode ==
                VulkanEngine::Components::ProjectionMode::Perspective);
}

TEST(ExplorerCameraTest, ModeSwitchKeepsCameraPosition) {
    ExplorerCamera camera{};
    camera.SetBounds(Bounds{glm::vec3{-1.0f, -1.0f, -1.0f}, glm::vec3{1.0f, 1.0f, 1.0f}, true});
    camera.FrameBounds();

    VulkanEngine::Components::Camera engine_camera{};
    camera.ApplyTo(engine_camera);
    const glm::vec3 before = engine_camera.position;

    camera.SetMode(CameraMode::Freefly);
    camera.ApplyTo(engine_camera);
    EXPECT_NEAR(glm::length(engine_camera.position - before), 0.0f, 1e-3f);

    camera.SetMode(CameraMode::Showcase);
    camera.ApplyTo(engine_camera);
    EXPECT_NEAR(glm::length(engine_camera.position - before), 0.0f, 1e-3f);
}

TEST(ExplorerCameraTest, PathModeRequiresAPathAndAdvances) {
    ExplorerCamera camera{};
    camera.SetBounds(Bounds{glm::vec3{-1.0f, -1.0f, -1.0f}, glm::vec3{1.0f, 1.0f, 1.0f}, true});

    CameraPath path{};
    path.name = "line";
    path.keys = {{glm::vec3{-10.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f}},
                 {glm::vec3{10.0f, 1.0f, 0.0f}, glm::vec3{0.0f, 1.0f, 0.0f}}};
    camera.SetPaths(std::span<const CameraPath>{&path, 1});
    EXPECT_TRUE(camera.HasPaths());

    camera.SetMode(CameraMode::Path);
    EXPECT_TRUE(camera.Mode() == CameraMode::Path);

    VulkanEngine::Components::Camera engine_camera{};
    camera.ApplyTo(engine_camera);
    const float start_x = engine_camera.position.x;

    camera.SetPathPlaying(true);
    camera.SetPathSpeed(4.0f);
    for (int i = 0; i < 120; ++i) {
        camera.Update(CameraInput{}, 1.0f / 60.0f);
    }
    camera.ApplyTo(engine_camera);
    EXPECT_GT(engine_camera.position.x, start_x);
    EXPECT_GT(camera.PathPosition(), 0.0f);

    // Without paths, selecting Path mode must fall back to Showcase instead of
    // leaving the camera with an unresolved pose.
    ExplorerCamera empty{};
    empty.SetMode(CameraMode::Path);
    EXPECT_TRUE(empty.Mode() == CameraMode::Showcase);
}

TEST(ExplorerCameraTest, ZoomStaysWithinSceneScaledRange) {
    ExplorerCamera camera{};
    camera.SetBounds(Bounds{glm::vec3{-5.0f, -5.0f, -5.0f}, glm::vec3{5.0f, 5.0f, 5.0f}, true});
    camera.SetMode(CameraMode::Showcase);
    camera.FrameBounds();

    CameraInput input{};
    input.orbit_drag = true;
    input.wheel = 40.0f;
    for (int i = 0; i < 60; ++i) camera.Update(input, 1.0f / 60.0f);

    VulkanEngine::Components::Camera engine_camera{};
    camera.ApplyTo(engine_camera);
    const float distance = glm::length(engine_camera.position);
    EXPECT_GT(distance, 0.0f);
    EXPECT_LT(distance, 1000.0f);
    // The auto-clip planes follow the scene scale instead of staying fixed.
    EXPECT_GT(engine_camera.far_plane, 100.0f);
}

} // namespace

TEST(ExplorerDownloadTest, Sha256KnownVectors) {
    EXPECT_EQ(Sha256Hex(std::span<const std::byte>{}),
              "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const auto digest = [](std::string_view text) {
        return Sha256Hex(std::span<const std::byte>{
            reinterpret_cast<const std::byte*>(text.data()), text.size()});
    };
    EXPECT_EQ(digest("abc"),
              "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    EXPECT_EQ(digest("The quick brown fox jumps over the lazy dog"),
              "d7a8fbb307d7809469ca9abcb0082e4f8d5651e46d3cdb762d02d0bf37c9e592");
}

TEST(ExplorerDownloadTest, LicensePolicyAllowsOnlyPermissiveLicenses) {
    EXPECT_TRUE(IsPermissiveLicense("CC0-1.0"));
    EXPECT_TRUE(IsPermissiveLicense("CC-BY-4.0"));
    EXPECT_TRUE(IsPermissiveLicense("CC-BY-3.0"));
    // The traps from the asset research must never pass this gate.
    EXPECT_FALSE(IsPermissiveLicense("CC-BY-NC-4.0"));
    EXPECT_FALSE(IsPermissiveLicense("CC-BY-NC-SA-4.0"));
    EXPECT_FALSE(IsPermissiveLicense("CC-BY-SA-4.0"));
    EXPECT_FALSE(IsPermissiveLicense("LicenseRef-CRYENGINE-Agreement"));
    EXPECT_FALSE(IsPermissiveLicense("LicenseRef-3DRT-Testing"));
    EXPECT_FALSE(IsPermissiveLicense(""));
}

TEST(ExplorerDownloadTest, CatalogEntryValidation) {
    DownloadableAsset asset{};
    asset.id = "toy-car";
    asset.name = "Toy Car";
    asset.license = "CC0-1.0";
    asset.url = "https://example.com/ToyCar.glb";
    asset.file_name = "toy-car.glb";
    asset.size = 1234;
    asset.sha256 = std::string(64, 'a');
    EXPECT_TRUE(IsValidDownloadableAsset(asset));

    auto broken = asset;
    broken.url = "http://example.com/ToyCar.glb";
    EXPECT_FALSE(IsValidDownloadableAsset(broken));

    broken = asset;
    broken.file_name = "../escape.glb";
    EXPECT_FALSE(IsValidDownloadableAsset(broken));

    broken = asset;
    broken.license = "CC-BY-NC-4.0";
    EXPECT_FALSE(IsValidDownloadableAsset(broken));

    broken = asset;
    broken.sha256 = "deadbeef";
    EXPECT_FALSE(IsValidDownloadableAsset(broken));

    broken = asset;
    broken.size = 0;
    EXPECT_FALSE(IsValidDownloadableAsset(broken));
}

TEST(ExplorerDownloadTest, InstalledStateIsSizeCheckedAndRemovable) {
    const std::filesystem::path root =
        std::filesystem::temp_directory_path() / "vkengine_explorer_download_tests";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root, ec);
    ASSERT_FALSE(ec);

    {
        SceneDownloader downloader(root);
        DownloadableAsset asset{};
        asset.id = "fixture";
        asset.name = "Fixture";
        asset.license = "CC0-1.0";
        asset.url = "https://example.com/fixture.glb";
        asset.file_name = "fixture.glb";
        asset.size = 4;
        asset.sha256 = std::string(64, 'a');

        EXPECT_FALSE(downloader.IsInstalled(asset));
        EXPECT_TRUE(downloader.InstalledFiles().empty());

        {
            std::ofstream file(downloader.InstallPath(asset), std::ios::binary);
            ASSERT_TRUE(file.is_open());
            file.write("glTF", 4);
        }
        EXPECT_TRUE(downloader.IsInstalled(asset));
        EXPECT_EQ(downloader.InstalledFiles().size(), 1u);

        // A wrong-sized file is not "installed", even at the right path.
        std::filesystem::resize_file(downloader.InstallPath(asset), 2, ec);
        ASSERT_FALSE(ec);
        EXPECT_FALSE(downloader.IsInstalled(asset));

        std::filesystem::resize_file(downloader.InstallPath(asset), 4, ec);
        ASSERT_FALSE(ec);
        EXPECT_TRUE(downloader.Remove(asset));
        EXPECT_FALSE(downloader.IsInstalled(asset));
    }

    std::filesystem::remove_all(root, ec);
}

TEST(ExplorerCameraTest, ShowcaseDragDownRaisesTheCamera) {
    ExplorerCamera camera{};
    camera.SetBounds(Bounds{glm::vec3{-2.0f, -1.0f, -2.0f}, glm::vec3{2.0f, 1.0f, 2.0f}, true});
    camera.SetMode(CameraMode::Showcase);
    camera.FrameBounds();

    VulkanEngine::Components::Camera before{};
    camera.ApplyTo(before);

    CameraInput input{};
    input.orbit_drag = true;
    input.mouse_delta = glm::vec2{0.0f, 40.0f};  // pointer moves down
    camera.Update(input, 1.0f / 60.0f);

    VulkanEngine::Components::Camera after{};
    camera.ApplyTo(after);
    // Dragging down orbits the camera above the target, so the eye rises and
    // the view tilts downward.
    EXPECT_GT(after.position.y, before.position.y);
    EXPECT_LT(after.forward.y, before.forward.y);
}
