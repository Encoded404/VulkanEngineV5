module;

#include <memory>
#include <future>
#include <filesystem>
#include <fstream>
#include <vector>
#include <string>
#include <stdexcept>
#include <array>
#include <cstddef>
#include <algorithm>
#include <cctype>
#include <FileLoader/FileLoader.hpp>
#include <FileLoader/Types.hpp>

export module VulkanEngine.FileLoaders.Mesh.MeshMagicLoader;

import VulkanEngine.FileLoaders.Mesh.BinMeshAssembler;
import VulkanEngine.FileLoaders.Mesh.GltfMeshAssembler;
import VulkanEngine.FileLoaders.Mesh.ObjMeshAssembler;
import VulkanEngine.FileLoaders.Mesh.MeshLoaderBase;
import VulkanEngine.Mesh.MeshTypes;

namespace {

[[nodiscard]] std::vector<std::byte> ReadEntireFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        throw std::runtime_error("MeshMagicLoader: Failed to open file: " + path.string());
    }
    const auto size = static_cast<size_t>(file.tellg());
    if (size == 0) {
        throw std::runtime_error("MeshMagicLoader: File is empty: " + path.string());
    }
    std::vector<std::byte> buf(size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        throw std::runtime_error("MeshMagicLoader: Failed to read file: " + path.string());
    }
    return buf;
}

[[nodiscard]] bool HasMagic(const std::vector<std::byte>& buf, const std::vector<std::byte>& magic)
{
    if (buf.size() < magic.size()) return false;
    return std::equal(magic.begin(), magic.end(), buf.begin());
}

[[nodiscard]] std::string LowerExtension(const std::filesystem::path& path)
{
    auto ext = path.extension().string();
    std::ranges::transform(ext, ext.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext;
}

} // anonymous namespace

export namespace VulkanEngine::FileLoaders::Mesh {

class MeshMagicLoader : public IMeshLoader {
public:
    MeshMagicLoader() = default;

protected:
    std::shared_ptr<VulkanEngine::Mesh> DoLoad(const std::filesystem::path& path) override {
        auto buf = ReadEntireFile(path);
        auto buf_ptr = std::make_shared<FileLoader::ByteBuffer>(buf.begin(), buf.end());

        // Magic bytes for binary formats
        static constexpr std::array<std::byte, 3> kBinMagic = {
            std::byte{0x1B}, std::byte{0xEF}, std::byte{0xF8}
        };
        static constexpr std::array<std::byte, 4> kGlbMagic = {
            std::byte{'g'}, std::byte{'l'}, std::byte{'T'}, std::byte{'F'}
        };

        std::vector<std::byte> buf_prefix;
        auto needs = std::max(kBinMagic.size(), kGlbMagic.size());
        buf_prefix.reserve(needs);
        for (size_t i = 0; i < needs && i < buf.size(); ++i) {
            buf_prefix.push_back(buf[i]);
        }

        std::shared_ptr<FileLoader::IAssembler<VulkanEngine::Mesh, FileLoader::AssemblyMode::FullBuffer>> assembler;

        if (HasMagic(buf_prefix, {kBinMagic.begin(), kBinMagic.end()})) {
            assembler = std::make_shared<BinMeshAssembler>();
        } else if (HasMagic(buf_prefix, {kGlbMagic.begin(), kGlbMagic.end()})) {
            assembler = std::make_shared<GltfMeshAssembler>();
        } else {
            auto ext = LowerExtension(path);
            if (ext == ".obj") {
                assembler = std::make_shared<ObjMeshAssembler>();
            } else if (ext == ".gltf" || ext == ".glb") {
                assembler = std::make_shared<GltfMeshAssembler>();
            } else {
                throw std::runtime_error("MeshMagicLoader: Unknown mesh format for file: " + path.string());
            }
        }

        return assembler->AssembleFromFullBuffer(std::move(buf_ptr)).get();
    }
};

[[nodiscard]] inline std::vector<std::string> KnownMeshExtensions()
{
    return {".bin", ".obj", ".gltf", ".glb"};
}

[[nodiscard]] std::future<std::shared_ptr<VulkanEngine::Mesh>> LoadMeshFromFile(const std::filesystem::path& path)
{
    auto prom = std::make_shared<std::promise<std::shared_ptr<VulkanEngine::Mesh>>>();
    try {
        MeshMagicLoader loader;
        auto mesh = loader.Load(path);
        prom->set_value(mesh);
    } catch (...) {
        prom->set_exception(std::current_exception());
    }
    return prom->get_future();
}

} // namespace VulkanEngine::FileLoaders::Mesh
