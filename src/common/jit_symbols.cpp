#include "common/jit_symbols.h"

#include <algorithm>
#include <atomic>
#include <iterator>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace Common::JitSymbols {
namespace {

constexpr std::size_t MAX_ANNOTATIONS = 2'000'000;
constexpr std::size_t MAX_BLOCK_ANNOTATIONS = 512;

struct Owner {
    std::uint64_t process;
    std::uint32_t core;
    std::uint64_t generation{1};
    std::unordered_map<std::uint64_t, std::uintptr_t> blocks;
};

struct Registry {
    std::shared_mutex mutex;
    std::map<std::uintptr_t, Block> blocks;
    std::unordered_map<std::uint64_t, Owner> owners;
    std::uint64_t next_owner{1};
    std::uint64_t next_block{1};
    std::size_t annotations{};
};

std::atomic_bool enabled{};

Registry& State() {
    static Registry state;
    return state;
}

void Erase(Registry& state, Owner& owner, std::uint64_t descriptor) {
    const auto entry = owner.blocks.find(descriptor);
    if (entry == owner.blocks.end()) {
        return;
    }
    const auto block = state.blocks.find(entry->second);
    if (block != state.blocks.end()) {
        state.annotations -= block->second.annotations.size();
        state.blocks.erase(block);
    }
    owner.blocks.erase(entry);
}

void ClearOwner(Registry& state, Owner& owner) {
    for (const auto& [descriptor, host] : owner.blocks) {
        const auto block = state.blocks.find(host);
        if (block != state.blocks.end()) {
            state.annotations -= block->second.annotations.size();
            state.blocks.erase(block);
        }
    }
    owner.blocks.clear();
    ++owner.generation;
}

}

void SetEnabled(bool value) {
    enabled.store(value, std::memory_order_relaxed);
}

bool Enabled() {
    return enabled.load(std::memory_order_relaxed);
}

std::uint64_t CreateOwner(std::uint64_t process, std::uint32_t core) {
    auto& state = State();
    std::unique_lock lock{state.mutex};
    const auto id = state.next_owner++;
    state.owners.emplace(id, Owner{process, core});
    return id;
}

void DestroyOwner(std::uint64_t id) {
    auto& state = State();
    std::unique_lock lock{state.mutex};
    const auto owner = state.owners.find(id);
    if (owner != state.owners.end()) {
        ClearOwner(state, owner->second);
        state.owners.erase(owner);
    }
}

void Clear(std::uint64_t id) {
    auto& state = State();
    std::unique_lock lock{state.mutex};
    const auto owner = state.owners.find(id);
    if (owner != state.owners.end()) {
        ClearOwner(state, owner->second);
    }
}

void Invalidate(std::uint64_t id, std::uint64_t descriptor) {
    auto& state = State();
    std::unique_lock lock{state.mutex};
    const auto owner = state.owners.find(id);
    if (owner != state.owners.end()) {
        Erase(state, owner->second, descriptor);
    }
}

void Publish(std::uint64_t id, std::uint64_t descriptor, std::uint64_t guest_begin,
             std::uint64_t guest_end, std::uintptr_t host, std::size_t size,
             std::span<const Annotation> annotations) {
    if (size == 0 || size > std::numeric_limits<std::uintptr_t>::max() - host) {
        return;
    }
    auto& state = State();
    std::unique_lock lock{state.mutex};
    const auto owner_entry = state.owners.find(id);
    if (owner_entry == state.owners.end()) {
        return;
    }
    auto& owner = owner_entry->second;
    Erase(state, owner, descriptor);
    auto next = state.blocks.lower_bound(host);
    if ((next != state.blocks.end() && next->first < host + size) ||
        (next != state.blocks.begin() &&
         std::prev(next)->first + std::prev(next)->second.size > host)) {
        return;
    }
    const auto count =
        std::min({annotations.size(), MAX_BLOCK_ANNOTATIONS, MAX_ANNOTATIONS - state.annotations});
    Block block{state.next_block++,
                id,
                owner.generation,
                owner.process,
                owner.core,
                descriptor,
                guest_begin,
                guest_end,
                host,
                size,
                count != annotations.size(),
                {}};
    block.annotations.reserve(count);
    for (const auto& annotation : annotations.first(count)) {
        if (annotation.begin < annotation.end && annotation.end <= size && annotation.opcode) {
            block.annotations.push_back(annotation);
        } else {
            block.annotations_truncated = true;
        }
    }
    state.annotations += block.annotations.size();
    state.blocks.emplace(host, std::move(block));
    owner.blocks.emplace(descriptor, host);
}

ReadView::ReadView() : lock{State().mutex, std::try_to_lock} {}

bool ReadView::Ready() const {
    return lock.owns_lock();
}

const Block* ReadView::Find(std::uintptr_t pc) const {
    if (!Ready()) {
        return nullptr;
    }
    const auto& blocks = State().blocks;
    auto next = blocks.upper_bound(pc);
    if (next == blocks.begin()) {
        return nullptr;
    }
    const auto& block = std::prev(next)->second;
    return pc - block.host < block.size ? &block : nullptr;
}

const std::map<std::uintptr_t, Block>& ReadView::Blocks() const {
    return State().blocks;
}

}
