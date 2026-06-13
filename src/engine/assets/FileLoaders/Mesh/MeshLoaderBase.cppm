module;

// workaround for LLVM #138558: friend/using-decl conflict in bits/shared_ptr.h
#include <memory>
#include <filesystem>

export module VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;

// workaround for LLVM #138558: friend/using-decl conflict in bits/shared_ptr.h
// import std;

export import VulkanEngine.Mesh.MeshTypes;
export import VulkanEngine.MaterialManager.MaterialId;

export namespace VulkanEngine::FileLoaders::Mesh {

using MaterialManager::MaterialId;

class IMeshLoader {
public:
    virtual ~IMeshLoader() = default;

    void SetMaterialBindings(const std::vector<MaterialId>* bindings) { material_bindings_ = bindings; }

    std::shared_ptr<VulkanEngine::Mesh> Load(const std::filesystem::path& path) {
        auto mesh = DoLoad(path);
        PostProcess(*mesh);
        return mesh;
    }

protected:
    virtual std::shared_ptr<VulkanEngine::Mesh> DoLoad(const std::filesystem::path& path) = 0;

    void PostProcess(VulkanEngine::Mesh& mesh);

    const std::vector<MaterialId>* material_bindings_ = nullptr; // NOLINT(misc-non-private-member-variables-in-classes)
};

} // namespace VulkanEngine::FileLoaders::Mesh
