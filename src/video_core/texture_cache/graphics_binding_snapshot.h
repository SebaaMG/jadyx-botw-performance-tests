#pragma once

#include <array>
#include <span>
#include "common/common_types.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/texture.h"

namespace VideoCommon {

struct ImageViewInOut {
    u32 index{};
    bool blacklist{};
    ImageViewId id{};
};

struct GraphicsBindingSnapshot {
    static constexpr size_t CAPACITY = 32;
    struct Image {
        ImageViewInOut view;
        Tegra::Texture::TICEntry descriptor;
    };
    struct Sampler {
        u32 index;
        SamplerId id;
        Tegra::Texture::TSCEntry descriptor;
    };
    std::array<Image, CAPACITY> images;
    std::array<Sampler, CAPACITY> samplers;
    u32 image_count{};
    u32 sampler_count{};
    u64 generation{};
    u64 cookie{};

    template <typename ReadImage, typename ReadSampler>
    bool Capture(std::span<const ImageViewInOut> views, std::span<const SamplerId> sampler_ids,
                 std::span<const u32> sampler_indices, u64 current_generation,
                 ReadImage&& read_image, ReadSampler&& read_sampler) {
        if (views.size() > images.size() || sampler_ids.size() > samplers.size() ||
            sampler_ids.size() != sampler_indices.size()) return false;
        for (size_t index = 0; index < views.size(); ++index) {
            if (views[index].blacklist) return false;
            const auto image = read_image(views[index]);
            if (!image) return false;
            images[index] = *image;
        }
        for (size_t index = 0; index < sampler_ids.size(); ++index) {
            const auto sampler = read_sampler(sampler_indices[index], sampler_ids[index]);
            if (!sampler) return false;
            samplers[index] = *sampler;
        }
        image_count = static_cast<u32>(views.size());
        sampler_count = static_cast<u32>(sampler_ids.size());
        generation = current_generation;
        return true;
    }

    template <typename Generation, typename ReadImage, typename ReadSampler>
    bool Validate(Generation&& current_generation, ReadImage&& read_image,
                  ReadSampler&& read_sampler) const {
        if (image_count > images.size() || sampler_count > samplers.size()) return false;
        if (generation != current_generation()) return false;
        for (u32 index = 0; index < image_count; ++index) {
            const auto& image = images[index];
            const auto descriptor = read_image(image.view.index);
            if (!descriptor || *descriptor != image.descriptor) return false;
        }
        for (u32 index = 0; index < sampler_count; ++index) {
            const auto& sampler = samplers[index];
            const auto descriptor = read_sampler(sampler.index);
            if (!descriptor || *descriptor != sampler.descriptor) return false;
        }
        return generation == current_generation();
    }

    template <typename Generation, typename PrepareImage>
    bool Prepare(Generation&& current_generation, PrepareImage&& prepare_image) const {
        if (image_count > images.size()) return false;
        if (generation != current_generation()) return false;
        for (u32 index = 0; index < image_count; ++index) {
            if (images[index].view.id != NULL_IMAGE_VIEW_ID)
                prepare_image(images[index].view.id);
            if (generation != current_generation()) return false;
        }
        return true;
    }
};

}
