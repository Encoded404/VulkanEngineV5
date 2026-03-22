#ifndef __INTERNAL_MODULE_FRAGMENT
module;
#endif

#include <memory>
#include <future>
#include <FileLoader/FileLoader.hpp>

export module VulkanEngine.Mesh.MeshAssembler;

import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

class MeshAssembler : public FileLoader::IAssembler<Mesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<Mesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override;
    static std::shared_ptr<Mesh> ParseFromBuffer(const FileLoader::ByteBuffer& buffer);
};

class SkinnedMeshAssembler : public FileLoader::IAssembler<skinnedMesh, FileLoader::AssemblyMode::FullBuffer>
{
public:
    std::future<std::shared_ptr<skinnedMesh>> AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer) override;
    static std::shared_ptr<skinnedMesh> ParseFromBuffer(const FileLoader::ByteBuffer& buffer);
};

} // namespace VulkanEngine
