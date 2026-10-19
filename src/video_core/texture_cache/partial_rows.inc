// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from ProsperoEden a26ba788, headless/texture_rows.inc. Included in util.cpp.
// Port (texture_partial.h): upload the block rows of a large texture that guest writes changed.
namespace {
struct RowLevel {
    Extent3D level_size, num_tiles;
    u32 block_height = 0, stride_alignment = 0, row_bytes = 0, row_tiles = 0, rows = 0, guest_offset = 0;
};
// The decoder's layout of each level, or nothing when it differs from the level sizes the image's
// guest size is made of (single-level images and tile width spacing are left to the whole upload).
std::optional<boost::container::small_vector<RowLevel, 16>> RowLevels(const ImageInfo& info, u32 guest_size) {
    if (VideoCore::Surface::GetFormatType(info.format) != VideoCore::Surface::SurfaceType::ColorTexture ||
        info.type != ImageType::e2D || info.resources.layers != 1 || info.resources.levels < 2 ||
        info.resources.levels > static_cast<s32>(MAX_MIP_LEVELS) ||
        info.tile_width_spacing != 0 || info.block.depth != 0 || info.size.depth != 1 || info.num_samples != 1) {
        return std::nullopt;
    }
    const u32 bpp_log2 = BytesPerBlockLog2(info.format);
    const Extent2D tile_size = DefaultBlockSize(info.format);
    const LevelInfo level_info = MakeLevelInfo(info);
    const s32 num_levels = info.resources.levels;
    const std::array level_sizes = CalculateLevelSizes(level_info, num_levels);
    if (CalculateLevelBytes(level_sizes, num_levels) != guest_size) return std::nullopt;
    const Extent2D gob = GobSize(bpp_log2, info.block.height, info.tile_width_spacing);
    boost::container::small_vector<RowLevel, 16> levels(num_levels);
    u32 guest_offset = 0;
    for (s32 level = 0; level < num_levels; ++level) {
        RowLevel& row = levels[level];
        row.level_size = AdjustMipSize(info.size, level);
        row.num_tiles = AdjustTileSize(row.level_size, tile_size);
        row.block_height = AdjustMipBlockSize(row.num_tiles, level_info.block, level, level_info.num_levels).height;
        row.stride_alignment = StrideAlignment(row.num_tiles, info.block, gob, bpp_log2);
        // Tegra::Texture::UnswizzleTexture / SwizzleImpl: stride, GOB columns, one block row.
        const u32 stride = Common::AlignUpLog2(row.num_tiles.width, row.stride_alignment) << bpp_log2;
        row.row_bytes = Common::DivCeilLog2(stride, GOB_SIZE_X_SHIFT) << (GOB_SIZE_SHIFT + row.block_height);
        row.row_tiles = 1U << (GOB_SIZE_Y_SHIFT + row.block_height);
        row.rows = Common::DivCeil(row.num_tiles.height, row.row_tiles);
        if (static_cast<u64>(row.rows) * row.row_bytes > level_sizes[level]) return std::nullopt;
        row.guest_offset = guest_offset;
        guest_offset += level_sizes[level];
    }
    return levels;
}
} // Anonymous namespace

bool CanUnswizzleImageRows(const ImageInfo& info, u32 guest_size) {
    return RowLevels(info, guest_size).has_value();
}

std::optional<std::vector<BufferImageCopy>> UnswizzleImageRows(
    const ImageInfo& info, u32 guest_size, std::span<const std::pair<u32, u32>> dirty,
    const std::function<void(u32 offset, u8* data, u32 size)>& read, Common::ScratchBuffer<u8>& output) {
    const auto levels = RowLevels(info, guest_size);
    if (!levels) return std::nullopt;
    const u32 bpp_log2 = BytesPerBlockLog2(info.format);
    const Extent2D tile_size = DefaultBlockSize(info.format);
    struct Band { s32 level; u32 first, last; };
    std::vector<Band> bands;
    size_t host_size = 0;
    for (s32 level = 0; level < static_cast<s32>(levels->size()); ++level) {
        const RowLevel& row = (*levels)[level];
        const u32 level_end = row.guest_offset + row.rows * row.row_bytes;
        for (const auto& [begin, end] : dirty) {
            if (end <= row.guest_offset || begin >= level_end) continue;
            const u32 first = ((std::max)(begin, row.guest_offset) - row.guest_offset) / row.row_bytes;
            const u32 last = Common::DivCeil((std::min)(end, level_end) - row.guest_offset, row.row_bytes);
            if (!bands.empty() && bands.back().level == level && first <= bands.back().last) {
                bands.back().last = (std::max)(bands.back().last, last);
            } else {
                bands.push_back({level, first, last});
            }
        }
    }
    std::vector<BufferImageCopy> copies;
    copies.reserve(bands.size());
    for (const Band& band : bands) {
        const RowLevel& row = (*levels)[band.level];
        const u32 tile_y = band.first * row.row_tiles;
        const u32 tiles_h = (std::min)(row.num_tiles.height, band.last * row.row_tiles) - tile_y;
        const u32 pixel_y = tile_y * tile_size.height;
        const size_t bytes = static_cast<size_t>(row.num_tiles.width) * tiles_h << bpp_log2;
        copies.push_back(BufferImageCopy{
            .buffer_offset = host_size,
            .buffer_size = bytes,
            .buffer_row_length = Common::AlignUp(row.level_size.width, tile_size.width),
            .buffer_image_height = tiles_h * tile_size.height,
            .image_subresource = {.base_level = band.level, .base_layer = 0, .num_layers = 1},
            .image_offset = {0, static_cast<s32>(pixel_y), 0},
            .image_extent = {row.level_size.width, (std::min)(row.level_size.height - pixel_y,
                                                              tiles_h * tile_size.height), 1},
        });
        host_size += bytes;
    }
    output.resize_destructive(host_size);
    static thread_local std::vector<u8> guest;
    for (size_t i = 0; i < bands.size(); ++i) {
        const RowLevel& row = (*levels)[bands[i].level];
        const u32 guest_begin = row.guest_offset + bands[i].first * row.row_bytes;
        const u32 guest_bytes = (bands[i].last - bands[i].first) * row.row_bytes;
        guest.resize(guest_bytes);
        read(guest_begin, guest.data(), guest_bytes);
        const u32 tiles_h = copies[i].buffer_image_height / tile_size.height;
        UnswizzleTexture(std::span<u8>(output.data() + copies[i].buffer_offset, copies[i].buffer_size), guest,
                         1U << bpp_log2, row.num_tiles.width, tiles_h, 1, row.block_height, 0,
                         row.stride_alignment);
    }
    return copies;
}
