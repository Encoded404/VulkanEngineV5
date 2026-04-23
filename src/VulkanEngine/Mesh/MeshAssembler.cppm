module;

#include <memory>
#include <future>
#include <FileLoader/FileLoader.hpp>
#include <FileLoader/Types.hpp>

export module VulkanEngine.Mesh.MeshAssembler;

import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

class MeshAssembler : public FileLoader::IAssembler<VulkanEngine::Mesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<VulkanEngine::Mesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override;
    static std::shared_ptr<VulkanEngine::Mesh> ParseFromBuffer(const FileLoader::ByteBuffer& buffer);
};

class SkinnedMeshAssembler : public FileLoader::IAssembler<VulkanEngine::SkinnedMesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<VulkanEngine::SkinnedMesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override;
    static std::shared_ptr<VulkanEngine::SkinnedMesh> ParseFromBuffer(const FileLoader::ByteBuffer& buffer);
};

} // namespace VulkanEngine
