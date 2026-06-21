// Wave Race 64 — Project64-derived RSP audio HLE integration (GPLv2).

#include "hle.h"
#include "librecomp/rsp.hpp"
#include "pj64/alist.h"
#include "pj64/mem.h"
#include "pj64/ucodes.h"

#include <atomic>
#include <cstdio>
#include <cstring>

extern RspUcodeFunc aspMain;

namespace ultramodern {
void boot_log(const char* fmt, ...);
}

namespace {

constexpr uint32_t kAbi1Marker = 0xf0000f00u;
constexpr uint32_t kWr64UsRevAAudioAbi = 0x1e24138cu; // aspMain ucode_data + 0x28

static bool wr64_try_hle_audio(CHle* hle) {
    const uint32_t ucode_data = *dmem_u32(hle, TASK_UCODE_DATA);
    if (*dram_u32(hle, ucode_data) != 0x00000001u) {
        return false;
    }

    uint32_t fingerprint = 0;
    if (*dram_u32(hle, ucode_data + 0x30) == kAbi1Marker) {
        fingerprint = *dram_u32(hle, ucode_data + 0x28);
    } else {
        fingerprint = *dram_u32(hle, ucode_data + 0x10);
    }

    switch (fingerprint) {
    case kWr64UsRevAAudioAbi:
        alist_process_audio(hle);
        return true;
    default:
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            ultramodern::boot_log(
                "[wr64] audio HLE: unknown aspMain fingerprint 0x%08X (ucode_data=0x%08X)\n",
                fingerprint,
                ucode_data);
        }
        return false;
    }
}

} // namespace

CHle::CHle(uint8_t* rdram) : dram_base_(rdram) {
    std::memset(m_alist_buffer, 0, sizeof(m_alist_buffer));
    std::memset(&m_alist_audio, 0, sizeof(m_alist_audio));
    std::memset(&m_alist_nead, 0, sizeof(m_alist_nead));
}

uint8_t* CHle::dmem() {
    return ::dmem;
}

void CHle::WarnMessage(const char* message, ...) {
    char buffer[512];
    va_list args;
    va_start(args, message);
    std::vsnprintf(buffer, sizeof(buffer), message, args);
    va_end(args);
    ultramodern::boot_log("[wr64] audio HLE warn: %s\n", buffer);
}

RspExitReason wr64_aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    CHle hle(rdram);
    if (wr64_try_hle_audio(&hle)) {
        static std::atomic<uint32_t> hle_count{0};
        const uint32_t n = hle_count.fetch_add(1) + 1;
        if (n <= 16 || (n % 120) == 0) {
            ultramodern::boot_log("[wr64] audio HLE task #%u OK\n", n);
        }
        return RspExitReason::Broke;
    }

    static std::atomic<bool> fallback_logged{false};
    if (!fallback_logged.exchange(true)) {
        ultramodern::boot_log("[wr64] audio HLE miss — falling back to recompiled aspMain\n");
    }
    return aspMain(rdram, ucode_addr);
}
