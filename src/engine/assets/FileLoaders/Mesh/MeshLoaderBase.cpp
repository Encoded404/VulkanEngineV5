module;

module VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;

import std;
import std.compat;

import VulkanEngine.Mesh.MeshTypes;

namespace VulkanEngine::FileLoaders::Mesh {

void IMeshLoader::PostProcess(VulkanEngine::Mesh& mesh) {
    for (auto& sm : mesh.subMeshes) {
        ComputeSubmeshBounds(sm, mesh.vertices, mesh.indices);
    }
}

} // namespace VulkanEngine::FileLoaders::Mesh
