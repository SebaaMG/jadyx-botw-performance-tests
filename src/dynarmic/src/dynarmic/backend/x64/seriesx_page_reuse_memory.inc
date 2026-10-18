// SPDX-FileCopyrightText: Copyright 2026 BlackBearReloaded
// SPDX-FileCopyrightText: Copyright 2026 SeriesX private project contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Adapted from ProsperoEden a26ba788, page-reuse.cmake. Included in the A64 emitter only.

// ProsperoEden: the full page-table lookup of a grouped access whose group lookup gave 0
// (headless/dynarmic/page-reuse.cmake), for code inside a deferred block: the direct table, its
// address space range-checked, misalignment detected only on page boundaries.
template<typename Config>
size_t SeriesXReuseAddressBits(const Config& config) {
    if constexpr (requires { config.page_table_address_space_bits; }) {
        return config.page_table_address_space_bits;
    } else {
        return 32;  // A32: never grouped
    }
}

inline void SeriesXReuseSlowLookup(BlockOfCode& code, size_t bitsize, bool detect, size_t valid_page_index_bits,
        Xbyak::Label& abort, Xbyak::Reg64 vaddr, Xbyak::Reg64 page) {
    if (bitsize != 8 && detect) {
        const u32 align_mask = static_cast<u32>(bitsize / 8 - 1);
        const u32 page_align_mask = static_cast<u32>(page_table_const_size - 1) & ~align_mask;
        Xbyak::Label aligned;
        code.test(vaddr, align_mask);
        code.jz(aligned);
        code.mov(page, vaddr);
        code.and_(page, page_align_mask);
        code.cmp(page, page_align_mask);
        code.je(abort, code.T_NEAR);
        code.L(aligned);
    }
    code.mov(page, vaddr);
    code.shr(page, int(page_table_const_bits));
    code.test(page, u32(-(1 << valid_page_index_bits)));
    code.jnz(abort, code.T_NEAR);
    code.mov(page, code.qword[r14 + page * 8]);
    code.test(page, page);
    code.jz(abort, code.T_NEAR);
}
