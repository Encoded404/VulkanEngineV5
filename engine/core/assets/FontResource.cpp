module;

#include <logging/logging_macros.hpp>

module VulkanEngine.ResourceSystem.FontResource;

import std;
import std.compat;

import FileLoader.Types;
import logiface;

import VulkanEngine.ResourceSystem;
import VulkanEngine.FileLoaders.Fonts;

namespace VulkanEngine {

FontResource::FontResource(ResourceId id)
    : Resource(std::move(id)) {
    Reset();
}

const std::vector<std::byte>& FontResource::GetBytes() const noexcept {
    return bytes_;
}

FileLoader::ByteSpan FontResource::GetByteSpan() const noexcept {
    return FileLoader::ByteSpan{bytes_.data(), bytes_.size()};
}

FileLoaders::Fonts::FontContainerInfo FontResource::GetContainerInfo() const noexcept {
    return container_;
}

bool FontResource::HasBytes() const noexcept {
    return !bytes_.empty();
}

void FontResource::Reset() noexcept {
    bytes_.clear();
    container_ = FileLoaders::Fonts::FontContainerInfo{};
}

bool FontResource::DoLoad() {
    LOGIFACE_LOG(error, "FontResource '" + GetId().value +
                            "' cannot be loaded without file buffer data");
    return false;
}

bool FontResource::DoUnload() {
    Reset();
    return true;
}

bool FontResource::DoLoadFromBuffer(const FileLoader::ByteBuffer& buf) {
    Reset();

    if (buf.empty()) {
        LOGIFACE_LOG(warn, "FontResource: empty buffer for resource '" + GetId().value + "'");
        return false;
    }

    const FileLoaders::Fonts::FontContainerInfo info =
        FileLoaders::Fonts::InspectFontContainer(FileLoader::ByteSpan{buf.data(), buf.size()});
    if (!info.IsValid()) {
        LOGIFACE_LOG(warn, "FontResource: '" + GetId().value +
                               "' is not a readable sfnt font container");
        return false;
    }

    // Move only after the container check succeeds, so a rejected file leaves
    // the resource empty rather than half-populated.
    bytes_ = buf;
    container_ = info;
    ++version_;
    return true;
}

} // namespace VulkanEngine
