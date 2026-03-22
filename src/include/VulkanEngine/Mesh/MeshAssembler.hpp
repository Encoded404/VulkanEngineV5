#ifndef VULKAN_ENGINE_MESH_ASSEMBLER_HPP
#define VULKAN_ENGINE_MESH_ASSEMBLER_HPP

#include <memory>
#include <future>
#include <stdexcept>
#include <FileLoader/FileLoader.hpp>
#include "MeshTypes.hpp"

namespace VulkanEngine {

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

#endif // VULKAN_ENGINE_MESH_ASSEMBLER_HPP

