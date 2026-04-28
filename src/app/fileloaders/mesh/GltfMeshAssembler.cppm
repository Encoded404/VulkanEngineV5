module;

#include <memory>
#include <future>
#include <FileLoader/FileLoader.hpp>
#include <FileLoader/Types.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>

export module App.FileLoaders.Mesh.GltfMeshAssembler;

import VulkanEngine.Mesh.MeshTypes;

export namespace App::FileLoaders::Mesh {

class GltfMeshAssembler : public FileLoader::IAssembler<VulkanEngine::Mesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<VulkanEngine::Mesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override {
        auto prom = std::make_shared<std::promise<std::shared_ptr<VulkanEngine::Mesh>>>();

        try {
            fastgltf::Parser parser;

            auto data = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte*>(buffer->data()), buffer->size());
            if (!data) {
                throw std::runtime_error("Failed to create GLTF data buffer");
            }

            auto asset = parser.loadGltfBinary(data.get(), std::filesystem::path(), fastgltf::Options::None);

            if (asset.error() != fastgltf::Error::None) {
                throw std::runtime_error("Failed to load GLTF");
            }

            auto mesh = std::make_shared<VulkanEngine::Mesh>();
            // Basic conversion logic would go here.

            prom->set_value(mesh);
        } catch (...) {
            prom->set_exception(std::current_exception());
        }

        return prom->get_future();
    }
};

} // namespace App::FileLoaders::Mesh
