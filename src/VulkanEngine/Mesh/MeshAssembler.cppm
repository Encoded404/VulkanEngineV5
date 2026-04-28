module;

#include <FileLoader/FileLoader.hpp>

export module VulkanEngine.Mesh.MeshAssembler;

import VulkanEngine.Mesh.MeshTypes;

export namespace VulkanEngine {

class MeshAssembler : public FileLoader::IAssembler<VulkanEngine::Mesh, FileLoader::AssemblyMode::FullBuffer> {};
class SkinnedMeshAssembler : public FileLoader::IAssembler<VulkanEngine::SkinnedMesh, FileLoader::AssemblyMode::FullBuffer> {};

} // namespace VulkanEngine
