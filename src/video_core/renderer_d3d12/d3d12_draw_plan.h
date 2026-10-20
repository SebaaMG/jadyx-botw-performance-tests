#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include "common/common_types.h"
#include "video_core/texture_cache/graphics_binding_snapshot.h"

namespace D3D12 {

struct StageBindings;
void SetDrawPlanReuse(bool enabled, bool validate = false);
bool DrawPlanReuseEnabled();
bool DrawPlanValidationEnabled();

struct DrawPlanKey {
    std::array<const StageBindings*, 5> stages{};
    const void* channel{};
    u64 memory_id{};
    u64 mapping_generation{};
    u64 view_generation{};
    GPUVAddr image_table{};
    GPUVAddr sampler_table{};
    u32 image_limit{};
    u32 sampler_limit{};
    bool via_header{};
    bool operator==(const DrawPlanKey&) const = default;
};

class DrawPlanCache {
public:
    static constexpr size_t CAPACITY = 16;
    static constexpr size_t MAX_WORDS = 128;
    struct Word {
        GPUVAddr base;
        u32 offset;
        u32 value;
        u32 stage;
        u32 buffer;
    };
    struct Entry {
        DrawPlanKey key{};
        std::array<std::shared_ptr<const StageBindings>, 5> owners;
        std::array<Word, MAX_WORDS> words;
        VideoCommon::GraphicsBindingSnapshot bindings;
        std::array<u32, VideoCommon::GraphicsBindingSnapshot::CAPACITY> sampler_indices;
        u32 word_count{};
        u32 sampler_count{};
        bool valid{};
        bool overflow{};

        void Record(u32 stage, u32 buffer, GPUVAddr base, u32 offset, u32 value) {
            if (word_count == words.size()) {
                overflow = true;
                return;
            }
            words[word_count++] = {base, offset, value, stage, buffer};
        }
        void RecordSampler(u32 index) {
            if (sampler_count == sampler_indices.size()) {
                overflow = true;
                return;
            }
            sampler_indices[sampler_count++] = index;
        }
        template <typename Base, typename Read>
        bool InputsMatch(Base&& base, Read&& read) const {
            for (u32 index = 0; index < word_count; ++index) {
                const auto& word = words[index];
                if (base(word.stage, word.buffer) != word.base ||
                    read(word.base + word.offset) != word.value) {
                    return false;
                }
            }
            return true;
        }
    };
    struct Stats {
        u64 attempts{}, unsupported{};
        u64 queries{}, hits{}, key_misses{}, input_misses{}, descriptor_misses{};
        u64 refresh_misses{}, captures{}, uncacheable{}, validated{}, mismatches{};
    } stats;

    DrawPlanCache() : enabled{DrawPlanReuseEnabled()}, validate{DrawPlanValidationEnabled()} {}
    explicit DrawPlanCache(bool enabled_, bool validate_ = false)
        : enabled{enabled_}, validate{validate_} {}
    bool Enabled() const { return enabled && stats.mismatches == 0; }
    bool Validate() const { return validate; }
    Entry& Select(const DrawPlanKey& key) {
        size_t hash = key.memory_id;
        for (const auto* stage : key.stages) {
            hash ^= (reinterpret_cast<size_t>(stage) >> 4) + (hash << 6) + (hash >> 2);
        }
        return entries[hash & (CAPACITY - 1)];
    }
    void Begin(Entry& entry, const DrawPlanKey& key,
               const std::array<std::shared_ptr<const StageBindings>, 5>& owners) {
        entry.valid = false;
        if (entry.key.stages != key.stages) entry.owners = owners;
        entry.key = key;
        entry.word_count = 0;
        entry.sampler_count = 0;
        entry.overflow = false;
    }
    void Commit(Entry& entry, bool captured) {
        entry.valid = captured && !entry.overflow && entry.key.mapping_generation != ~u64{} &&
                      Enabled();
        if (entry.valid) {
            entry.bindings.cookie = ++next_cookie;
            ++stats.captures;
        } else {
            ++stats.uncacheable;
        }
    }

private:
    std::array<Entry, CAPACITY> entries;
    u64 next_cookie{};
    bool enabled{};
    bool validate{};
};

}
