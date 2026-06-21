#ifndef WR64_MEMORY_H
#define WR64_MEMORY_H

#include <stdint.h>

/* Wave Race 64 memory map (from leak-ref/kn_memory.h, Nintendo tree). */

#define WR64_STATIC_SEGMENT 1
#define WR64_AUTO_SEGMENT 2
#define WR64_DYNAMIC_SEGMENT 3
#define WR64_SCREEN_SEGMENT 4
#define WR64_ENDYNAMIC_SEGMENT 5
#define WR64_SOT_DYNAMIC_SEGMENT 6
#define WR64_KN_DYNAMIC_SEGMENT 7
#define WR64_BANK_SEGMENT 8
#define WR64_TEX_SEGMENT 9
#define WR64_TEX2_SEGMENT 10
#define WR64_TEX3_SEGMENT 11
#define WR64_TEX4_SEGMENT 12
#define WR64_COURSE_SEGMENT 13
#define WR64_COURSE_COM_SEGMENT 14

#define WR64_AUDIO_HEAP 0x80001000u
#define WR64_LOAD_ATABLE_ADDR 0x80045800u
#define WR64_DRAM_STACK 0x80045800u
#define WR64_BOOT_STACKSIZE 0x200u
#define WR64_BOOT_STACK_TOP (WR64_DRAM_STACK + WR64_BOOT_STACKSIZE)
#define WR64_BOOT_ADDR 0x80046800u
#define WR64_BOOT_ENTRYPOINT 0x80047AA4u

#define WR64_START_GATE_ADDR 0x8029A200u
#define WR64_Z_BUFFER 0x802A0000u
#define WR64_PROG_BANK 0x802C5800u
#define WR64_COURSE_BUFFER 0x802D6800u
#define WR64_COURSE_DT 0x80306800u
#define WR64_BANK_BUFFER 0x80316800u
#define WR64_FRAME_BUFFER 0x8038F800u
#define WR64_FRAME_BUFFER_H0 0x802D4000u
#define WR64_FRAME_BUFFER_H1 0x8036A000u
#define WR64_EDIT_BANK 0x80328000u
#define WR64_CAMERA_BUFFER 0x80580000u

/* Host byte offset for MIPS virtual addresses in the 1 GiB RDRAM sandbox. */
static inline uint64_t wr64_rdram_host_offset(uint32_t vaddr) {
    if (vaddr >= 0xC0000000u) {
        return UINT64_MAX;
    }
    if (vaddr >= 0x80000000u) {
        return static_cast<uint64_t>(vaddr - 0x80000000u);
    }
    return static_cast<uint64_t>(vaddr & 0x00FFFFFFu);
}

static inline uint32_t wr64_kseg0_to_rdram_offset(uint32_t vaddr) {
    return static_cast<uint32_t>(wr64_rdram_host_offset(vaddr));
}

#ifdef __cplusplus
extern "C" void wr64_trap_bad_vaddr(uint32_t vaddr, const char* op, uint64_t reg, int64_t offset);

static inline uint8_t* wr64_rdram_byte_ptr(uint8_t* rdram, uint32_t vaddr) {
    const uint64_t off = wr64_rdram_host_offset(vaddr);
    if (off >= (1024ULL * 1024ULL * 1024ULL)) {
        wr64_trap_bad_vaddr(vaddr, "host_ptr", vaddr, 0);
    }
    return rdram + off;
}
#endif

#endif
