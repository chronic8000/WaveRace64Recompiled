// Standalone CHle adapter for Project64 RSP audio HLE (GPLv2).
// Original: Project64-rsp-core/Hle/hle.h — stripped of emulator dependencies.

#pragma once

#include "pj64/ucodes.h"
#include <cstdarg>
#include <cstdint>
#include <cstring>

#ifdef __GNUC__
#define UNUSED(x) UNUSED_##x __attribute__((__unused__))
#else
#define UNUSED(x) /* x */
#endif

class CHle {
public:
    explicit CHle(uint8_t* rdram);

    uint8_t* dram() { return dram_base_; }
    uint8_t* dmem();
    uint8_t* imem() { return dmem(); }

    struct alist_audio_t& alist_audio() { return m_alist_audio; }
    struct alist_nead_t& alist_nead() { return m_alist_nead; }
    uint8_t* alist_buffer() { return m_alist_buffer; }

    void WarnMessage(const char* message, ...);

private:
    uint8_t* dram_base_;
    uint8_t m_alist_buffer[0x1000]{};
    struct alist_audio_t m_alist_audio{};
    struct alist_nead_t m_alist_nead{};
};
