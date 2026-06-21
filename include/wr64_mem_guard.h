#ifndef WR64_MEM_GUARD_H
#define WR64_MEM_GUARD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void wr64_trap_bad_vaddr(uint32_t vaddr, const char* op, uint64_t reg, int64_t offset);

#ifdef __cplusplus
}
#endif

// Must match recomp::mem_size in librecomp/addresses.hpp.
enum { WR64_RDRAM_BYTES = 1024 * 1024 * 1024 };

static inline uint64_t wr64_mem_normalize_reg(uint64_t reg) {
    const uint32_t u = (uint32_t)reg;
    // Promote stripped RDRAM physical offsets (not small integers / NULL).
    if (u < 0x80000000u && u >= 0x10000u && u < 0x00800000u) {
        return (uint64_t)(int32_t)(u | 0x80000000u);
    }
    return reg;
}

static inline uint32_t wr64_mem_vaddr(uint64_t reg, int64_t offset) {
    reg = wr64_mem_normalize_reg(reg);
    return (uint32_t)(int32_t)((int64_t)reg + offset);
}

static inline uint64_t wr64_mem_host_offset(uint32_t vaddr) {
    if (vaddr >= 0xC0000000u) {
        return (uint64_t)(-1);
    }
    if (vaddr >= 0x80000000u) {
        return (uint64_t)(vaddr - 0x80000000u);
    }
    return (uint64_t)(vaddr & 0x00FFFFFFu);
}

static inline uint64_t wr64_mem_rdram_offset(uint32_t vaddr) {
    return wr64_mem_host_offset(vaddr);
}

static inline void wr64_mem_check_vaddr(uint32_t vaddr, const char* op, uint64_t reg, int64_t offset) {
    if (vaddr < 0x80000000u) {
        wr64_trap_bad_vaddr(vaddr, op, reg, offset);
    }
    const uint64_t rdram_off = wr64_mem_host_offset(vaddr);
    if (rdram_off >= WR64_RDRAM_BYTES) {
        wr64_trap_bad_vaddr(vaddr, op, reg, offset);
    }
}

static inline int32_t* wr64_mem_w_ptr(uint8_t* rdram, uint64_t reg, int64_t offset) {
    const uint32_t vaddr = wr64_mem_vaddr(reg, offset);
    wr64_mem_check_vaddr(vaddr, "MEM_W", reg, offset);
    return (int32_t*)(rdram + wr64_mem_rdram_offset(vaddr));
}

static inline uint32_t* wr64_mem_wu_ptr(uint8_t* rdram, uint64_t reg, int64_t offset) {
    const uint32_t vaddr = wr64_mem_vaddr(reg, offset);
    wr64_mem_check_vaddr(vaddr, "MEM_WU", reg, offset);
    return (uint32_t*)(rdram + wr64_mem_rdram_offset(vaddr));
}

static inline int16_t* wr64_mem_h_ptr(uint8_t* rdram, uint64_t reg, int64_t offset) {
    const uint32_t vaddr = wr64_mem_vaddr(reg, offset) ^ 2u;
    wr64_mem_check_vaddr(vaddr, "MEM_H", reg, offset);
    return (int16_t*)(rdram + wr64_mem_rdram_offset(vaddr));
}

static inline int8_t* wr64_mem_b_ptr(uint8_t* rdram, uint64_t reg, int64_t offset) {
    const uint32_t vaddr = wr64_mem_vaddr(reg, offset) ^ 3u;
    wr64_mem_check_vaddr(vaddr, "MEM_B", reg, offset);
    return (int8_t*)(rdram + wr64_mem_rdram_offset(vaddr));
}

static inline uint16_t* wr64_mem_hu_ptr(uint8_t* rdram, uint64_t reg, int64_t offset) {
    const uint32_t vaddr = wr64_mem_vaddr(reg, offset) ^ 2u;
    wr64_mem_check_vaddr(vaddr, "MEM_HU", reg, offset);
    return (uint16_t*)(rdram + wr64_mem_rdram_offset(vaddr));
}

static inline uint8_t* wr64_mem_bu_ptr(uint8_t* rdram, uint64_t reg, int64_t offset) {
    const uint32_t vaddr = wr64_mem_vaddr(reg, offset) ^ 3u;
    wr64_mem_check_vaddr(vaddr, "MEM_BU", reg, offset);
    return (uint8_t*)(rdram + wr64_mem_rdram_offset(vaddr));
}

#define WR64_MEM_W(rdram_ptr, reg, offset) (*wr64_mem_w_ptr((rdram_ptr), (reg), (offset)))
#define WR64_MEM_WU(rdram_ptr, reg, offset) (*wr64_mem_wu_ptr((rdram_ptr), (reg), (offset)))
#define WR64_MEM_H(rdram_ptr, reg, offset) (*wr64_mem_h_ptr((rdram_ptr), (reg), (offset)))
#define WR64_MEM_B(rdram_ptr, reg, offset) (*wr64_mem_b_ptr((rdram_ptr), (reg), (offset)))
#define WR64_MEM_HU(rdram_ptr, reg, offset) (*wr64_mem_hu_ptr((rdram_ptr), (reg), (offset)))
#define WR64_MEM_BU(rdram_ptr, reg, offset) (*wr64_mem_bu_ptr((rdram_ptr), (reg), (offset)))

#endif
