// SPDX-FileCopyrightText: Copyright 2026 SeriesX contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Production descriptor queue, root signature and GPU. No game assets.
#include "common/logging.h"
#include "common/settings.h"
#include "video_core/renderer_d3d12/d3d12_descriptor_heap.h"
#include "video_core/renderer_d3d12/d3d12_root_signature.h"
#include "video_core/renderer_d3d12/d3d12_resource_utils.h"
#include "video_core/renderer_d3d12/d3d12_scheduler.h"
#include "video_core/renderer_d3d12/d3d12_shader_compiler.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <stdexcept>
#include <string>
#include <vector>

using namespace D3D12;
static void Check(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
static void DebugClean(ID3D12InfoQueue* queue) {
    Check(queue != nullptr, "debug layer unavailable");
    for (u64 i = 0; i < queue->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size{}; queue->GetMessage(i, nullptr, &size);
        std::vector<u8> bytes(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        queue->GetMessage(i, message, &size);
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
            std::fprintf(stderr, "%s\n", message->pDescription);
            throw std::runtime_error("D3D12 debug message");
        }
    }
}
using Words = std::array<u32, 4>;
static Words Values(u32 row, bool second) {
    return {row * 31 + 3 + second, row * 7 + 5 + second,
            (row ^ 0x1953u) + second, row * 19 + 11 + second};
}
struct UniformGateResult { double ms{}; GuestDescriptorQueue::UniformBindingStats stats; u64 submissions{}; };

// 0: repeated tables, 1: mixed stream, 2: pending-source collisions, 3: stable/dynamic transitions.
static UniformGateResult Run(Device& device, ID3DBlob* ps, ID3DBlob* vs, bool enabled, bool worker,
                  u32 draws, u32 capacity, u32 pattern,
                  const std::function<void(Scheduler&, u32)>& after_draw = {}) {
    SetIncrementalUniformBindings(enabled);
    SetImageTableReuse(true);
    SetAsyncRecording(worker);
    Scheduler scheduler{device};
    DescriptorRing ring{device.Get(), scheduler, capacity};
    GuestDescriptorQueue descriptors{device.Get(), ring};
    CpuDescriptorAllocator offline{device.Get(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV};
    RootSignatureCache roots{device};
    Shader::Info info;
    for (u32 j = 0; j < 8; ++j) info.constant_buffer_descriptors.push_back({.index = j, .count = 1});
    info.image_descriptors.push_back({.count = 1});
    const std::array<const Shader::Info*, 1> stages{&info};
    const auto& layout = roots.Get(stages, false);
    Check(layout.NumResourceDescriptors() == 10 && layout.NumImageDescriptors() == 2,
          "fixture layout changed");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = layout.Handle();
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.SampleMask = ~0u;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.SampleDesc.Count = 1;
    for (auto& target : pd.BlendState.RenderTarget) {
        target.SrcBlend = target.DestBlend = D3D12_BLEND_ONE;
        target.SrcBlendAlpha = target.DestBlendAlpha = D3D12_BLEND_ONE;
        target.BlendOp = target.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    ComPtr<ID3D12PipelineState> pipeline;
    ThrowIfFailed(device.Get()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pipeline)), "test PSO");
    constexpr u32 Rows = 2048;
    auto input = CreateCommittedBuffer(device.Get(), Rows * 512, D3D12_HEAP_TYPE_UPLOAD,
        D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, "uniform test input");
    void* mapped{}; const D3D12_RANGE empty{};
    ThrowIfFailed(input->Map(0, &empty, &mapped), "map uniform input");
    std::memset(mapped, 0, Rows * 512);
    for (u32 row = 0; row < Rows; ++row) {
        const auto a = Values(row, false), b = Values(row, true);
        std::memcpy(static_cast<u8*>(mapped) + row * 512, a.data(), sizeof(a));
        std::memcpy(static_cast<u8*>(mapped) + row * 512 + 256, b.data(), sizeof(b));
    }
    input->Unmap(0, nullptr);
    auto output = CreateCommittedBuffer(device.Get(), u64(draws) * 16, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "uniform test output");
    auto readback = CreateCommittedBuffer(device.Get(), u64(draws) * 16, D3D12_HEAP_TYPE_READBACK,
        D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, "uniform test readback");
    const auto uav = offline.Allocate(), srv = offline.Allocate();
    const D3D12_UNORDERED_ACCESS_VIEW_DESC ud{.Format = DXGI_FORMAT_R32G32B32A32_UINT,
        .ViewDimension = D3D12_UAV_DIMENSION_BUFFER, .Buffer = {.NumElements = draws}};
    const D3D12_SHADER_RESOURCE_VIEW_DESC sd{.Format = DXGI_FORMAT_R32G32B32A32_UINT,
        .ViewDimension = D3D12_SRV_DIMENSION_BUFFER,
        .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING,
        .Buffer = {.NumElements = draws}};
    device.Get()->CreateUnorderedAccessView(output.Get(), nullptr, &ud, uav);
    device.Get()->CreateShaderResourceView(output.Get(), &sd, srv);
    std::vector<Words> expected(draws);
    u64 state_tick = ~0ull;
    const u64 first_tick = scheduler.CurrentTick();
    const auto start = std::chrono::steady_clock::now();
    for (u32 i = 0; i < draws; ++i) {
        if (!after_draw && i && i % 97 == 0) scheduler.Flush();
        descriptors.Acquire(layout.NumResourceDescriptors(), layout.NumImageDescriptors());
        for (u32 j = 0; j < 8; ++j) {
            const bool streamed = pattern == 1 ? j < 3
                : pattern == 3 && (i / 16) % 2 && j == 4;
            const u32 row = pattern == 2 ? (i * 8 + j) % Rows
                : streamed ? (j == 0 ? i % 16 : (i * 3 + j) % Rows)
                : j + (i / 16 % 4) * 8;
            const bool null_view = j == 7 && (i / 29) % 2;
            const u32 size = j == 6 && (i / 23) % 2 ? 256 : 512;
            const u64 address = input->GetGPUVirtualAddress() + row * 512;
            u64 identity = (1ull << 62) | 1;
            if (streamed) identity = ((j == 0 ? 3ull : 2ull) << 62) | (row + 1);
            if (pattern == 2) {
                // Multiplicative hash's low byte is 21; inverse 61 forces all views into
                // one four-way set, with different GPU addresses and retained sources.
                identity = (1ull << 62) | (u64(i * 8 + j + 1) << 8) |
                    ((((address >> 8) ^ (address >> 20) ^ size) * 61) & 255);
            }
            // New owner at the same VA must also miss even when bytes happen to match.
            if (pattern == 0 && i >= draws / 2) identity += 256;
            descriptors.AddConstantBuffer(null_view ? 0 : address, null_view ? 0 : size, identity);
            if (!null_view) {
                const auto v = Values(row, false);
                for (u32 k = 0; k < 4; ++k) expected[i][k] += v[k];
            }
            if (j == 6 && size == 512) {
                const auto v = Values(row, true);
                for (u32 k = 0; k < 4; ++k) expected[i][k] += v[k];
            }
        }
        descriptors.AddImageCopy(uav); descriptors.AddImageCopy(srv);
        const auto table = descriptors.Table(), image_table = descriptors.ImageTable();
        auto* cmd = scheduler.CommandList();
        ID3D12DescriptorHeap* heaps[]{ring.Heap()}; cmd->SetDescriptorHeaps(1, heaps);
        if (state_tick != scheduler.CurrentTick()) {
            cmd->SetGraphicsRootSignature(layout.Handle());
            const D3D12_VIEWPORT viewport{0, 0, 1, 1, 0, 1}; const D3D12_RECT scissor{0, 0, 1, 1};
            cmd->RSSetViewports(1, &viewport); cmd->RSSetScissorRects(1, &scissor);
            cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);
        }
        scheduler.SetPipelineState(pipeline.Get());
        std::array<u32, 32> words{}; words[0] = i;
        cmd->SetGraphicsRoot32BitConstants(layout.PushConstantsIndex(), PUSH_CONSTANT_WORDS, words.data(), 0);
        cmd->SetGraphicsRoot32BitConstants(PipelineLayout::RUNTIME_DATA_INDEX, layout.RuntimeDataWords(), words.data(), 0);
        cmd->SetGraphicsRootDescriptorTable(layout.ResourceTableIndex(), table);
        cmd->SetGraphicsRootDescriptorTable(layout.ImageTableIndex(), image_table);
        cmd->DrawInstanced(3, 1, 0, 0);
        state_tick = scheduler.CurrentTick();
        if (after_draw) after_draw(scheduler, i);
    }
    UniformGateResult result;
    result.ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    result.stats = descriptors.GetUniformBindingStats();
    TransitionResource(scheduler.CommandList(), output.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
    scheduler.CommandList()->CopyBufferRegion(readback.Get(), 0, output.Get(), 0, u64(draws) * 16);
    scheduler.Finish();
    result.submissions = scheduler.CurrentTick() - first_tick;
    const D3D12_RANGE read_range{0, SIZE_T(draws) * 16};
    ThrowIfFailed(readback->Map(0, &read_range, &mapped), "read uniform results");
    Check(std::memcmp(mapped, expected.data(), u64(draws) * 16) == 0,
          "stale descriptor, wrong null/bounds, mixed copy destination or table lifetime");
    readback->Unmap(0, &empty);
    if (enabled) {
        if (pattern == 0) Check(result.stats.table_hits > draws / 3, "whole-table reuse not exercised");
        if (pattern == 1) Check(result.stats.hits == 0 && result.stats.creates == draws * 8,
            "mixed upload stream did not retain direct view creation");
        if (pattern == 2) Check(result.stats.evictions > draws / 8, "pending-source eviction not exercised");
        if (pattern == 3) Check(result.stats.table_hits > draws / 8 &&
            result.stats.creates > draws / 4 && result.stats.hits > draws * 2,
            "stable/dynamic fallback and rearming were not exercised");
    } else Check(result.stats.creates == u64(draws) * 8, "control skipped CBV creation");
    return result;
}

int main(int argc, char**) try {
    Common::Log::Initialize();
    const bool worker = argc > 1;
    const std::string shader =
        "cbuffer A0:register(b0){uint4 a0[32];}; cbuffer A1:register(b1){uint4 a1[32];};"
        "cbuffer A2:register(b2){uint4 a2[32];}; cbuffer A3:register(b3){uint4 a3[32];};"
        "cbuffer A4:register(b4){uint4 a4[32];}; cbuffer A5:register(b5){uint4 a5[32];};"
        "cbuffer A6:register(b6){uint4 a6[32];}; cbuffer A7:register(b7){uint4 a7[32];};"
        "cbuffer Push:register(b0,space" + std::to_string(PUSH_CONSTANT_SPACE) + "){uint index;};"
        "RWBuffer<uint4> result:register(u8); void main(float4 p:SV_Position){"
        "result[index]=a0[0]+a1[0]+a2[0]+a3[0]+a4[0]+a5[0]+a6[0]+a7[0]+a6[16];}";
    const char vertex[] = "float4 main(uint i:SV_VertexID):SV_Position{"
        "return float4(i==1?3:-1,i==2?3:-1,0,1);}";
    ComPtr<ID3DBlob> ps, vs, errors;
    const auto hr = D3DCompile(shader.data(), shader.size(), nullptr, nullptr, nullptr, "main",
        "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &ps, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "%s", static_cast<const char*>(errors->GetBufferPointer()));
    ThrowIfFailed(hr, "test pixel shader");
    ThrowIfFailed(D3DCompile(vertex, sizeof(vertex), nullptr, nullptr, nullptr, "main", "vs_5_1",
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vs, &errors), "test vertex shader");
    Settings::values.renderer_debug = true;
    {
        Device device;
        ComPtr<ID3D12InfoQueue> debug; device.Get()->QueryInterface(IID_PPV_ARGS(&debug));
        if (debug) debug->ClearStoredMessages();
        for (u32 pattern = 0; pattern < 4; ++pattern) {
            for (bool enabled : {false, true}) {
                const auto r = Run(device, ps.Get(), vs.Get(), enabled, worker, 512, 64, pattern);
                std::printf("pattern=%u enabled=%u creates=%llu hits=%llu tables=%llu evictions=%llu compared=8192 bytes\n",
                    pattern, enabled, r.stats.creates, r.stats.hits, r.stats.table_hits, r.stats.evictions);
            }
        }
        DebugClean(debug.Get());
    }
    Settings::values.renderer_debug = false;
    Device device;
    for (u32 pattern = 0; pattern < 2; ++pattern) {
        std::vector<double> times[2];
        for (u32 repeat = 0; repeat < 7; ++repeat) for (u32 phase = 0; phase < 2; ++phase) {
            const bool enabled = (repeat + phase) % 2;
            const auto r = Run(device, ps.Get(), vs.Get(), enabled, worker, 4096, 65536, pattern);
            times[enabled].push_back(r.ms);
        }
        for (auto& t : times) std::sort(t.begin(), t.end());
        std::printf("BENCH pattern=%u draws=4096 samples=7 control_ms=%.6f candidate_ms=%.6f\n",
                    pattern, times[0][3], times[1][3]);
    }
    std::puts("PASS: incremental uniform descriptors, exact GPU bytes and D3D12 debug clean");
    return 0;
} catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
