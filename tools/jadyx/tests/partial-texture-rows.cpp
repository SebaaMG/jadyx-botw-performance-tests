// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Use the production D3D12 fixture; its original tests keep their own entry point.
#define main OriginalDepthGate
#include "tools/xbox/tests/depth-region-copy.cpp"
#undef main
#include <map>
#include <random>
#include "video_core/texture_cache/util.h"
#include "video_core/texture_cache/partial_upload.h"
#include "video_core/buffer_cache/binding_dependency.h"
#include "video_core/texture_cache/descriptor_table.h"

struct DescriptorMemory {
    std::array<std::array<u32,8>,8> descriptors{};
    void ReadBlockUnsafe(u64 address,void* data,size_t size) const {
        if (address+size>sizeof(descriptors)) std::terminate();
        std::memcpy(data,reinterpret_cast<const u8*>(descriptors.data())+address,size);
    }
};
void DescriptorInvalidation() {
    DescriptorMemory memory;
    DescriptorTable<std::array<u32,8>> table;
    Check(table.Synchronize(0,3),"new descriptor table not refreshed");
    Check(table.Read(memory,2).second && !table.Read(memory,2).second,"descriptor change detection");
    memory.descriptors[2][5]=123;
    Check(table.Read(memory,2).second,"live TIC/TSC byte change not detected");
    table.Invalidate(); // deletion, rescale or unmap invalidates the production table
    Check(table.Read(memory,2).second,"deleted view could reuse dense slot without rebuilding");
    Check(table.Synchronize(32,3) && table.Read(memory,2).second,"table address change not detected");
    Check(table.Synchronize(0,1) && table.Read(memory,1).second,"table shrink not detected");
    Check(table.Synchronize(0,3) && table.Read(memory,2).second,"table regrowth reused stale slot");
}

struct PageMemory {
    std::map<u64,int> counts;
    void UpdatePagesCachedCount(u64 address,u64 size,int delta) {
        for (u64 p=address>>12;p<=(address+size-1)>>12;++p) {
            counts[p]+=delta;
            Check(counts[p]>=1,"partial tracking removed another owner's page");
        }
    }
};
void PageAccounting() {
    PageMemory memory;
    constexpr u64 base=0x100123, bytes=0x12ab3;
    for (u64 p=base>>12;p<=(base+bytes-1)>>12;++p) memory.counts[p]=2;
    const auto original=memory.counts;
    PartialUploadState state;
    state.Write(memory,base,bytes,base+8,8192);
    const auto first=memory.counts;
    state.Write(memory,base,bytes,base+8,8192);
    Check(first==memory.counts,"duplicate writes untracked pages twice");
    state.Write(memory,base,bytes,base+bytes-10,4096);
    const auto pages=state.End(memory);
    Check(memory.counts==original && state.dirty_pages.empty(),"refresh/untrack did not restore counts");
    const auto ranges=PartialUploadState::Ranges(pages,base,bytes);
    Check(ranges.size()==2 && ranges.front().first==0 && ranges.back().second==bytes,
          "unaligned edge pages escaped image bounds");
    Check(state.End(memory).empty() && memory.counts==original,"repeated end altered counts");
    for (int pass=0;pass<100;++pass) {
        state.Write(memory,base,bytes,base+pass*100,4097);
        (void)state.End(memory); // same balance required before full untrack, deletion and unmap
        Check(memory.counts==original,"page counts drifted after repeated updates");
    }
}
void PlanDependencies() {
    StoragePlanCache<u64> cache;
    const StoragePlanKey key{11,5,0x1000,0x2000,4096,256,false};
    Check(!cache.Find(key),"uninitialized storage plan hit");
    cache.Insert(key,123);
    Check(cache.Find(key) && *cache.Find(key)==123,"stable dependency missed");
    auto changed=key;
    for (u32 field=0;field<7;++field) {
        changed=key;
        switch(field) {
        case 0: ++changed.address_space;break;
        case 1: ++changed.mapping_generation;break;
        case 2: ++changed.descriptor_address;break;
        case 3: ++changed.address;break;
        case 4: ++changed.size_word;break;
        case 5: ++changed.alignment;break;
        case 6: changed.written=true;break;
        }
        Check(!cache.Find(changed),"changed storage dependency reused old plan");
    }
    changed=key; changed.descriptor_address+=1ULL<<20;
    cache.Insert(changed,456);
    Check(!cache.Find(key) && *cache.Find(changed)==456,"direct-slot collision reused another plan");
}

struct Rows {
    Common::ScratchBuffer<u8> bytes;
    std::vector<BufferImageCopy> copies;
    size_t read_bytes{};
};
Rows DecodeRows(const ImageInfo& info,std::span<const u8> guest,
                std::span<const std::pair<u32,u32>> dirty,std::span<const u8> whole,
                std::span<const BufferImageCopy> whole_copies) {
    Rows rows;
    const auto read=[&](u32 offset,u8* data,u32 size) {
        Check(u64(offset)+size<=guest.size(),"partial guest read escaped image");
        std::memcpy(data,guest.data()+offset,size); rows.read_bytes+=size;
    };
    auto decoded=UnswizzleImageRows(info,u32(guest.size()),dirty,read,rows.bytes);
    Check(decoded.has_value(),"supported layout unexpectedly fell back");
    rows.copies=std::move(*decoded);
    const auto bw=VideoCore::Surface::DefaultBlockWidth(info.format);
    const auto bh=VideoCore::Surface::DefaultBlockHeight(info.format);
    const auto bpp=VideoCore::Surface::BytesPerBlock(info.format);
    for (const auto& copy:rows.copies) {
        const u32 level=copy.image_subresource.base_level;
        const u32 width=std::max(1U,info.size.width>>level);
        const size_t stride=size_t((width+bw-1)/bw)*bpp;
        const size_t at=whole_copies[level].buffer_offset+(copy.image_offset.y/bh)*stride;
        Check(at+copy.buffer_size<=whole.size(),"partial row escaped full decode");
        Check(!std::memcmp(rows.bytes.data()+copy.buffer_offset,whole.data()+at,copy.buffer_size),
              "partial unswizzle differs from full production decoder");
    }
    return rows;
}

int main(int argc,char** argv) try {
    Common::Log::Initialize();
    Settings::values.renderer_debug=true;
    D3D12::SetOrderedCopyReuse(true);
    D3D12::SetCopyPlanCache(true);
    D3D12::SetAsyncRecording(argc>1 && std::string{argv[1]}=="--async");
    PageAccounting(); PlanDependencies(); DescriptorInvalidation();
    Gate gate;
    std::mt19937 random(0xabc123);
    u32 layouts=0,gpu_cases=0;
    u64 full_bytes=0,partial_bytes=0;
    for (auto format:{PixelFormat::A8B8G8R8_UNORM,PixelFormat::R8_UNORM,
                      PixelFormat::BC4_UNORM,PixelFormat::BC3_UNORM}) {
        for (u32 height_shift:{0U,1U,3U,5U}) for (u32 width:{65U,256U,513U,2048U}) {
            ImageInfo info;
            info.format=format; info.type=ImageType::e2D; info.resources={5,1};
            info.size={width,width+20,1}; info.block={0,height_shift,0};
            const auto size=CalculateGuestSizeInBytes(info);
            Check(CanUnswizzleImageRows(info,size),"expected row layout rejected");
            std::vector<u8> guest(size),whole(CalculateUnswizzledSizeBytes(info));
            for (auto& b:guest) b=u8(random());
            const auto copies=UnswizzleBlockLinearImage(info,guest,whole);
            ++layouts;
            for (u32 pass=0;pass<12;++pass) {
                const u32 begin=random()%size, end=std::min(size,begin+1+random()%8192);
                std::vector<std::pair<u32,u32>> dirty{{begin,end}};
                (void)DecodeRows(info,guest,dirty,whole,copies);
            }
            // Real resource: leave all other rows/mips untouched, then compare the whole readback.
            if (height_shift!=3 || width!=2048) continue;
            Image image{gate.runtime,info,0,0};
            Check(False(image.flags & ImageFlagBits::Converted),"GPU fixture is converted, outside row-upload eligibility");
            Check(size >= (1U<<20),"GPU fixture is smaller than partial-upload eligibility");
            std::cerr<<"GPU rows format="<<u32(format)<<" width="<<width<<" height="<<info.size.height<<'\n';
            auto initial=gate.runtime.UploadStagingBuffer(whole.size());
            std::memcpy(initial.mapped_span.data(),whole.data(),whole.size());
            image.UploadMemory(initial,copies);
            auto original=gate.runtime.DownloadStagingBuffer(whole.size());
            image.DownloadMemory(original,copies);
            gate.scheduler.Finish();
            gate.CheckDebugLayer("whole texture initialization");
            Check(!std::memcmp(original.mapped_span.data(),whole.data(),whole.size()),
                  "initial full upload/readback differs before partial updates");
            const auto old_whole=whole;
            const u32 first=size/3 & ~4095U;
            const u32 last=std::min(size,first+4096);
            for (u32 i=first;i<last;++i) guest[i]^=0x5a;
            const auto updated_copies=UnswizzleBlockLinearImage(info,guest,whole);
            const std::array dirty{std::pair{first,last}};
            auto rows=DecodeRows(info,guest,dirty,whole,updated_copies);
            auto composite=old_whole;
            const u32 bw=VideoCore::Surface::DefaultBlockWidth(format);
            const u32 bh=VideoCore::Surface::DefaultBlockHeight(format);
            const u32 bpp=VideoCore::Surface::BytesPerBlock(format);
            for (auto c:rows.copies) {
                const u32 level=c.image_subresource.base_level;
                const size_t stride=size_t((std::max(1U,width>>level)+bw-1)/bw)*bpp;
                const size_t at=updated_copies[level].buffer_offset+(c.image_offset.y/bh)*stride;
                std::memcpy(composite.data()+at,rows.bytes.data()+c.buffer_offset,c.buffer_size);
            }
            Check(composite==whole,"row selection missed changed bytes outside selected bands");
            Check(rows.bytes.size()<whole.size(),"small write still decoded the full image");
            auto upload=gate.runtime.UploadStagingBuffer(rows.bytes.size());
            std::memcpy(upload.mapped_span.data(),rows.bytes.data(),rows.bytes.size());
            image.UploadMemory(upload,rows.copies);
            auto readback=gate.runtime.DownloadStagingBuffer(whole.size());
            image.DownloadMemory(readback,updated_copies);
            gate.scheduler.Finish();
            gate.CheckDebugLayer("partial texture rows");
            if (std::memcmp(readback.mapped_span.data(),whole.data(),whole.size())) {
                size_t bad=0;
                while (bad<whole.size() && readback.mapped_span[bad]==whole[bad]) ++bad;
                std::cerr<<"first mismatch at "<<bad<<" expected="<<u32(whole[bad])
                         <<" actual="<<u32(readback.mapped_span[bad])<<'\n';
                for (auto c:rows.copies) std::cerr<<"copy mip="<<c.image_subresource.base_level
                    <<" y="<<c.image_offset.y<<" h="<<c.image_extent.height<<" bytes="<<c.buffer_size<<'\n';
            }
            Check(!std::memcmp(readback.mapped_span.data(),whole.data(),whole.size()),
                  "partial GPU upload changed untouched rows or lost written data");
            full_bytes+=whole.size();partial_bytes+=rows.bytes.size();++gpu_cases;
        }
    }
    ImageInfo excluded;
    excluded.format=PixelFormat::A8B8G8R8_UNORM; excluded.type=ImageType::e2D;
    excluded.resources={1,1}; excluded.size={64,64,1};
    Check(!CanUnswizzleImageRows(excluded,CalculateGuestSizeInBytes(excluded)),"single mip accepted");
    excluded.resources={2,2}; excluded.layer_stride=65536;
    Check(!CanUnswizzleImageRows(excluded,CalculateGuestSizeInBytes(excluded)),"array accepted");
    excluded.resources={2,1}; excluded.format=PixelFormat::D32_FLOAT;
    Check(!CanUnswizzleImageRows(excluded,CalculateGuestSizeInBytes(excluded)),"depth partial upload accepted");
    ThrowIfFailed(gate.device.Get()->GetDeviceRemovedReason(),"row test device removed");
    std::cout<<"PASS: "<<layouts<<" layouts, "<<(layouts*12)<<" row/full byte comparisons, "<<gpu_cases
             <<" GPU full-readback cases, page accounting, storage dependencies; D3D12 debug clean; "
             <<"GPU partial_bytes="<<partial_bytes<<" vs full_bytes="<<full_bytes<<'\n';
    return 0;
} catch(const std::exception& e) { std::cerr<<e.what()<<'\n';return 1; }
