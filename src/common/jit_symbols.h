#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <shared_mutex>
#include <span>
#include <vector>

namespace Common::JitSymbols {

struct Annotation {
    std::uint32_t begin;
    std::uint32_t end;
    const char* opcode;
};

struct Block {
    std::uint64_t id;
    std::uint64_t owner;
    std::uint64_t generation;
    std::uint64_t process;
    std::uint32_t core;
    std::uint64_t descriptor;
    std::uint64_t guest_begin;
    std::uint64_t guest_end;
    std::uintptr_t host;
    std::size_t size;
    bool annotations_truncated;
    std::vector<Annotation> annotations;
};

void SetEnabled(bool enabled);
bool Enabled();
std::uint64_t CreateOwner(std::uint64_t process, std::uint32_t core);
void DestroyOwner(std::uint64_t owner);
void Clear(std::uint64_t owner);
void Invalidate(std::uint64_t owner, std::uint64_t descriptor);
void Publish(std::uint64_t owner, std::uint64_t descriptor, std::uint64_t guest_begin,
             std::uint64_t guest_end, std::uintptr_t host, std::size_t size,
             std::span<const Annotation> annotations);

class ReadView {
public:
    ReadView();
    bool Ready() const;
    const Block* Find(std::uintptr_t pc) const;
    const std::map<std::uintptr_t, Block>& Blocks() const;

private:
    std::shared_lock<std::shared_mutex> lock;
};

}
