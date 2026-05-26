module;

#include <memory>
#include <future>
#include <vector>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <FileLoader/FileLoader.hpp>
#include <FileLoader/Types.hpp>
#include <fastgltf/core.hpp>
#include <fastgltf/types.hpp>

export module VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;

import VulkanEngine.Mesh.MeshTypes;
import VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;

export namespace VulkanEngine::FileLoaders::Mesh {

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

class GltfMeshLoader : public IMeshLoader {
public:
    GltfMeshLoader() = default;

protected:
    std::shared_ptr<VulkanEngine::Mesh> DoLoad(const std::filesystem::path& path) override {
        auto buf = ReadEntireFile(path);
        auto buf_ptr = std::make_shared<FileLoader::ByteBuffer>(buf.begin(), buf.end());
        GltfMeshAssembler assembler;
        return assembler.AssembleFromFullBuffer(std::move(buf_ptr)).get();
    }

private:
    static std::vector<std::byte> ReadEntireFile(const std::filesystem::path& file_path) {
        std::ifstream file(file_path, std::ios::binary | std::ios::ate);
        if (!file.is_open()) {
            throw std::runtime_error("GltfMeshLoader: Failed to open file: " + file_path.string());
        }
        const auto size = static_cast<size_t>(file.tellg());
        if (size == 0) {
            throw std::runtime_error("GltfMeshLoader: File is empty: " + file_path.string());
        }
        std::vector<std::byte> buf(size);
        file.seekg(0, std::ios::beg);
        if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
            throw std::runtime_error("GltfMeshLoader: Failed to read file: " + file_path.string());
        }
        return buf;
    }
};

} // namespace VulkanEngine::FileLoaders::Mesh
