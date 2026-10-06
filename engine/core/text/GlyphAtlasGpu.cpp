module;

module VulkanEngine.Text.GlyphAtlasGpu;

import std;
import std.compat;

import vulkan_hpp;

import VulkanBackend.Vulkan.VulkanBootstrap;
import VulkanEngine.BindlessManager;
import VulkanEngine.BindlessManager.TextureSlot;
import VulkanEngine.GpuResources.GpuImageHeap;
import VulkanEngine.GpuResources.SamplerCache;
import VulkanEngine.GpuResources.StagingPool;
import VulkanEngine.GpuTexture;
import VulkanEngine.ResourceSystem;
import VulkanEngine.Text.Atlas;
import VulkanEngine.Text.GlyphRaster;

namespace VulkanEngine::Text {

namespace {

// The staging range for one page's regions is one allocation; the alignment is
// the R8 texel block size rounded up to the 4 bytes a copy offset is most
// commonly required to observe. It is never a correctness problem to over-align.
constexpr std::uint64_t kStagingAlignment = 4;

[[nodiscard]] bool Intersects(const AtlasRect& a, const AtlasRect& b) {
    return a.x < static_cast<std::uint64_t>(b.x) + b.width &&
           b.x < static_cast<std::uint64_t>(a.x) + a.width &&
           a.y < static_cast<std::uint64_t>(b.y) + b.height &&
           b.y < static_cast<std::uint64_t>(a.y) + a.height;
}

} // namespace

GlyphAtlasGpu::~GlyphAtlasGpu() = default;

bool GlyphAtlasGpu::Initialize(VulkanBackend::Vulkan::IVulkanBootstrap& backend,
                              GpuResources::GpuImageHeap& heap,
                              GpuResources::StagingPool& staging,
                              BindlessManager::BindlessManager& bindless,
                              GpuResources::SamplerCache* sampler_cache) {
    if (!heap.IsValid() || !staging.IsValid() || !bindless.IsValid()) {
        return false;
    }
    backend_ = &backend;
    heap_ = &heap;
    staging_ = &staging;
    bindless_ = &bindless;
    sampler_cache_ = sampler_cache;
    return true;
}

void GlyphAtlasGpu::Shutdown() {
    // Teardown path: the device is idle (or the caller released every page and
    // drained the bindless ring), so an immediate release is safe here.
    if (bindless_ != nullptr) {
        for (const Page& page : pages_) {
            if (page.handle.IsValid()) {
                bindless_->ReleaseSlotImmediate(page.handle.slot);
            }
        }
    }
    pages_.clear();
    glyph_source_ = {};
    backend_ = nullptr;
    heap_ = nullptr;
    staging_ = nullptr;
    bindless_ = nullptr;
    sampler_cache_ = nullptr;
}

bool GlyphAtlasGpu::CreatePage(const AtlasConfig& config, std::uint32_t page) {
    if (page != pages_.size()) {
        return false; // pages are created in order; a gap is a caller bug
    }
    GpuResources::GpuTexture texture = GpuResources::GpuTexture::CreateStream(
        *backend_, *heap_, config.page_width, config.page_height, vk::Format::eR8Unorm,
        /*linear_filter=*/true, sampler_cache_);
    if (!texture.IsValid()) {
        return false;
    }

    Page record{};
    // Captured before ownership moves into the bindless slot; the image belongs
    // to the heap and stays valid until the slot's deferred destroy applies.
    record.image = texture.GetImage();
    record.view = texture.GetImageView();
    record.layout = vk::ImageLayout::eUndefined;

    const std::string name = "glyph-atlas-page-" + std::to_string(page);
    auto handle = bindless_->AllocateTextureSlot(std::move(texture), ResourceId{name});
    if (!handle.has_value()) {
        // The GpuTexture was moved into the failed call; the heap image leaks
        // nothing because AllocateTextureSlot only rejects before taking it.
        return false;
    }
    record.handle = *handle;
    pages_.push_back(record);
    return true;
}

void GlyphAtlasGpu::TrimPages(std::size_t count, std::uint32_t frame_index) {
    while (pages_.size() > count) {
        const Page& page = pages_.back();
        if (bindless_ != nullptr && page.handle.IsValid()) {
            // Frame-gated: the descriptor stays bound until the FIF drain proves
            // no recorded frame can still sample it, exactly as the texture
            // reload/residency paths retire a slot.
            bindless_->ReleaseSlot(page.handle, frame_index);
        }
        pages_.pop_back();
    }
}

void GlyphAtlasGpu::ReleaseAll(std::uint32_t frame_index) {
    TrimPages(0, frame_index);
}

bool GlyphAtlasGpu::EnsurePages(const GlyphAtlas& atlas) {
    if (!IsValid()) {
        return false;
    }
    const AtlasConfig& config = atlas.Config();
    while (pages_.size() < atlas.PageCount()) {
        if (!CreatePage(config, static_cast<std::uint32_t>(pages_.size()))) {
            break;
        }
    }
    return pages_.size() >= atlas.PageCount();
}

void GlyphAtlasGpu::Transition(vk::CommandBuffer cmd, vk::Image image,
                               vk::ImageLayout from, vk::ImageLayout to) {
    vk::PipelineStageFlags src_stage = vk::PipelineStageFlagBits::eFragmentShader;
    vk::AccessFlags src_access = vk::AccessFlagBits::eShaderRead;
    if (from == vk::ImageLayout::eUndefined) {
        // Nothing has read or written the image yet; the copy must not wait on a
        // stage that could never have touched it.
        src_stage = vk::PipelineStageFlagBits::eTopOfPipe;
        src_access = {};
    }

    vk::PipelineStageFlags dst_stage = vk::PipelineStageFlagBits::eFragmentShader;
    vk::AccessFlags dst_access = vk::AccessFlagBits::eShaderRead;
    if (to == vk::ImageLayout::eTransferDstOptimal) {
        dst_stage = vk::PipelineStageFlagBits::eTransfer;
        dst_access = vk::AccessFlagBits::eTransferWrite;
    }

    vk::ImageMemoryBarrier barrier{};
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = vk::QueueFamilyIgnored;
    barrier.dstQueueFamilyIndex = vk::QueueFamilyIgnored;
    barrier.image = image;
    barrier.subresourceRange =
        vk::ImageSubresourceRange(vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1);
    barrier.srcAccessMask = src_access;
    barrier.dstAccessMask = dst_access;
    cmd.pipelineBarrier(src_stage, dst_stage, {}, {}, {}, barrier);
}

std::uint32_t GlyphAtlasGpu::UploadDirty(GlyphAtlas& atlas, vk::CommandBuffer cmd,
                                         std::uint32_t frame_index) {
    if (!IsValid()) {
        return 0;
    }
    const AtlasConfig& config = atlas.Config();

    // A Reset() drops atlas pages; release the GPU pages above the new count.
    if (pages_.size() > atlas.PageCount()) {
        TrimPages(atlas.PageCount(), frame_index);
    }
    // One persistent image per atlas page, created in order. A failure (heap or
    // bindless capacity) leaves the remaining pages without an image; their
    // dirty state is left set so a later frame retries.
    while (pages_.size() < atlas.PageCount()) {
        if (!CreatePage(config, static_cast<std::uint32_t>(pages_.size()))) {
            break;
        }
    }

    struct Piece {
        std::shared_ptr<const RasterGlyph> glyph;
        std::uint32_t x = 0;
        std::uint32_t y = 0;
    };

    const auto entries = atlas.Entries();
    std::uint32_t written = 0;
    for (std::uint32_t page = 0; page < static_cast<std::uint32_t>(atlas.PageCount()); ++page) {
        const auto dirty = atlas.DirtyRect(page);
        if (!dirty.has_value()) {
            continue;
        }
        if (page >= pages_.size()) {
            // No image and no slot: leave the page dirty so the next frame
            // retries instead of silently sampling nothing.
            continue;
        }
        Page& gpu = pages_[page];

        // A freshly created page and a page an eviction invalidated are entirely
        // dirty: their previous contents are unknown, so every live rectangle is
        // recomposed into a zeroed page. A precise region instead uploads each
        // intersecting glyph's own inner rectangle.
        const bool full = dirty->x == 0 && dirty->y == 0 &&
                          dirty->width == config.page_width &&
                          dirty->height == config.page_height;

        std::vector<Piece> pieces;
        std::uint64_t total_bytes = 0;
        if (full) {
            total_bytes = static_cast<std::uint64_t>(config.page_width) * config.page_height;
        }
        for (const auto& [key, slot] : entries) {
            if (slot.page != page) {
                continue;
            }
            if (!full && !Intersects(slot.rect, *dirty)) {
                continue;
            }
            std::shared_ptr<const RasterGlyph> glyph =
                glyph_source_ ? glyph_source_(key) : nullptr;
            if (glyph == nullptr || glyph->coverage.empty()) {
                continue;
            }
            if (!full) {
                total_bytes += static_cast<std::uint64_t>(glyph->width) * glyph->height;
            }
            pieces.push_back(Piece{std::move(glyph), slot.rect.x + config.padding,
                                   slot.rect.y + config.padding});
        }
        if (!full && pieces.empty()) {
            // The dirty region is only freed space (an Erase); the bytes already
            // on the page are the best available, and nothing samples the slot.
            atlas.ClearDirty(page);
            continue;
        }

        // One staging range per page keeps the retire granularity coarse enough
        // that a frame with a full page does not fragment the pool. Back-pressure
        // leaves the dirty state alone so the next frame retries.
        auto alloc = staging_->Allocate(std::max<std::uint64_t>(total_bytes, 1), kStagingAlignment);
        if (!alloc.has_value()) {
            continue;
        }
        auto* base = static_cast<std::byte*>(alloc->mapped_ptr);

        std::vector<vk::BufferImageCopy> regions;
        if (full) {
            std::memset(base, 0, static_cast<std::size_t>(total_bytes));
            for (const Piece& piece : pieces) {
                const std::uint32_t w = piece.glyph->width;
                const std::uint32_t h = piece.glyph->height;
                if (w == 0 || h == 0 || piece.x + w > config.page_width ||
                    piece.y + h > config.page_height) {
                    continue;
                }
                for (std::uint32_t row = 0; row < h; ++row) {
                    const std::size_t dst = (static_cast<std::size_t>(piece.y + row) *
                                                 config.page_width) +
                                            piece.x;
                    std::memcpy(base + dst, piece.glyph->coverage.data() +
                                               static_cast<std::size_t>(row) * w,
                                w);
                }
            }
            vk::BufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource =
                vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
            region.imageOffset = vk::Offset3D{0, 0, 0};
            region.imageExtent = vk::Extent3D{config.page_width, config.page_height, 1};
            regions.push_back(region);
        } else {
            std::uint64_t offset = 0;
            for (const Piece& piece : pieces) {
                const std::uint32_t w = piece.glyph->width;
                const std::uint32_t h = piece.glyph->height;
                std::memcpy(base + offset, piece.glyph->coverage.data(),
                            piece.glyph->coverage.size());
                vk::BufferImageCopy region{};
                region.bufferOffset = offset;
                region.bufferRowLength = 0; // tightly packed w-wide rows
                region.bufferImageHeight = 0;
                region.imageSubresource =
                    vk::ImageSubresourceLayers(vk::ImageAspectFlagBits::eColor, 0, 0, 1);
                region.imageOffset =
                    vk::Offset3D{static_cast<std::int32_t>(piece.x),
                                 static_cast<std::int32_t>(piece.y), 0};
                region.imageExtent = vk::Extent3D{w, h, 1};
                regions.push_back(region);
                offset += static_cast<std::uint64_t>(w) * h;
            }
        }

        Transition(cmd, gpu.image, gpu.layout, vk::ImageLayout::eTransferDstOptimal);
        staging_->RecordBufferToImage(cmd, *alloc, gpu.image, regions);
        Transition(cmd, gpu.image, vk::ImageLayout::eTransferDstOptimal,
                   vk::ImageLayout::eShaderReadOnlyOptimal);
        gpu.layout = vk::ImageLayout::eShaderReadOnlyOptimal;

        staging_->Retire(*alloc, frame_index);
        atlas.ClearDirty(page);
        ++written;
    }
    return written;
}

} // namespace VulkanEngine::Text
