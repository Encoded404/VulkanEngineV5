module;

#include <memory>
#include <filesystem>

export module VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;

export import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine::FileLoaders::Mesh {

class IMeshLoader {
public:
    virtual ~IMeshLoader() = default;

    std::shared_ptr<VulkanEngine::Mesh> Load(const std::filesystem::path& path) {
        auto mesh = DoLoad(path);
        PostProcess(*mesh);
        return mesh;
    }

protected:
    virtual std::shared_ptr<VulkanEngine::Mesh> DoLoad(const std::filesystem::path& path) = 0;

    void PostProcess(VulkanEngine::Mesh& mesh);
};

} // namespace VulkanEngine::FileLoaders::Mesh
