import VulkanEngine.Mesh.MeshAssembler;
import VulkanEngine.Mesh.MeshTypes;
#include <FileLoader/Types.hpp>

#include <memory>
#include <future>
#include <exception>

namespace VulkanEngine {

std::shared_ptr<Mesh> MeshAssembler::ParseFromBuffer(const FileLoader::ByteBuffer& buffer)
{
	auto mesh = std::make_shared<Mesh>();
	mesh->FromBuffer(buffer, 0);
	return mesh;
}

std::future<std::shared_ptr<Mesh>> MeshAssembler::AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer)
{
	auto prom = std::make_shared<std::promise<std::shared_ptr<Mesh>>>();
	try {
		auto mesh = ParseFromBuffer(*buffer);
		prom->set_value(mesh);
	} catch (...) {
		prom->set_exception(std::current_exception());
	}
	return prom->get_future();
}

std::shared_ptr<skinnedMesh> SkinnedMeshAssembler::ParseFromBuffer(const FileLoader::ByteBuffer& buffer)
{
	auto mesh = std::make_shared<skinnedMesh>();
	mesh->FromBuffer(buffer, 0);
	return mesh;
}

std::future<std::shared_ptr<skinnedMesh>> SkinnedMeshAssembler::AssembleFromFullBuffer(std::shared_ptr<FileLoader::ByteBuffer> buffer)
{
	auto prom = std::make_shared<std::promise<std::shared_ptr<skinnedMesh>>>();
	try {
		auto mesh = ParseFromBuffer(*buffer);
		prom->set_value(mesh);
	} catch (...) {
		prom->set_exception(std::current_exception());
	}
	return prom->get_future();
}

} // namespace VulkanEngine

