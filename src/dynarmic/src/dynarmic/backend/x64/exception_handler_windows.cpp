// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* This file is part of the dynarmic project.
 * Copyright (c) 2016 MerryMage
 * SPDX-License-Identifier: 0BSD
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>
#include <vector>

#include "common/assert.h"
#include <bit>
#include "common/common_types.h"

#include "dynarmic/backend/exception_handler.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/common/safe_ops.h"

using UBYTE = u8;

enum UNWIND_REGISTER_CODES {
    UWRC_RAX,
    UWRC_RCX,
    UWRC_RDX,
    UWRC_RBX,
    UWRC_RSP,
    UWRC_RBP,
    UWRC_RSI,
    UWRC_RDI,
    UWRC_R8,
    UWRC_R9,
    UWRC_R10,
    UWRC_R11,
    UWRC_R12,
    UWRC_R13,
    UWRC_R14,
    UWRC_R15,
};

enum UNWIND_OPCODE {
    UWOP_PUSH_NONVOL = 0,
    UWOP_ALLOC_LARGE = 1,
    UWOP_ALLOC_SMALL = 2,
    UWOP_SET_FPREG = 3,
    UWOP_SAVE_NONVOL = 4,
    UWOP_SAVE_NONVOL_FAR = 5,
    UWOP_SAVE_XMM128 = 8,
    UWOP_SAVE_XMM128_FAR = 9,
    UWOP_PUSH_MACHFRAME = 10,
};

union UNWIND_CODE {
    struct {
        UBYTE CodeOffset;
        UBYTE UnwindOp : 4;
        UBYTE OpInfo : 4;
    } code;
    USHORT FrameOffset;
};

// UNWIND_INFO is a tail-padded structure
struct UNWIND_INFO {
    UBYTE Version : 3;
    UBYTE Flags : 5;
    UBYTE SizeOfProlog;
    UBYTE CountOfCodes;
    UBYTE FrameRegister : 4;
    UBYTE FrameOffset : 4;
    // UNWIND_CODE UnwindCode[];
    // With Flags == 0 there are no additional fields.
    // OPTIONAL UNW_EXCEPTION_INFO ExceptionInfo;
};

struct UNW_EXCEPTION_INFO {
    ULONG ExceptionHandler;
    // OPTIONAL ARBITRARY HandlerData;
};

static_assert(sizeof(UNWIND_INFO) == 4 && sizeof(UNWIND_CODE) == 2);

namespace Dynarmic::Backend {

using namespace Dynarmic::Backend::X64;

static UNWIND_INFO* WriteUnwindInfo(BlockOfCode& code, const UnwindRegion& region,
                                   bool body, std::vector<UNW_EXCEPTION_INFO*>* handlers,
                                   ULONG handler_offset) {
    std::vector<UNWIND_CODE> entries;
    bool frame_register = false;
    const auto operation = [&](u8 offset, u8 opcode, u8 info) {
        UNWIND_CODE entry{};
        entry.code.CodeOffset = offset;
        entry.code.UnwindOp = opcode;
        entry.code.OpInfo = info;
        entries.push_back(entry);
    };
    const auto operand = [&](u32 value) {
        ASSERT(value <= 0xffff);
        UNWIND_CODE entry{};
        entry.FrameOffset = static_cast<USHORT>(value);
        entries.push_back(entry);
    };
    for (auto iter = region.operations.rbegin(); iter != region.operations.rend(); ++iter) {
        ASSERT(iter->code_offset <= 255);
        const u8 offset = body ? 0 : static_cast<u8>(iter->code_offset);
        switch (iter->kind) {
        case UnwindOperation::Kind::Push:
            operation(offset, UWOP_PUSH_NONVOL, static_cast<u8>(iter->register_index));
            break;
        case UnwindOperation::Kind::Allocate:
            ASSERT(iter->stack_offset != 0 && iter->stack_offset % 8 == 0);
            if (iter->stack_offset <= 128) {
                operation(offset, UWOP_ALLOC_SMALL, static_cast<u8>(iter->stack_offset / 8 - 1));
            } else {
                operation(offset, UWOP_ALLOC_LARGE, 0);
                operand(iter->stack_offset / 8);
            }
            break;
        case UnwindOperation::Kind::SaveXmm:
            ASSERT(iter->stack_offset % 16 == 0);
            operation(offset, UWOP_SAVE_XMM128, static_cast<u8>(iter->register_index));
            operand(iter->stack_offset / 16);
            break;
        case UnwindOperation::Kind::SetFrame:
            ASSERT(iter->register_index == UWRC_RBP);
            operation(offset, UWOP_SET_FPREG, 0);
            frame_register = true;
            break;
        }
    }
    ASSERT(entries.size() <= 255 && region.end - region.begin <= 255);
    const u8 count = static_cast<u8>(entries.size());
    if (entries.size() % 2) {
        entries.emplace_back();
    }
    code.align(4);
    const size_t size = sizeof(UNWIND_INFO) + entries.size() * sizeof(UNWIND_CODE);
    auto* info = static_cast<UNWIND_INFO*>(code.AllocateFromCodeSpace(
        size + (handlers ? sizeof(UNW_EXCEPTION_INFO) : 0)));
    *info = {};
    info->Version = 1;
    info->Flags = handlers ? UNW_FLAG_EHANDLER : 0;
    info->SizeOfProlog = body ? 0 : static_cast<u8>(region.end - region.begin);
    info->CountOfCodes = count;
    info->FrameRegister = frame_register ? UWRC_RBP : 0;
    if (!entries.empty()) {
        std::memcpy(info + 1, entries.data(), entries.size() * sizeof(UNWIND_CODE));
    }
    if (handlers) {
        auto* except = reinterpret_cast<UNW_EXCEPTION_INFO*>(reinterpret_cast<u8*>(info) + size);
        except->ExceptionHandler = handler_offset;
        handlers->push_back(except);
    }
    return info;
}

struct ExceptionHandler::Impl final {
    Impl(BlockOfCode& code) {
        code.align(16);
        const u8* exception_handler_without_cb = code.getCurr<u8*>();
        code.mov(code.eax, u32(ExceptionContinueSearch));
        code.ret();

        code.align(16);
        const u8* exception_handler_with_cb = code.getCurr<u8*>();
        UnwindRegion handler_frame;
        handler_frame.begin = static_cast<u32>(exception_handler_with_cb - code.getCode());
        // Our 3rd argument is a PCONTEXT.

        // If not within our codeblock, ignore this exception.
        code.mov(code.rax, Safe::Negate(std::bit_cast<u64>(code.getCode())));
        code.add(code.rax, code.qword[code.ABI_PARAM3 + Xbyak::RegExp(offsetof(CONTEXT, Rip))]);
        code.cmp(code.rax, static_cast<u32>(code.GetTotalCodeSize()));
        code.ja(exception_handler_without_cb);

        code.sub(code.rsp, 40);
        handler_frame.end = static_cast<u32>(code.getCurr() - code.getCode());
        handler_frame.operations.push_back({UnwindOperation::Kind::Allocate,
            handler_frame.end - handler_frame.begin, 0, 40});
        code.mov(code.ABI_PARAM1, std::bit_cast<u64>(&cb));
        code.mov(code.ABI_PARAM2, code.ABI_PARAM3);
        code.CallLambda([](const std::function<FakeCall(u64)>& cb_, PCONTEXT ctx) {
            FakeCall fc = cb_(ctx->Rip);
            ctx->Rsp -= sizeof(u64);
            *std::bit_cast<u64*>(ctx->Rsp) = fc.ret_rip;
            ctx->Rip = fc.call_rip;
        });
        code.mov(code.eax, u32(ExceptionContinueExecution));
        code.add(code.rsp, 40);
        code.ret();
        const auto handler_end = static_cast<u32>(code.getCurr() - code.getCode());

        exception_handler_without_cb_offset = ULONG(exception_handler_without_cb - code.getCode<u8*>());
        exception_handler_with_cb_offset = ULONG(exception_handler_with_cb - code.getCode<u8*>());

        const auto& roots = code.GetUnwindRegions();
        ASSERT(roots.size() == 2);
        const auto relative = [&](const void* pointer) {
            return static_cast<DWORD>(static_cast<const u8*>(pointer) - code.getCode());
        };
        const DWORD body_info = relative(WriteUnwindInfo(code, roots.front(), true,
            &except_infos, exception_handler_without_cb_offset));
        std::vector<RUNTIME_FUNCTION> functions;
        const auto append = [&](u32 begin, u32 end, DWORD info) {
            if (begin < end) {
                functions.push_back({begin, end, info});
            }
        };
        u32 cursor = roots.front().begin;
        for (const auto& root : roots) {
            append(cursor, root.begin, body_info);
            const auto info = relative(WriteUnwindInfo(code, root, false,
                &except_infos, exception_handler_without_cb_offset));
            append(root.begin, root.end, info);
            cursor = root.end;
        }
        append(cursor, exception_handler_without_cb_offset, body_info);
        const UnwindRegion leaf;
        append(exception_handler_without_cb_offset, exception_handler_with_cb_offset,
               relative(WriteUnwindInfo(code, leaf, false, nullptr, 0)));
        append(exception_handler_with_cb_offset, handler_end,
               relative(WriteUnwindInfo(code, handler_frame, false, nullptr, 0)));
        append(handler_end, static_cast<u32>(code.GetTotalCodeSize()), body_info);
        code.align(4);
        rfuncs = static_cast<RUNTIME_FUNCTION*>(code.AllocateFromCodeSpace(
            functions.size() * sizeof(RUNTIME_FUNCTION)));
        std::memcpy(rfuncs, functions.data(), functions.size() * sizeof(RUNTIME_FUNCTION));
        const auto registered = RtlAddFunctionTable(rfuncs, static_cast<DWORD>(functions.size()),
                                                   reinterpret_cast<DWORD64>(code.getCode()));
        ASSERT(registered);
    }

    void SetCallback(std::function<FakeCall(u64)> new_cb) {
        cb = new_cb;
        for (auto* info : except_infos) {
            info->ExceptionHandler = cb ? exception_handler_with_cb_offset : exception_handler_without_cb_offset;
        }
    }

    ~Impl() {
        RtlDeleteFunctionTable(rfuncs);
    }

private:
    RUNTIME_FUNCTION* rfuncs;
    std::function<FakeCall(u64)> cb;
    std::vector<UNW_EXCEPTION_INFO*> except_infos;
    ULONG exception_handler_without_cb_offset;
    ULONG exception_handler_with_cb_offset;
};

ExceptionHandler::ExceptionHandler() = default;
ExceptionHandler::~ExceptionHandler() = default;

void ExceptionHandler::Register(BlockOfCode& code) {
    impl = std::make_unique<Impl>(code);
}

bool ExceptionHandler::SupportsFastmem() const noexcept {
    return static_cast<bool>(impl);
}

void ExceptionHandler::SetFastmemCallback(std::function<FakeCall(u64)> cb) {
    impl->SetCallback(cb);
}

}  // namespace Dynarmic::Backend
