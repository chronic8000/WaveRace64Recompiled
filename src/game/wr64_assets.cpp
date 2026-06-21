#include "wr64_memory.h"
#include "wr64_mem_guard.h"

#include "librecomp/addresses.hpp"
#include "librecomp/game.hpp"
#include "librecomp/mods.hpp"
#include "librecomp/overlays.hpp"
#include "recomp.h"
#include "recomp_ui.h"
#include "wr64_boot_config.hpp"

#include <ultramodern/ultramodern.hpp>
#include <ultramodern/ultra64.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <vector>

uint8_t* wr64_last_rdram = nullptr;

extern std::atomic_bool exited;

extern "C" void wr64_trap_bad_vaddr(uint32_t vaddr, const char* op, uint64_t reg, int64_t offset) {
    std::fprintf(stderr,
        "[wr64] invalid %s vaddr=0x%08X reg=0x%016llX offset=%lld "
        "(bad pointer — uninitialized overlay data or corrupt struct field)\n",
        op ? op : "MEM",
        vaddr,
        static_cast<unsigned long long>(reg),
        static_cast<long long>(offset));
    if (vaddr < 0x100u && reg < 0x10000u) {
        std::fprintf(stderr,
            "[wr64]   hint: reg=0x%llX looks like a small integer/garbage used as KSEG0 pointer "
            "(check osPiStartDma HLE + overlay jump tables @ 0x802260BC)\n",
            static_cast<unsigned long long>(reg));
    }
    if (vaddr < 0x80000000u) {
        std::fprintf(stderr,
            "[wr64]   hint: vaddr is in KUSEG (0x00000000–0x7FFFFFFF), not valid KSEG0 RDRAM\n");
    } else {
        const uint64_t rdram_off = wr64_rdram_host_offset(vaddr);
        std::fprintf(stderr,
            "[wr64]   hint: host rdram offset 0x%llX exceeds 1 GiB sandbox\n",
            static_cast<unsigned long long>(rdram_off));
    }
#ifdef _WIN32
    void* trace[16];
    const USHORT frames = CaptureStackBackTrace(1, 16, trace, nullptr);
    for (USHORT i = 0; i < frames; ++i) {
        std::fprintf(stderr, "[wr64]   backtrace[%u]: %p\n", i, trace[i]);
    }
#endif
    if (wr64_last_rdram != nullptr) {
        auto rd32 = [&](uint32_t addr) -> uint32_t {
            return *reinterpret_cast<uint32_t*>(wr64_last_rdram + wr64_kseg0_to_rdram_offset(addr));
        };
        std::fprintf(stderr,
            "[wr64]   globals: gGameState=0x%08X D_801CE638=0x%08X gGameModes=0x%08X\n",
            rd32(0x800DAB24u),
            rd32(0x801CE638u),
            rd32(0x801CE620u));
        std::fprintf(stderr,
            "[wr64]   DL ptr table[0] @0x802260BC=0x%08X (ROM expect 0x80226210) "
            "D_801AE948=0x%08X render_gate@0x800E8170=0x%08X\n",
            rd32(0x802260BCu),
            rd32(0x801AE948u),
            rd32(0x800E8170u));
        for (int i = 0; i < 8; ++i) {
            std::fprintf(stderr,
                "[wr64]   DL ptr table[%d]=0x%08X\n",
                i,
                rd32(0x802260BCu + static_cast<uint32_t>(i) * 4u));
        }
        std::fprintf(stderr,
            "[wr64]   overlay idx @0x802261C4=0x%08X counter@0x802261E0=0x%08X\n",
            rd32(0x802261C4u),
            rd32(0x802261E0u));
    }
    std::abort();
}

namespace wr64 {

extern "C" void codeSEG_800C5A00(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C5C60(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C5DF0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C5E60(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C59B0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6AD0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6B40(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CAAF0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6420(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6570(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C57A0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CB7D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6DE0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6D00(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6770(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_bridge_osRecvMesg(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_bridge_osSendMesg(uint8_t* rdram, recomp_context* ctx);
extern "C" void osViSetMode_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osViBlack_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osViGetCurrentFramebuffer_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osViSetSpecialFeatures_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C63B0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CBF50(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CB9C0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CAB50(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osPiRawStartDma(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osPiStartDma(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80097EC8(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80047C38(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80047C38(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CA370(uint8_t* rdram, recomp_context* ctx);
extern "C" void osCreateMesgQueue(uint8_t* rdram, PTR(OSMesgQueue) mq_, PTR(OSMesg) msg, s32 count);
extern "C" void codeSEG_800C6310(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C615C(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C62BC(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C6340(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800B8CB0(uint8_t* rdram, recomp_context* ctx);
extern "C" void osSetEventMesg_recomp(uint8_t* rdram, recomp_context* ctx);

extern "C" void wr64_osSetEventMesg(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t event_id = static_cast<uint32_t>(ctx->r4);
    const uint32_t mq = static_cast<uint32_t>(ctx->r5);
    const uint32_t msg = static_cast<uint32_t>(ctx->r6);
    osSetEventMesg_recomp(rdram, ctx);
    if (event_id == OS_EVENT_SP || event_id == OS_EVENT_DP) {
        static std::atomic<uint32_t> log_count{0};
        const uint32_t n = log_count.fetch_add(1) + 1;
        if (n <= 8) {
            ultramodern::boot_log(
                "[wr64] osSetEventMesg event=%u mq=0x%08X msg=0x%08X\n",
                event_id,
                mq,
                msg);
        }
    }
}
extern "C" void osContInit_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void osContGetReadData_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_guOrthoF(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_guOrtho(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C7020(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C5AC4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C7380(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C74D4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80047B00(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80047B20(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800BA100(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C4C40(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80047B00(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80047B20(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800BF370(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800C2FDC(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800C3034(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800C3044(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80091F50(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_8004A2B4(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_dispatch_802C5800(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800926F4(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_jt_80092654(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80092CF0(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80046D2C(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800474E4(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800468E0(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_8004A130(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80048854(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80047EE0(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80046BF4(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80046C30(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80046CF8(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80097E68(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80095A28(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80095050(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_80098208(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800922E4(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800922E4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_8006A264(uint8_t* rdram, recomp_context* ctx);

namespace {

void ensure_libultra_virt_count(uint8_t* rdram);
void init_libultra_timer_globals(uint8_t* rdram, bool full_boot_init);
void init_libultra_vi_state(uint8_t* rdram);
void ensure_libultra_timer_list(uint8_t* rdram);
void init_libultra_timer_root(uint8_t* rdram);
void ensure_pi_manager_globals(uint8_t* rdram);
void wr64_game_create_mesg_queue(uint8_t* rdram, uint32_t mq, uint32_t msg_buf, int32_t count);
void wr64_ensure_audio_mesg_queue_pointers(uint8_t* rdram);
void wr64_nudge_audio_vi_queue(uint8_t* rdram);
void wr64_try_audio_load_init(uint8_t* rdram);

extern "C" void wr64_pi_manager_dma_handler(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800C5DF0(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osCreateViManager(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800C6DE0(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800C6D00(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800CB7D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_codeSEG_800CBF50(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C5720(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80091F50(uint8_t* rdram, recomp_context* ctx);
extern "C" void segment_1B1FB0_802C7510(uint8_t* rdram, recomp_context* ctx);
extern "C" void segment_1B1FB0_802C5BA4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_8004A2B4(uint8_t* rdram, recomp_context* ctx);
extern "C" void static_0_80092928(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800926F4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80092938(uint8_t* rdram, recomp_context* ctx);
extern "C" void ovl_i3_802C5800(uint8_t* rdram, recomp_context* ctx);
extern "C" void segment_1B1FB0_802C5800(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80092CF0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80046D2C(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800468E0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_8004A130(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80048854(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80047EE0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80046BF4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80046C30(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80046CF8(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80097E68(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80097F74(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800B4D30(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80095A28(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80095050(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80098208(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800474E4(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_80097EC8(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800CA2C0(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800BF370(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C2FDC(uint8_t* rdram, recomp_context* ctx);
extern "C" void codeSEG_800C3034(uint8_t* rdram, recomp_context* ctx);

static void wr64_invoke_stock_gfx_bootstrap(uint8_t* rdram);

std::recursive_mutex g_timer_path_mutex;
std::atomic<bool> g_vi_init_complete{false};
std::atomic<bool> g_vi_manager_started{false};
std::atomic<uint32_t> g_presents_after_vi_init{0};
std::atomic<bool> g_allow_vi_pulse{false};
std::atomic<bool> g_main_thread_mq_ready{false};
std::atomic<bool> g_entrypoint_complete{false};

static bool wr64_host_hle_should_stop() {
    return ::exited.load(std::memory_order_acquire);
}

// Bootstrap-only HLE message injects; disable once the first Gfx task proves SysMain is live.
static bool wr64_boot_hle_inject_enabled() {
    return !g_allow_vi_pulse.load(std::memory_order_acquire);
}

// Host-side map: OSThread RDRAM address -> entry VRAM passed to osCreateThread ($a2).
// The port's OSThread layout does not store entry PC at +0x10 (that field is state/pad).
std::unordered_map<uint32_t, uint32_t> g_thread_entry_by_vaddr;
static std::vector<uint32_t> g_deferred_libultra_threads;
static std::vector<uint32_t> g_pending_child_thread_starts;

constexpr uint32_t kMainThreadEntry = 0x80047530u;
constexpr uint32_t kAudioThreadEntry = 0x80047B20u;
constexpr uint32_t kSysMainThreadEntry = 0x80046DA0u;
constexpr uint32_t kViManagerEntry = 0x800C68F4u;
constexpr uint32_t kPiManagerEntry = 0x800CC450u;

uint64_t wr64_rdram_byte_offset(uint32_t vaddr) {
    return wr64_rdram_host_offset(vaddr);
}

uint8_t* n64_vram(uint8_t* rdram, uint32_t vaddr) {
    return wr64_rdram_byte_ptr(rdram, vaddr);
}

uint32_t& n64_mem32(uint8_t* rdram, uint32_t vaddr) {
    return *reinterpret_cast<uint32_t*>(wr64_rdram_byte_ptr(rdram, vaddr));
}

uint8_t& n64_mem8(uint8_t* rdram, uint32_t vaddr) {
    return *reinterpret_cast<uint8_t*>(wr64_rdram_byte_ptr(rdram, vaddr));
}

uint16_t& n64_mem16(uint8_t* rdram, uint32_t vaddr) {
    return *reinterpret_cast<uint16_t*>(wr64_rdram_byte_ptr(rdram, vaddr));
}


bool is_kseg0_rdram_pointer(uint32_t vaddr) {
    return vaddr >= 0x80000000u && vaddr < 0x80800000u;
}

constexpr uint32_t kSeg800F = 0x800F0000u;
constexpr uint32_t kSeg801E = 0x801E0000u;
constexpr uint32_t kTimerRootStruct = kSeg800F - 0x6FB0u;            // 0x800E9050
constexpr uint32_t kTimerAuxStruct = kTimerRootStruct + 0x30u;       // 0x800E9080
constexpr uint32_t kViThreadData = kSeg801E - 0x7890u;               // 0x801D8770 (viThread)
// WR64 vimgr BSS: viEventQueue @ 0x801D9938; do NOT use 0x801D9920 (vi manager stack top).
constexpr uint32_t kViEventQueue = kSeg801E - 0x66C8u;               // 0x801D9938
constexpr uint32_t kViManagerStackTop = kSeg801E - 0x66E0u;          // 0x801D9920
constexpr uint32_t kViEventMsgRing = kSeg801E - 0x6700u;             // 0x801D9900 (5 slots, below stack)
constexpr uint32_t kViMesgQueueGlobal = kSeg800F - 0x7068u;
constexpr uint32_t kOsViDevMgr = 0x800E8F90u;
constexpr uint32_t kTimerHandlerNtsc = kSeg800F - 0x6EB0u;
constexpr uint32_t kTimerHandlerPal = kSeg800F - 0x6E60u;
constexpr uint32_t kViRetraceMesg = kSeg801E - 0x66B0u;              // 0x801D9950
constexpr uint32_t kViCounterMesg = kSeg801E - 0x6698u;              // 0x801D9968
constexpr uint32_t kPiStatusReg = 0xA4400010u;
constexpr uint32_t kPiAccessQueue = kSeg801E - 0x5388u;  // 0x801DAC78
constexpr uint32_t kPiCmdBuf = kSeg801E - 0x54A0u;       // 0x801DAB60
constexpr uint32_t kPiCmdEntryCount = kSeg801E - 0x545Fu; // 0x801DABA1
constexpr uint32_t kBootGateMainThread = 0x800D4620u;  // Main_IdleThread -> osStartThread(Main_Thread)
constexpr uint32_t kBootGateViThread = 0x800D4628u;
constexpr uint32_t kBootGateEventsThread = 0x800D4624u;
constexpr uint32_t kMainThreadMesgQueue = 0x80154130u;   // gMainThreadMesgQueue (decomp symbol_addrs)
constexpr uint32_t kMainThreadMesgBuf = 0x80154260u;
constexpr uint32_t kMainOsThread = 0x80153B90u;          // Main_Thread OSThread (pri 0x64)
constexpr uint32_t kDisplayModeGlobal = 0x800DAB1Cu;     // D_800DAB1C (framebuffer copy mode)
constexpr uint32_t kGameStateGlobal = 0x800DAB24u;
// func_800922E4 uses lui 0x800E + 0xAB24 (not the decomp symbol at 0x800DAB24).
constexpr uint32_t kGameStateDispatchGlobal = 0x800EAB24u;
// Jump table ROM 0xA547C. func_800922E4 lw/jr uses lui 0x800F - 0x5384 => 0x800EAC7C;
// other dispatch paths read the +0x10000 mirror (0x800FAC7C). Seed both.
constexpr uint32_t kGameStateJumpTableMirrorDelta = 0x10000u;
constexpr uint32_t kJumpTableRdramAliasDelta = 0x20000u; // asset-load tables (lui 0x800F - 0x4Exx)
constexpr uint32_t kGameStateHandlerTableVram = 0x800FAC7Cu;
constexpr uint32_t kGameStateHandlerTableDispatchVram = 0x800EAC7Cu; // 922E4 lw/jr base
constexpr uint32_t kGameStateHandlerTableRom = 0xA547Cu;
constexpr uint32_t kGameStateHandlerCount = 0x66u;
static_assert(kGameStateHandlerTableVram - kGameStateHandlerTableDispatchVram == kGameStateJumpTableMirrorDelta);
// func_800926F4 switch table: lui 0x800F - 0x51EC => 0x800EAE14; high alias 0x800FAE14.
constexpr uint32_t kGsAuxHandlerTableVram = 0x800FAE14u;
constexpr uint32_t kGsAuxHandlerTableDispatchVram = 0x800EAE14u;
static_assert(kGsAuxHandlerTableVram - kGsAuxHandlerTableDispatchVram == kGameStateJumpTableMirrorDelta);
constexpr uint32_t kGsAuxHandlerTableRom = 0xA5614u;
constexpr uint32_t kGsAuxHandlerCount = 0x65u;
// unk_game_load reads 0x800F0000-0x4EB0 (VRAM 0x800AB150); ROM rodata is at 0xA5950.
constexpr uint32_t kUnkGameLoadTableVram = 0x800AB150u;
constexpr uint32_t kUnkGameLoadTableHighVram = kUnkGameLoadTableVram + kJumpTableRdramAliasDelta;
constexpr uint32_t kUnkGameLoadTableRom = 0xA5950u;
constexpr uint32_t kUnkGameLoadTableCount = 0x72u;
// GameLoad_LoadOverlay reads 0x800F0000-0x4CE0 (VRAM 0x800AB334); ROM rodata at 0xA5B20.
constexpr uint32_t kGameLoadJumpTableVram = 0x800AB334u;
constexpr uint32_t kGameLoadJumpTableHighVram = kGameLoadJumpTableVram + kJumpTableRdramAliasDelta;
constexpr uint32_t kGameLoadJumpTableRom = 0xA5B20u;
constexpr uint32_t kGameLoadJumpTableCount = 0x66u;
// gOverlayTable @ symbol_addrs.txt — 19 Overlay rows (0x20 bytes each).
constexpr uint32_t kOverlayTableVram = 0x800E4ED0u;
constexpr uint32_t kOverlayIoMesg = 0x801542A0u;
constexpr uint32_t kOverlayVram = 0x802C5800u;

struct Wr64OverlayRow {
    uint32_t rom_start;
    uint32_t rom_end;
    uint32_t text_start;
    uint32_t text_end;
    uint32_t data_start;
    uint32_t data_end;
    uint32_t bss_start;
    uint32_t bss_end;
};

// refs/Wave-Race-64/src/ovl_table.c (manual layout; linker OVERLAY_ENTRY macro is disabled).
static constexpr Wr64OverlayRow kWr64OverlayRows[] = {
    {0x001B1FB0u, 0x001B3EC0u, 0x802C5800u, 0x802C7960u, 0x802C7960u, 0x802C7A00u, 0x802C7A00u, 0x802C7AE0u},
    {0x001B3EC0u, 0x001B55A0u, 0x802C5800u, 0x802C71D0u, 0x802C71D0u, 0x802C7340u, 0x802C7340u, 0x802C7380u},
    {0x001B55A0u, 0x001B7960u, 0x802C5800u, 0x802C7B90u, 0x802C7B90u, 0x802C7DB0u, 0x802C7DB0u, 0x802C7E30u},
    {0x001B7960u, 0x001B9440u, 0x802C5800u, 0x802C7D00u, 0x802C7D00u, 0x802C7E40u, 0x802C7E40u, 0x802C7E80u},
    {0x001B9440u, 0x001BFF50u, 0x802C5800u, 0x802C76A0u, 0x802C76A0u, 0x802C76A0u, 0x802C76A0u, 0x802C76E0u},
    {0x001BFF50u, 0x001C2250u, 0x802C5800u, 0x802C7A00u, 0x802C7A00u, 0x802C7B00u, 0x802C7B00u, 0x802C7BA0u},
    {0x001C3D00u, 0x001C43F0u, 0x802C5800u, 0x802C5EB0u, 0x802C5EB0u, 0x802C5EF0u, 0x802C5EF0u, 0x802C5F00u},
    {0x001C2250u, 0x001C3780u, 0x802C5800u, 0x802C6C70u, 0x802C6C70u, 0x802C6D30u, 0x802C6D30u, 0x802C6D50u},
    {0x001C3780u, 0x001C3D00u, 0x802C5800u, 0x802C5D30u, 0x802C5D30u, 0x802C5D80u, 0x802C5D80u, 0x802C5D90u},
    {0x001C43F0u, 0x001C49A0u, 0x802C5800u, 0x802C5D20u, 0x802C5D20u, 0x802C5DB0u, 0x802C5DB0u, 0x802C5DC0u},
    {0x001C49A0u, 0x001C66D0u, 0x802C5800u, 0x802C7040u, 0x802C7040u, 0x802C7530u, 0x802C7530u, 0x802C7570u},
    {0x001C66D0u, 0x001C9150u, 0x802C5800u, 0x802C80C0u, 0x802C80C0u, 0x802C8280u, 0x802C8280u, 0x802C8290u},
    {0x001C9150u, 0x001CA480u, 0x802C5800u, 0x802C6AA0u, 0x802C6AA0u, 0x802C6B30u, 0x802C6B30u, 0x802C6B60u},
    {0x001CA480u, 0x001CAE40u, 0x802C5800u, 0x802C6150u, 0x802C6150u, 0x802C61C0u, 0x802C61C0u, 0x802C61D0u},
    {0x001CAE40u, 0x001CBAF0u, 0x802C5800u, 0x802C6460u, 0x802C6460u, 0x802C64B0u, 0x802C64B0u, 0x802C64C0u},
    {0x001CBAF0u, 0x001CF180u, 0x802C5800u, 0x802C8D50u, 0x802C8D50u, 0x802C8E90u, 0x802C8E90u, 0x802C8EC0u},
    {0x001CF180u, 0x001CFB60u, 0x802C5800u, 0x802C60F0u, 0x802C60F0u, 0x802C61E0u, 0x802C61E0u, 0x802C61F0u},
    {0x001CFB60u, 0x001D11D0u, 0x802C5800u, 0x802C6D50u, 0x802C6D50u, 0x802C6E70u, 0x802C6E70u, 0x802C6F50u},
    {0x001B55A0u, 0x001B9440u, 0x802C5800u, 0x802C9440u, 0x802C9440u, 0x802C96A0u, 0x802C96A0u, 0x802C96E0u},
};
static_assert(std::size(kWr64OverlayRows) == 19u);
constexpr uint32_t kOverlayBankUnloadSize =
    kWr64OverlayRows[0].rom_end - kWr64OverlayRows[0].rom_start;

// Title seg-8 DmaEntry chain (ROM 0x96EE0, stock D_800DC708 path).
constexpr uint32_t kTitleSeg8Scratch = 0x80360000u;
constexpr uint32_t kTitleSeg8PoolVirt = 0x80683180u;
constexpr uint32_t kTitleSeg8BranchProbeOff = 0x66180u;

struct Wr64TitleSeg8DmaEntry {
    uint32_t rom_start;
    uint32_t rom_end;
    uint32_t flag;
    uint32_t pool_offset;
};

constexpr Wr64TitleSeg8DmaEntry kTitleSeg8DmaChain[] = {
    {0x002E5FB0u, 0x002F9BE0u, 1u, 0x00000000u},
    {0x002F9BE0u, 0x0030BC50u, 1u, 0x00038000u},
};
constexpr uint32_t kOverlayLoadGate = 0x801CE63Cu;         // D_801CE63C — enables GameLoad_LoadOverlay
constexpr uint32_t kOverlayModeIdx = 0x801CE638u;
constexpr uint32_t kFrameCounterGlobal = 0x80151960u;
constexpr uint32_t kPiCmdMesgBufStock = 0x80154148u;     // overlaps queue structs @ 80154100–80154130
constexpr uint32_t kPiCmdMesgBufSafe = 0x80157000u;      // after gSinTable (0x80154350 + 0x2000)
constexpr uint32_t kBootMqVi = 0x801540E8u;
constexpr uint32_t kAudioTaskMesgOut = 0x801542D0u; // sAudioTaskMsg (stock sys_audio.c)
constexpr uint32_t kCurrentAudioTask = 0x801542B8u;  // gCurrentAudioTask (decomp symbol_addrs)
// Audio thread indirect queue pointers (audio_symbols.txt) + embedded OSMesgQueue storage.
constexpr uint32_t kPtrAudioTaskStartQueue = 0x800E85F8u;
constexpr uint32_t kPtrThreadCmdProcQueue = 0x800E85FCu;
constexpr uint32_t kPtrAudioSpecQueue = 0x800E8600u;
constexpr uint32_t kPtrAudioResetQueue = 0x800E8604u;
constexpr uint32_t kAudioTaskStartQueueObj = 0x801D8630u;
constexpr uint32_t kThreadCmdProcQueueObj = 0x801D8648u;
constexpr uint32_t kAudioSpecQueueObj = 0x801D8660u;
constexpr uint32_t kAudioResetQueueObj = 0x801D8678u;
constexpr uint32_t kBootMqEvents = 0x80154100u;
// Main_Thread VI throttle (main.c: D_800D4610..461C).
constexpr uint32_t kMainViFrameCounter = 0x800D4610u;
constexpr uint32_t kMainViFrameLastSync = 0x800D4614u;
constexpr uint32_t kMainViFrameThreshold = 0x800D4618u;
constexpr uint32_t kMainViFrameTarget = 0x800D461Cu;
// SysMain per-frame audio tick (n_alSynRemovePlayer) reads D_8003FE64 before audio is live.
constexpr uint32_t kSequenceChannelPtr = 0x8003FE64u;
constexpr uint32_t kSequenceChannelNone = 0x80044688u;
// Main/events boot handshake (main osSendMesg 0x33 @ 0x800478D4, events recv @ 0x8004721C).
constexpr uint32_t kBootMqMainSync = 0x80154118u;
constexpr uint32_t kBootMqTimer = 0x80154130u;
constexpr uint32_t kPiCmdQueue = 0x801540A0u;
constexpr uint32_t kPiSecondaryMesgQueue = 0x801540B8u; // msg buf @ 0x80154248 (main thread init)
constexpr uint32_t kPiEventsMesgQueue = 0x801540D0u;
constexpr uint32_t kContStatusArray = 0x801542E0u;
constexpr uint32_t kControllersPad = 0x801542F0u;
constexpr uint32_t kControllerOne = 0x80154308u;
constexpr uint32_t kContIndexMap = 0x80154330u;
constexpr uint32_t kContMaskByte = 0x80154340u;
constexpr uint32_t kContSiMesg = 0x80154348u;
constexpr uint32_t kContCountGlobal = 0x80154344u;
constexpr uint32_t kPiEventsMesgBuf = 0x8015424Cu;
// Game __osPiGetAccess (codeSEG_800B8CB0) blocks on this BSS queue until granted.
constexpr uint32_t kPiAccessClientQueue = 0x80044DB8u;
constexpr uint32_t kPiAccessClientMesgBuf = 0x80044DD8u;
constexpr uint32_t kPiManagerGlobal = 0x800E8FB0u; // osCreatePiManager BSS (0x800F0000 - 0x7050)
constexpr uint32_t kPiManagerHandler = 0x800CC370u;
constexpr uint32_t kPiManagerActiveFlag = 0x800E8FB0u;
constexpr uint32_t kPiStackMesgQueue = kSeg801E - 0x54D0u; // 0x801DAB30
// codeSEG_800CB9C0 direction==0: PI cart addr 0x1FC007C0 -> ROM offset 0x7C0.
constexpr uint32_t kPiRawInitRomOffset = 0x7C0u;
constexpr uint32_t kPiRawInitReadSize = 0x40u;
constexpr uint32_t kGfxPoolIdx = 0x8011F8E0u;
constexpr uint32_t kGfxPoolArr = 0x8011F8E8u;
constexpr uint32_t kGfxPoolGlobal = 0x801518B8u;
constexpr uint32_t kGfxTaskArr = 0x801518C0u;
constexpr uint32_t kSGfxTaskGlobal = 0x80151940u;
constexpr uint32_t kDisplayListHeadGlobal = 0x80151944u;
constexpr uint32_t kCurrentGfxTaskGlobal = 0x801542B4u;
constexpr uint32_t kGfxTaskMesg = 0x15u;
constexpr uint32_t kGfxTaskActiveGlobal = 0x800D4604u;
constexpr uint32_t kGfxTaskDpPendingGlobal = 0x800D460Cu;
constexpr size_t kOSTaskBytes = 0x40u;
constexpr uint32_t kGfxPoolBytes = 0x18FE8u;
constexpr uint32_t kFrameBuffersGlobal = 0x801542C0u;
constexpr uint32_t kFramebuffersIdxGlobal = 0x80151948u;
constexpr uint32_t kFramebufferIdxPrevGlobal = 0x8015194Cu;
constexpr uint32_t kFramebufferIdxAltGlobal = 0x80151950u;
constexpr uint32_t kSeg1BaseGlobal = 0x80151984u;
constexpr uint32_t kSeg2PtrGlobal = 0x8011EDE0u;
constexpr uint32_t kSeg7MatrixGlobal = 0x801CE5F8u;
constexpr uint32_t kSeg8BaseGlobal = 0x800D45F0u;
constexpr uint32_t kSeg13BaseGlobal = 0x800D45E4u;
constexpr uint32_t kSeg14BaseGlobal = 0x800D45E8u;
constexpr uint32_t kPlayersGlobal = 0x800DAB28u;
constexpr uint32_t kDisplayModeFbIdxGlobal = 0x800D45D8u;
constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;
// Scene framebuffer phys bases — stock indexes {D_801519B4, D_801519BC}, not B4 + stride.
constexpr uint32_t kSceneFbPhys0Global = 0x801519B4u;
constexpr uint32_t kSceneFbPhys1Global = 0x801519BCu;
constexpr uint32_t kSceneFbEnd0Global = 0x801519B8u;
constexpr uint32_t kSceneFbEnd1Global = 0x801519C0u;
constexpr uint32_t kSceneFbAux0Global = 0x801519C4u;
constexpr uint32_t kSceneFbAux1Global = 0x801519C8u;
constexpr uint32_t kSceneFbAuxSpan = 0x10000u;
constexpr uint32_t kSceneFbCourseDtOffset = 0x30000u;
constexpr uint32_t kSceneFbBankSpan = 0x79000u;
constexpr uint32_t kSceneFbCourseSpan = 0x40000u;
// Observed per-swap stride at crash site (0x80319FE0 + swap_idx * 0x20).
constexpr uint32_t kSceneFbSwapStride = 0x20u;
constexpr uint32_t kDlPtrTable = 0x802260BCu;
constexpr uint32_t kDlPtrTableRomExpect0 = 0x80226210u;
// D_1000000 bootstrap DL heads (segment 1); stock 468E0 branches here before scene sub-DLs.
constexpr uint32_t kSceneDlBootstrapOne = 0x01000000u;
constexpr uint32_t kSceneDlBootstrapTwo = 0x01000098u;

static std::atomic<uint32_t> g_wr64_pending_main_gfx_task{0};
// Set when any source posts 0x29 to kBootMqEvents; cleared when SysMain recv's it.
static std::atomic<bool> g_sysmain_event_pending{false};

static bool wr64_sysmain_event_outstanding(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kBootMqEvents)) {
        return false;
    }
    const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, kBootMqEvents));
    if (mq->msgCount > 0 && mq->validCount > 0) {
        return true;
    }
    // Queue is the source of truth; drop stale pending after consume.
    g_sysmain_event_pending.store(false, std::memory_order_release);
    return false;
}

static void wr64_publish_pending_main_gfx_task(uint32_t task);
static void wr64_wait_pending_main_gfx_dispatched(uint8_t* rdram);
static bool wr64_gfx_hw_pipeline_busy(uint8_t* rdram);
extern "C" void wr64_codeSEG_800474E4(uint8_t* rdram, recomp_context* ctx);
static void wr64_sanitize_mesg_queue_blocked_threads(uint8_t* rdram, uint32_t mq_addr);
static void wr64_fixup_scene_framebuffer_bases(uint8_t* rdram);

static bool wr64_main_mq_has_gfx_task_pending(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kMainThreadMesgQueue)) {
        return false;
    }
    const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, kMainThreadMesgQueue));
    if (mq->validCount <= 0) {
        return false;
    }
    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<int32_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        return false;
    }
    for (s32 i = 0; i < mq->validCount; ++i) {
        const s32 idx = (mq->first + i) % mq->msgCount;
        if (n64_mem32(rdram, msg_buf + static_cast<uint32_t>(idx) * sizeof(uint32_t)) == kGfxTaskMesg) {
            return true;
        }
    }
    return false;
}

static bool wr64_should_coalesce_gfx_task_send(uint8_t* rdram) {
    return n64_mem32(rdram, kGfxTaskActiveGlobal) != 0u || wr64_main_mq_has_gfx_task_pending(rdram);
}
// D_1008290 / boot-wave sub-DLs inside segment_F6090 (after the setup-only head at D_1000000).
// Boot UI Gfx entry (segment-3 vtx); enable once runtime vtx pool matches stock layout.
// constexpr uint32_t kSceneDlBootUiGfx = 0x010082F0u;
static uint32_t wr64_resolve_scene_branch_dl(uint8_t* rdram) {
    const uint32_t from_table = n64_mem32(rdram, kDlPtrTable);
    if (is_kseg0_rdram_pointer(from_table)) {
        return from_table;
    }
    return (n64_mem32(rdram, kPlayersGlobal) == 2u) ? kSceneDlBootstrapTwo : kSceneDlBootstrapOne;
}

static void wr64_seed_dl_ptr_table_if_needed(uint8_t* rdram) {
    const uint32_t current = n64_mem32(rdram, kDlPtrTable);
    if (is_kseg0_rdram_pointer(current)) {
        return;
    }
    n64_mem32(rdram, kDlPtrTable) = kDlPtrTableRomExpect0;
}
static void wr64_set_scene_gfx_context(uint8_t* rdram);
static void wr64_fixup_scene_gfx_pointer(uint8_t* rdram);
constexpr uint32_t kProgOverlayVram = WR64_PROG_BANK;
constexpr uint32_t kProgOverlayRom = 0x001B1FB0u;
constexpr uint32_t kProgOverlaySize = 0x00001F10u;

static void wr64_endian_swap_loaded_words(uint8_t* rdram, uint32_t dram_addr, uint32_t size) {
    if (!is_kseg0_rdram_pointer(dram_addr) || size < 4) {
        return;
    }
    uint8_t* const dst = n64_vram(rdram, dram_addr);
    for (uint32_t i = 0; i + 4 <= size; i += 4) {
        uint8_t* const p = dst + i;
        const uint8_t b0 = p[0];
        p[0] = p[3];
        p[3] = b0;
        const uint8_t b1 = p[1];
        p[1] = p[2];
        p[2] = b1;
    }
}

void wr64_preload_boot_prog_overlay(uint8_t* rdram) {
    static std::atomic<bool> loaded{false};
    if (loaded.load(std::memory_order_acquire)) {
        return;
    }
    const std::span<const uint8_t> rom = recomp::get_rom();
    if (kProgOverlayRom + kProgOverlaySize > rom.size()) {
        return;
    }
    unload_overlays(static_cast<int32_t>(kProgOverlayVram), kProgOverlaySize);
    recomp::do_rom_read(rdram, kProgOverlayVram, recomp::rom_base + kProgOverlayRom, kProgOverlaySize);
    wr64_endian_swap_loaded_words(rdram, kProgOverlayVram, kProgOverlaySize);
    load_overlays(kProgOverlayRom, static_cast<int32_t>(kProgOverlayVram), kProgOverlaySize);
    // load_overlays maps segment_1B1FB0_802C5800 onto the title dispatch VRAM; restore our router.
    recomp::overlays::add_loaded_function(0x802C5800, wr64_dispatch_802C5800);
    loaded.store(true, std::memory_order_release);
    ultramodern::boot_log(
        "[wr64] prog overlay: loaded ROM 0x%08X -> 0x%08X (%u bytes) [recompiled]\n",
        kProgOverlayRom,
        kProgOverlayVram,
        kProgOverlaySize);
}

static void wr64_reregister_title_dispatch() {
    recomp::overlays::add_loaded_function(0x802C5800, wr64_dispatch_802C5800);
    // Jump-table targets are labels inside codeSEG_800923D0; register exact VRAMs.
    recomp::overlays::add_loaded_function(0x80092654, wr64_jt_80092654);
}

static void wr64_fixup_gfx_matrix_pointer(uint8_t* rdram) {
    constexpr uint32_t kGfxMatrixBase = 0x801CB6C8u;
    constexpr uint32_t kMatrixStride = 0x17A0u;
    const uint32_t slot = n64_mem32(rdram, kGfxPoolIdx) & 1u;
    const uint32_t expected = kGfxMatrixBase + slot * kMatrixStride;
    const uint32_t cur = n64_mem32(rdram, kSeg7MatrixGlobal);
    if (cur != expected) {
        n64_mem32(rdram, kSeg7MatrixGlobal) = expected;
    }
}

void wr64_seed_framebuffers_if_needed(uint8_t* rdram) {
    static constexpr uint32_t kFbAddrs[4] = {
        0x8038F800u, 0x803B5000u, 0x803DA800u, 0x8038F800u};
    if (is_kseg0_rdram_pointer(n64_mem32(rdram, kFrameBuffersGlobal))) {
        return;
    }
    for (int i = 0; i < 4; ++i) {
        n64_mem32(rdram, kFrameBuffersGlobal + static_cast<uint32_t>(i) * 4u) = kFbAddrs[i];
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log("[wr64] seeded gFrameBuffers[0..3] @ 0x801542C0\n");
    }
}

static void wr64_wake_main_thread(uint8_t* rdram) {
    wr64_fixup_scene_gfx_pointer(rdram);
    if (!is_kseg0_rdram_pointer(kMainOsThread)) {
        return;
    }
    const PTR(OSThread) main_t = static_cast<PTR(OSThread)>(kMainOsThread);
    ultramodern::schedule_running_thread(rdram, main_t);
    ultramodern::wake_thread(rdram, main_t);
}

extern "C" void wr64_wake_main_after_rsp_event(uint8_t* rdram) {
    wr64_wake_main_thread(rdram);
}

extern "C" void wr64_on_vi_retrace(uint8_t* rdram) {
    if (!g_allow_vi_pulse.load(std::memory_order_acquire)) {
        return;
    }
    wr64_wake_main_thread(rdram);
}

static void wr64_wake_sysmain_thread(uint8_t* rdram) {
    constexpr uint32_t kSysMainOsThread = 0x80153EF0u;
    if (!is_kseg0_rdram_pointer(kSysMainOsThread)) {
        return;
    }
    const PTR(OSThread) sysmain_t = static_cast<PTR(OSThread)>(kSysMainOsThread);
    ultramodern::schedule_running_thread(rdram, sysmain_t);
    ultramodern::wake_thread(rdram, sysmain_t);
}

constexpr uint32_t kRspbootText = 0x800D22B0u;
constexpr uint32_t kGspFast3DText = 0x800D2380u;
constexpr uint32_t kGspFast3DData = 0x800EE310u;
constexpr uint32_t kDramStack = 0x80046400u;
constexpr uint32_t kTaskOutputBuffer = 0x800EEDD0u;
constexpr uint32_t kOSYieldData = 0x80045800u;

void wr64_sysmain_gfx_init_buffers(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kGfxPoolArr) || !is_kseg0_rdram_pointer(kGfxTaskArr)) {
        return;
    }
    uint32_t idx = n64_mem32(rdram, kGfxPoolIdx) ^ 1u;
    idx &= 1u;
    n64_mem32(rdram, kGfxPoolIdx) = idx;
    const uint32_t pool = kGfxPoolArr + idx * kGfxPoolBytes;
    const uint32_t task = kGfxTaskArr + idx * static_cast<uint32_t>(kOSTaskBytes);
    n64_mem32(rdram, kGfxPoolGlobal) = pool;
    n64_mem32(rdram, kSGfxTaskGlobal) = task;
    n64_mem32(rdram, kDisplayListHeadGlobal) = pool;
    n64_mem32(rdram, kCurrentGfxTaskGlobal) = task;
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log(
            "[wr64] gfx init idx=%u pool=0x%08X task=0x%08X dl=0x%08X\n",
            idx,
            pool,
            task,
            pool);
    }
}

static void wr64_publish_pending_main_gfx_task(uint32_t task) {
    if (is_kseg0_rdram_pointer(task)) {
        g_wr64_pending_main_gfx_task.store(task, std::memory_order_release);
    }
}

static void wr64_wait_pending_main_gfx_dispatched(uint8_t* rdram) {
    uint32_t waits = 0;
    uint32_t last_kick_task = 0u;
    while (g_wr64_pending_main_gfx_task.load(std::memory_order_acquire) != 0u) {
        // Main may run stock overlay/Gfx paths while SysMain waits; keep scene FB
        // globals course-relative so B4/C4+row cannot alias bank RAM (0x80319FE0).
        wr64_fixup_scene_framebuffer_bases(rdram);
        wr64_sanitize_mesg_queue_blocked_threads(rdram, kMainThreadMesgQueue);
        wr64_sanitize_mesg_queue_blocked_threads(rdram, kBootMqEvents);
        const uint32_t pending = g_wr64_pending_main_gfx_task.load(std::memory_order_relaxed);
        if (is_kseg0_rdram_pointer(pending)) {
            const uint32_t yield_ptr = n64_mem32(rdram, pending + 0x38u);
            if (!is_kseg0_rdram_pointer(yield_ptr) || yield_ptr < 0x80010000u) {
                n64_mem32(rdram, pending + 0x38u) = kOSYieldData;
            }
        }
        // Main can park in wait_for_host_frame_presented while a gfx mesg sits on its
        // queue — kick SpTask directly once HW/RT64 is idle instead of waiting on Main.
        if (pending != 0u && pending != last_kick_task && !wr64_gfx_hw_pipeline_busy(rdram) &&
            (waits >= 1u || wr64_main_mq_has_gfx_task_pending(rdram))) {
            ultramodern::boot_log(
                "[wr64] gfx kick: forcing Main SpTask (pending=0x%08X waits=%u)\n",
                pending,
                waits);
            recomp_context kick{};
            wr64_codeSEG_800474E4(rdram, &kick);
            last_kick_task = pending;
        }
        ultramodern::process_deferred_rsp_completions(rdram);
        ultramodern::run_next_thread_and_wait(rdram);
        if (++waits <= 8u) {
            ultramodern::boot_log(
                "[wr64] GfxInitBuffers: waiting for Main dispatch (pending=0x%08X)\n",
                g_wr64_pending_main_gfx_task.load(std::memory_order_relaxed));
        }
    }
}

static void wr64_log_framebuffer_sample(uint8_t* rdram, uint32_t fb, uint32_t tag) {
    if (!is_kseg0_rdram_pointer(fb) || tag > 128) {
        return;
    }
    const auto* pixels = reinterpret_cast<const uint16_t*>(n64_vram(rdram, fb));
    uint32_t nonzero = 0;
    for (int i = 0; i < 64; ++i) {
        if (pixels[i] != 0) {
            ++nonzero;
        }
    }
    const uint32_t mode = n64_mem32(rdram, kDisplayModeGlobal);
    const uint32_t field_px_idx = (mode == 3u) ? 640u : 320u;
    const uint16_t px_field = pixels[field_px_idx];
    uint32_t nonzero_field = 0;
    for (int i = 0; i < 64; ++i) {
        if (pixels[field_px_idx + i] != 0) {
            ++nonzero_field;
        }
    }
    ultramodern::boot_log(
        "[wr64] fb_sample #%u fb=0x%08X px0=0x%04X px_field(+0x%X)=0x%04X "
        "nonzero_base=%u nonzero_field=%u\n",
        tag,
        fb,
        pixels[0],
        field_px_idx * 2u,
        px_field,
        nonzero,
        nonzero_field);
}

static void wr64_log_dl_summary(uint8_t* rdram, uint32_t dl_start, uint32_t data_size, uint32_t tag) {
    if (!is_kseg0_rdram_pointer(dl_start) || tag > 24 || data_size < 8) {
        return;
    }
    const uint32_t cmds = data_size / 8u;
    const uint32_t show = cmds < 6u ? cmds : 6u;
    ultramodern::boot_log(
        "[wr64] dl_summary #%u start=0x%08X bytes=0x%X cmds=%u\n",
        tag,
        dl_start,
        data_size,
        cmds);
    for (uint32_t i = 0; i < show; ++i) {
        const uint32_t off = dl_start + i * 8u;
        const uint32_t w0 = n64_mem32(rdram, off);
        const uint32_t w1 = n64_mem32(rdram, off + 4u);
        ultramodern::boot_log("  dl[%u]=0x%08X%08X\n", i, w0, w1);
    }
}

static uint8_t wr64_game_state_byte_at(uint8_t* rdram, uint32_t addr) {
    const uint32_t word = n64_mem32(rdram, addr);
    if (word <= 0xFFu) {
        return static_cast<uint8_t>(word);
    }
    const uint32_t hi = (word >> 24) & 0xFFu;
    const uint32_t lo = word & 0xFFu;
    if (hi != 0u && hi < 0x80u) {
        return static_cast<uint8_t>(hi);
    }
    if (lo != 0u && lo < 0x80u) {
        return static_cast<uint8_t>(lo);
    }
    return static_cast<uint8_t>(hi);
}

static uint8_t wr64_game_state_byte(uint8_t* rdram) {
    return wr64_game_state_byte_at(rdram, kGameStateGlobal);
}

static void wr64_normalize_game_state_at(uint8_t* rdram, uint32_t addr) {
    const uint32_t raw = n64_mem32(rdram, addr);
    if (raw <= 0xFFu) {
        return;
    }
    n64_mem32(rdram, addr) = wr64_game_state_byte_at(rdram, addr);
}

static void wr64_sync_game_state_dispatch(uint8_t* rdram) {
    wr64_normalize_game_state_at(rdram, kGameStateGlobal);
    const uint32_t canonical = n64_mem32(rdram, kGameStateGlobal);
    n64_mem32(rdram, kGameStateDispatchGlobal) = canonical;
}

// Title overlays can clobber gGameState while dispatch still points at gs 5/6 handlers.
static void wr64_repair_title_game_state(uint8_t* rdram, uint8_t gs_at_enter) {
    if (gs_at_enter != 5u && gs_at_enter != 6u) {
        return;
    }
    const uint32_t raw = n64_mem32(rdram, kGameStateGlobal);
    const uint32_t dispatch = n64_mem32(rdram, kGameStateDispatchGlobal);
    if (raw <= 0xFFu && raw == dispatch) {
        return;
    }
    const uint8_t gs_now = wr64_game_state_byte_at(rdram, kGameStateGlobal);
    const uint8_t dispatch_byte =
        dispatch <= 0xFFu ? static_cast<uint8_t>(dispatch) : wr64_game_state_byte_at(rdram, kGameStateDispatchGlobal);
    if ((gs_now == 2u || gs_now == 1u || gs_now == 3u || gs_now == 4u) &&
        (dispatch_byte == 5u || dispatch_byte == 6u)) {
        n64_mem32(rdram, kGameStateGlobal) = dispatch_byte;
        wr64_sync_game_state_dispatch(rdram);
        static std::atomic<uint32_t> repair_log{0};
        if (repair_log.fetch_add(1) < 8u) {
            ultramodern::boot_log(
                "[wr64] title gs repair: gs=%u dispatch=%u -> gs=%u\n",
                gs_now,
                dispatch_byte,
                dispatch_byte);
        }
    }
}

static uint32_t wr64_rom_be32(const uint8_t* rom, size_t off) {
    return (static_cast<uint32_t>(rom[off]) << 24) | (static_cast<uint32_t>(rom[off + 1]) << 16) |
           (static_cast<uint32_t>(rom[off + 2]) << 8) | static_cast<uint32_t>(rom[off + 3]);
}

static bool wr64_is_plausible_mips_vram(uint32_t addr) {
    return addr >= 0x80000000u && addr < 0x80800000u;
}

static uint32_t wr64_fixup_mips_ptr_word(uint32_t word) {
    if (wr64_is_plausible_mips_vram(word)) {
        return word;
    }
    const uint32_t swapped = __builtin_bswap32(word);
    if (wr64_is_plausible_mips_vram(swapped)) {
        return swapped;
    }
    if (word < 0x00800000u) {
        return word | 0x80000000u;
    }
    return word;
}

static void wr64_refresh_jump_table_words(uint8_t* rdram, uint32_t dst_vram, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const uint32_t addr = dst_vram + static_cast<uint32_t>(i * 4u);
        const uint32_t cur = n64_mem32(rdram, addr);
        const uint32_t fixed = wr64_fixup_mips_ptr_word(cur);
        if (cur != fixed) {
            n64_mem32(rdram, addr) = fixed;
        }
    }
}

static bool wr64_jump_table_rdram_span_ok(uint32_t base_vram, size_t count) {
    if (!is_kseg0_rdram_pointer(base_vram)) {
        return false;
    }
    const uint32_t end = base_vram + static_cast<uint32_t>(count * 4u);
    return is_kseg0_rdram_pointer(end);
}

// Stock paths disagree on the RDRAM alias for the same ROM rodata tables.
// Game-state / gs-aux: high (+0x10000) vs 922E4/926F4 lui-0x800F dispatch base.
// Asset-load tables: low vs high (+0x20000).
static void wr64_copy_jump_table_rdram_alias(uint8_t* rdram, uint32_t src_vram, uint32_t dst_vram, size_t count) {
    if (!wr64_jump_table_rdram_span_ok(src_vram, count) || !wr64_jump_table_rdram_span_ok(dst_vram, count)) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        const uint32_t off = static_cast<uint32_t>(i * 4u);
        n64_mem32(rdram, dst_vram + off) = n64_mem32(rdram, src_vram + off);
    }
    wr64_refresh_jump_table_words(rdram, dst_vram, count);
}

static void wr64_mirror_jump_table_low_to_high(uint8_t* rdram, uint32_t low_vram, uint32_t mirror_delta, size_t count) {
    wr64_copy_jump_table_rdram_alias(rdram, low_vram, low_vram + mirror_delta, count);
}

static void wr64_mirror_jump_table_high_to_dispatch(
    uint8_t* rdram,
    uint32_t high_vram,
    uint32_t dispatch_vram,
    size_t count) {
    wr64_copy_jump_table_rdram_alias(rdram, high_vram, dispatch_vram, count);
}

static bool wr64_jump_table_low_entry_plausible(uint8_t* rdram, uint32_t low_vram, size_t index) {
    const uint32_t word = n64_mem32(rdram, low_vram + static_cast<uint32_t>(index * 4u));
    return wr64_is_plausible_mips_vram(word);
}

static void wr64_seed_rom_ptr_table(uint8_t* rdram, uint32_t dst_vram, size_t rom_off, size_t count) {
    const std::span<const uint8_t> rom = recomp::get_rom();
    const size_t bytes = count * 4u;
    if (rom_off + bytes > rom.size()) {
        return;
    }
    for (size_t i = 0; i < count; ++i) {
        n64_mem32(rdram, dst_vram + static_cast<uint32_t>(i * 4u)) =
            wr64_rom_be32(rom.data(), rom_off + i * 4u);
    }
    wr64_refresh_jump_table_words(rdram, dst_vram, count);
}

static void wr64_seed_dual_alias_jump_table(
    uint8_t* rdram,
    uint32_t high_vram,
    uint32_t dispatch_vram,
    size_t rom_off,
    size_t count) {
    wr64_seed_rom_ptr_table(rdram, high_vram, rom_off, count);
    wr64_mirror_jump_table_high_to_dispatch(rdram, high_vram, dispatch_vram, count);
    if (!wr64_jump_table_low_entry_plausible(rdram, dispatch_vram, 4u)) {
        wr64_refresh_jump_table_words(rdram, high_vram, count);
        wr64_mirror_jump_table_high_to_dispatch(rdram, high_vram, dispatch_vram, count);
    }
}

static void wr64_refresh_runtime_jump_tables(uint8_t* rdram) {
    wr64_refresh_jump_table_words(rdram, kGameStateHandlerTableVram, kGameStateHandlerCount);
    wr64_mirror_jump_table_high_to_dispatch(
        rdram, kGameStateHandlerTableVram, kGameStateHandlerTableDispatchVram, kGameStateHandlerCount);
    wr64_refresh_jump_table_words(rdram, kGsAuxHandlerTableVram, kGsAuxHandlerCount);
    wr64_mirror_jump_table_high_to_dispatch(
        rdram, kGsAuxHandlerTableVram, kGsAuxHandlerTableDispatchVram, kGsAuxHandlerCount);
    if (!wr64_jump_table_low_entry_plausible(rdram, kGsAuxHandlerTableDispatchVram, 4u)) {
        wr64_refresh_jump_table_words(rdram, kGsAuxHandlerTableVram, kGsAuxHandlerCount);
        wr64_mirror_jump_table_high_to_dispatch(
            rdram, kGsAuxHandlerTableVram, kGsAuxHandlerTableDispatchVram, kGsAuxHandlerCount);
    }
    wr64_refresh_jump_table_words(rdram, kUnkGameLoadTableVram, kUnkGameLoadTableCount);
    wr64_mirror_jump_table_low_to_high(rdram, kUnkGameLoadTableVram, kJumpTableRdramAliasDelta, kUnkGameLoadTableCount);
    wr64_refresh_jump_table_words(rdram, kGameLoadJumpTableVram, kGameLoadJumpTableCount);
    wr64_mirror_jump_table_low_to_high(rdram, kGameLoadJumpTableVram, kJumpTableRdramAliasDelta, kGameLoadJumpTableCount);
}

static void wr64_seed_game_state_jump_table(uint8_t* rdram) {
    wr64_seed_dual_alias_jump_table(
        rdram,
        kGameStateHandlerTableVram,
        kGameStateHandlerTableDispatchVram,
        kGameStateHandlerTableRom,
        kGameStateHandlerCount);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log(
            "[wr64] seeded game-state jump table high=0x%08X dispatch=0x%08X (delta=0x%X) <- ROM 0x%08X "
            "gs6_high=0x%08X gs4_dispatch=0x%08X (922E4 lw/jr base)\n",
            kGameStateHandlerTableVram,
            kGameStateHandlerTableDispatchVram,
            kGameStateJumpTableMirrorDelta,
            kGameStateHandlerTableRom,
            n64_mem32(rdram, kGameStateHandlerTableVram + 6u * 4u),
            n64_mem32(rdram, kGameStateHandlerTableDispatchVram + 4u * 4u));
    }
}

static void wr64_seed_gs_aux_jump_table(uint8_t* rdram) {
    wr64_seed_dual_alias_jump_table(
        rdram,
        kGsAuxHandlerTableVram,
        kGsAuxHandlerTableDispatchVram,
        kGsAuxHandlerTableRom,
        kGsAuxHandlerCount);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log(
            "[wr64] seeded gs-aux jump table high=0x%08X dispatch=0x%08X (delta=0x%X) <- ROM 0x%08X "
            "gs6->0x%08X\n",
            kGsAuxHandlerTableVram,
            kGsAuxHandlerTableDispatchVram,
            kGameStateJumpTableMirrorDelta,
            kGsAuxHandlerTableRom,
            n64_mem32(rdram, kGsAuxHandlerTableVram + 4u * 4u));
    }
}

static void wr64_seed_unk_game_load_jump_table(uint8_t* rdram) {
    wr64_seed_rom_ptr_table(
        rdram, kUnkGameLoadTableVram, kUnkGameLoadTableRom, kUnkGameLoadTableCount);
    wr64_mirror_jump_table_low_to_high(
        rdram, kUnkGameLoadTableVram, kJumpTableRdramAliasDelta, kUnkGameLoadTableCount);
}

static void wr64_seed_gameload_jump_table(uint8_t* rdram) {
    wr64_seed_rom_ptr_table(
        rdram, kGameLoadJumpTableVram, kGameLoadJumpTableRom, kGameLoadJumpTableCount);
    wr64_mirror_jump_table_low_to_high(
        rdram, kGameLoadJumpTableVram, kJumpTableRdramAliasDelta, kGameLoadJumpTableCount);
}

static void wr64_write_overlay_row(uint8_t* rdram, uint32_t index, const Wr64OverlayRow& row) {
    const uint32_t base = kOverlayTableVram + index * 0x20u;
    n64_mem32(rdram, base + 0x00u) = row.rom_start;
    n64_mem32(rdram, base + 0x04u) = row.rom_end;
    n64_mem32(rdram, base + 0x08u) = row.text_start;
    n64_mem32(rdram, base + 0x0Cu) = row.text_end;
    n64_mem32(rdram, base + 0x10u) = row.data_start;
    n64_mem32(rdram, base + 0x14u) = row.data_end;
    n64_mem32(rdram, base + 0x18u) = row.bss_start;
    n64_mem32(rdram, base + 0x1Cu) = row.bss_end;
}

static void wr64_seed_goverlay_table(uint8_t* rdram) {
    for (uint32_t i = 0; i < static_cast<uint32_t>(std::size(kWr64OverlayRows)); ++i) {
        wr64_write_overlay_row(rdram, i, kWr64OverlayRows[i]);
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log(
            "[wr64] seeded gOverlayTable @0x%08X (%zu entries)\n",
            kOverlayTableVram,
            std::size(kWr64OverlayRows));
    }
}

static int wr64_gameload_overlay_index(uint8_t gs) {
    switch (gs) {
    case 0x05u:
        return 0; // GAME_STATE_BOOT_UP -> OVL_BOOT_UP
    case 0x02u:
        return 1; // GAME_STATE_TITLE_SCREEN -> OVL_TITLE_SCREEN
    case 0x0Au:
        return 2; // GAME_STATE_RIDER_SELECT
    case 0x1Eu:
        return 3; // GAME_SATE_COURSE_OVERVIEW
    case 0x14u:
        return 4; // GAME_STATE_COURSE_SELECT
    case 0x34u:
        return 5; // GAME_STATE_RACE_RESULTS
    case 0x36u:
        return 6; // GAME_STATE_36
    case 0x32u:
        return 7; // GAME_STATE_TIME_TRIALS_RESULTS
    case 0x38u:
        return 8; // GAME_STATE_STUNT_MODE_RESULTS
    case 0x3Cu:
        return 9; // GAME_STATE_OPTIONS_MENU
    case 0x3Eu:
        return 10; // GAME_STATE_OPTIONS_CHANGE_NAMES
    case 0x42u:
        return 11; // GAME_STATE_OPTIONS_VIEW_RECORDS
    case 0x44u:
        return 12; // GAME_STATE_OPTIONS_CHANGE_CONDITIONS
    case 0x48u:
        return 13; // GAME_STATE_OPTIONS_AUDIO
    case 0x46u:
        return 14; // GAME_STATE_OPTIONS_ERASE_COURSE_RECORDS
    case 0x40u:
        return 15; // GAME_STATE_OPTIONS_SAVE_AND_LOAD
    case 0x50u:
        return 16; // GAME_STATE_50
    case 0x66u:
        return 17; // GAME_STATE_CEREMONY
    case 0x07u: // GAME_STATE_DEMO
    case 0x28u: // GAME_STATE_TIME_TRIAL
        return 18; // OVL_DEMO
    default:
        return -1;
    }
}

static void wr64_drain_pi_mesg_queue_if_full(uint8_t* rdram) {
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kPiSecondaryMesgQueue));
    if (mq->msgCount <= 0) {
        return;
    }
    if (mq->validCount < mq->msgCount - 1) {
        return;
    }
    mq->first = static_cast<s32>((mq->first + 1) % mq->msgCount);
    mq->validCount--;
}

static bool wr64_try_fixup_overlay_pi_dma(
    uint8_t* rdram,
    uint32_t& dev_addr,
    uint32_t& dram_addr,
    uint32_t& size) {
    if (dram_addr != kOverlayVram) {
        return false;
    }
    constexpr uint32_t kMaxOverlayBytes = 0x20000u;
    const uint32_t rom_hint = dev_addr & 0x00FFFFFFu;
    if (size > 0 && size <= kMaxOverlayBytes && rom_hint >= 0x001B0000u && rom_hint < 0x00200000u) {
        return false;
    }

    const int idx = wr64_gameload_overlay_index(wr64_game_state_byte(rdram));
    if (idx < 0) {
        return false;
    }

    const uint32_t bad_dev = dev_addr;
    const uint32_t bad_size = size;

    wr64_seed_goverlay_table(rdram);
    const Wr64OverlayRow& ovl = kWr64OverlayRows[static_cast<size_t>(idx)];
    dev_addr = ovl.rom_start;
    dram_addr = ovl.text_start;
    size = ovl.rom_end - ovl.rom_start;
    size = (size + 1u) & ~1u;

    static std::atomic<uint32_t> fix_count{0};
    const uint32_t n = fix_count.fetch_add(1) + 1;
    if (n <= 16) {
        ultramodern::boot_log(
            "[wr64] pi_dma overlay fixup #%u gs=%u ovl=%d rom=[0x%08X,0x%08X) size=0x%08X dram=0x%08X "
            "(was dev=0x%08X size=0x%08X)\n",
            n,
            wr64_game_state_byte(rdram),
            idx,
            ovl.rom_start,
            ovl.rom_end,
            size,
            dram_addr,
            bad_dev,
            bad_size);
    }
    return true;
}

static void wr64_seed_asset_load_jump_tables(uint8_t* rdram) {
    wr64_seed_unk_game_load_jump_table(rdram);
    wr64_seed_gameload_jump_table(rdram);
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log(
            "[wr64] seeded asset-load jump tables unk={0x%08X,0x%08X} gameload={0x%08X,0x%08X} "
            "delta=0x%X (gs5=0x%08X gs6=0x%08X)\n",
            kUnkGameLoadTableVram,
            kUnkGameLoadTableHighVram,
            kGameLoadJumpTableVram,
            kGameLoadJumpTableHighVram,
            kJumpTableRdramAliasDelta,
            n64_mem32(rdram, kUnkGameLoadTableVram + 4u * 4u),
            n64_mem32(rdram, kUnkGameLoadTableVram + 5u * 4u));
    }
}

static void wr64_repair_jump_table_rdram_aliases(uint8_t* rdram) {
    wr64_seed_game_state_jump_table(rdram);
    wr64_seed_gs_aux_jump_table(rdram);
    wr64_seed_asset_load_jump_tables(rdram);
    wr64_refresh_runtime_jump_tables(rdram);
}

static void wr64_seed_display_mode_fbs_if_needed(uint8_t* rdram) {
    const uint32_t fb0 = n64_mem32(rdram, kDisplayModeFbArray);
    if (is_kseg0_rdram_pointer(fb0)) {
        return;
    }
    n64_mem32(rdram, kDisplayModeFbArray + 0u) = WR64_FRAME_BUFFER_H0;
    n64_mem32(rdram, kDisplayModeFbArray + 4u) = WR64_FRAME_BUFFER_H1;
    n64_mem32(rdram, kDisplayModeFbIdxGlobal) = 0;
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log(
            "[wr64] seeded D_800D45DC[] = {0x%08X, 0x%08X}\n",
            WR64_FRAME_BUFFER_H0,
            WR64_FRAME_BUFFER_H1);
    }
}

static void wr64_sync_mode3_vi_swap(uint8_t* rdram);

static void wr64_apply_boot_display_mode_if_needed(uint8_t* rdram) {
    if (n64_mem32(rdram, kOverlayLoadGate) == 0) {
        return;
    }
    if (n64_mem32(rdram, kDisplayModeGlobal) == 3u) {
        return;
    }
    wr64_seed_display_mode_fbs_if_needed(rdram);
    static std::atomic<bool> transitioned{false};
    if (!transitioned.exchange(true)) {
        recomp_context ctx{};
        ctx.r29 = 0x80150E00u;
        // Stock title transition: gGameState=6, mode=3, D_801CE638=0x13, gate=1.
        segment_1B1FB0_802C7510(rdram, &ctx);
        wr64_sync_game_state_dispatch(rdram);
        wr64_seed_game_state_jump_table(rdram);
        wr64_seed_gs_aux_jump_table(rdram);
        wr64_seed_asset_load_jump_tables(rdram);
    }
    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log(
            "[wr64] boot display mode -> 3 (D_800D45DC swap path, gate=%u gGameState=%u D_801CE638=%u)\n",
            n64_mem32(rdram, kOverlayLoadGate),
            wr64_game_state_byte(rdram),
            n64_mem32(rdram, kOverlayModeIdx));
    }
    wr64_sync_mode3_vi_swap(rdram);
}

static uint32_t wr64_resolve_seg8_pool_virt(uint8_t* rdram) {
    const uint32_t raw = n64_mem32(rdram, kSeg8BaseGlobal);
    auto valid_pool = [&](uint32_t addr) -> bool {
        return is_kseg0_rdram_pointer(addr) &&
            n64_mem32(rdram, addr + kTitleSeg8BranchProbeOff) != 0u;
    };
    if (is_kseg0_rdram_pointer(raw) && valid_pool(raw)) {
        return raw;
    }
    if (raw < 0x00800000u) {
        const uint32_t kseg = raw | 0x80000000u;
        if (valid_pool(kseg)) {
            return kseg;
        }
    }
    if (valid_pool(kTitleSeg8PoolVirt)) {
        return kTitleSeg8PoolVirt;
    }
    if (is_kseg0_rdram_pointer(raw)) {
        return raw;
    }
    if (raw < 0x00800000u) {
        return raw | 0x80000000u;
    }
    return raw;
}

static void wr64_fixup_seg8_pool_pointer(uint8_t* rdram) {
    const uint32_t virt = wr64_resolve_seg8_pool_virt(rdram);
    if (virt != n64_mem32(rdram, kSeg8BaseGlobal) && is_kseg0_rdram_pointer(virt)) {
        n64_mem32(rdram, kSeg8BaseGlobal) = virt;
    }
}

// Stock SysMain calls unk_game_load after 92CF0, but segment_1B1FB0_802C5BA4 clears
// D_801CE63C before that point. Title seg-8 assets are loaded by unk_game_load gs=5
// (0x800950C8); gs=6 is a no-op. Boot jumps straight to gs=6, so load explicitly.
static bool wr64_seg8_pool_has_branch_content(uint8_t* rdram) {
    const uint32_t seg8 = wr64_resolve_seg8_pool_virt(rdram);
    if (!is_kseg0_rdram_pointer(seg8)) {
        return false;
    }
    // Title overlay BranchDL targets use seg8+0x66180 (see RT64 DL scan #2).
    const uint32_t w0 = n64_mem32(rdram, seg8 + kTitleSeg8BranchProbeOff);
    const uint32_t w1 = n64_mem32(rdram, seg8 + kTitleSeg8BranchProbeOff + 4u);
    if (w0 == 0u && w1 == 0u) {
        return false;
    }
    const uint8_t cmd = static_cast<uint8_t>(w0 >> 24);
    return cmd == 0xBCu || cmd == 0xE7u || cmd == 0xB8u || cmd == 0xBAu || cmd == 0xFFu;
}

static void wr64_load_title_mio0_to_seg8(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t seg8 = wr64_resolve_seg8_pool_virt(rdram);
    if (!is_kseg0_rdram_pointer(seg8)) {
        return;
    }
    for (const Wr64TitleSeg8DmaEntry& entry : kTitleSeg8DmaChain) {
        if (entry.flag == 0u) {
            break;
        }
        const uint32_t size = entry.rom_end - entry.rom_start;
        recomp_context dma_ctx = *ctx;
        dma_ctx.r4 = entry.rom_start;
        dma_ctx.r5 = kTitleSeg8Scratch;
        dma_ctx.r6 = size;
        codeSEG_80097F74(rdram, &dma_ctx);
        recomp_context mio_ctx = *ctx;
        mio_ctx.r4 = kTitleSeg8Scratch;
        mio_ctx.r5 = seg8 + entry.pool_offset;
        codeSEG_800B4D30(rdram, &mio_ctx);
    }
}

static void wr64_maybe_unk_game_load_before_overlay(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t gate = n64_mem32(rdram, kOverlayLoadGate);
    if (gate == 0) {
        return;
    }
    const uint8_t gs = wr64_game_state_byte(rdram);
    if (gs != 5u && gs != 6u) {
        return;
    }
    if (wr64_seg8_pool_has_branch_content(rdram)) {
        return;
    }
    wr64_fixup_seg8_pool_pointer(rdram);
    wr64_seed_asset_load_jump_tables(rdram);
    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    if (n <= 16u) {
        ultramodern::boot_log(
            "[wr64] title seg8 load #%u gs=%u gate=%u D_800D45F0=0x%08X\n",
            n,
            gs,
            gate,
            n64_mem32(rdram, kSeg8BaseGlobal));
    }
    wr64_load_title_mio0_to_seg8(rdram, ctx);
    if (n <= 16u) {
        const uint32_t seg8 = wr64_resolve_seg8_pool_virt(rdram);
        const uint32_t sample =
            is_kseg0_rdram_pointer(seg8) ? n64_mem32(rdram, seg8 + kTitleSeg8BranchProbeOff) : 0u;
        ultramodern::boot_log(
            "[wr64] title seg8 load #%u done branch@+0x%05X=0x%08X\n",
            n,
            kTitleSeg8BranchProbeOff,
            sample);
    }
}

static uint32_t wr64_resolve_color_framebuffer(uint8_t* rdram) {
    const uint32_t mode = n64_mem32(rdram, kDisplayModeGlobal);
    const uint32_t fb_idx = n64_mem32(rdram, kFramebuffersIdxGlobal);
    switch (mode) {
        case 3: {
            const uint32_t swap_idx = n64_mem32(rdram, kDisplayModeFbIdxGlobal) & 1u;
            return n64_mem32(rdram, kDisplayModeFbArray + swap_idx * 4u);
        }
        case 1:
        case 2:
            return n64_mem32(rdram, kFrameBuffersGlobal + 3u * 4u);
        case 0:
        default:
            return n64_mem32(rdram, kFrameBuffersGlobal + (fb_idx & 3u) * 4u);
    }
}

static void wr64_log_boot_progress(uint8_t* rdram, const char* tag) {
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n > 64 && (n % 60) != 0) {
        return;
    }
    ultramodern::boot_log(
        "[wr64] boot_progress [%s] gGameState=%u (raw=0x%08X) D_801CE63C=%u D_801CE638=%u mode=%u frame=%u\n",
        tag,
        wr64_game_state_byte(rdram),
        n64_mem32(rdram, kGameStateGlobal),
        n64_mem32(rdram, kOverlayLoadGate),
        n64_mem32(rdram, kOverlayModeIdx),
        n64_mem32(rdram, kDisplayModeGlobal),
        n64_mem32(rdram, kFrameCounterGlobal));
}

[[maybe_unused]] static void wr64_framebuffer_idx_swap(uint8_t* rdram) {
    const int32_t prev = static_cast<int32_t>(n64_mem32(rdram, kFramebufferIdxPrevGlobal));
    const int32_t cur = static_cast<int32_t>(n64_mem32(rdram, kFramebuffersIdxGlobal));
    n64_mem32(rdram, kFramebufferIdxPrevGlobal) = cur;
    n64_mem32(rdram, kFramebuffersIdxGlobal) = n64_mem32(rdram, kFramebufferIdxAltGlobal);
    n64_mem32(rdram, kFramebufferIdxAltGlobal) = prev;
    if (n64_mem32(rdram, kDisplayModeGlobal) == 3u) {
        n64_mem32(rdram, kDisplayModeFbIdxGlobal) ^= 1u;
    }
}

static uint32_t wr64_kseg0_to_phys(uint32_t addr) {
    if (addr >= 0x80000000u && addr < 0xA0000000u) {
        return addr & 0x1FFFFFFFu;
    }
    if (addr >= 0xA0000000u && addr < 0xC0000000u) {
        return addr & 0x1FFFFFFFu;
    }
    return addr;
}

static void wr64_restore_scene_framebuffer_globals(uint8_t* rdram) {
    const uint32_t bank = wr64_kseg0_to_phys(WR64_BANK_BUFFER);
    const uint32_t course = wr64_kseg0_to_phys(WR64_COURSE_BUFFER);
    n64_mem32(rdram, kSceneFbPhys0Global) = bank;
    n64_mem32(rdram, kSceneFbPhys1Global) = course;
    n64_mem32(rdram, kSceneFbEnd0Global) = bank + kSceneFbBankSpan;
    n64_mem32(rdram, kSceneFbEnd1Global) = course + kSceneFbCourseSpan;
    n64_mem32(rdram, kSceneFbAux0Global) = course + kSceneFbCourseDtOffset;
    n64_mem32(rdram, kSceneFbAux1Global) = course + kSceneFbCourseDtOffset + kSceneFbAuxSpan;
}

static void wr64_apply_title_mode3_scene_framebuffer_globals(uint8_t* rdram, uint32_t swap_idx) {
    const uint32_t course = wr64_kseg0_to_phys(WR64_COURSE_BUFFER);
    // Stock walks D_801519B4 + (swap_idx * 0x20) + row. With swap_idx=0 and B4=bank phys
    // that becomes 0x80316800+row (crash at 0x80319FE0). Keep B4 course-relative for both swaps.
    const uint32_t effective_b4 = course - swap_idx * kSceneFbSwapStride;
    n64_mem32(rdram, kSceneFbPhys0Global) = effective_b4;
    n64_mem32(rdram, kSceneFbEnd0Global) = effective_b4 + kSceneFbBankSpan;
    n64_mem32(rdram, kSceneFbPhys1Global) = course;
    n64_mem32(rdram, kSceneFbEnd1Global) = course + kSceneFbCourseSpan;
    // Stock: C4 = phys(course) + 0x30000, C8 = C4 + 0x10000 (sys_main.c).
    n64_mem32(rdram, kSceneFbAux0Global) = course + kSceneFbCourseDtOffset;
    n64_mem32(rdram, kSceneFbAux1Global) = course + kSceneFbCourseDtOffset + kSceneFbAuxSpan;
}

static void wr64_seed_scene_framebuffer_globals(uint8_t* rdram) {
    if (n64_mem32(rdram, kSceneFbPhys0Global) == 0u ||
        n64_mem32(rdram, kSceneFbPhys1Global) == 0u) {
        wr64_restore_scene_framebuffer_globals(rdram);
    }
}

static void wr64_fixup_scene_framebuffer_bases(uint8_t* rdram) {
    wr64_seed_scene_framebuffer_globals(rdram);
    const uint32_t swap_idx = n64_mem32(rdram, kDisplayModeFbIdxGlobal) & 1u;
    const uint32_t course = wr64_kseg0_to_phys(WR64_COURSE_BUFFER);
    const uint8_t gs = wr64_game_state_byte(rdram);
    const bool title_mode3 =
        n64_mem32(rdram, kDisplayModeGlobal) == 3u &&
        (gs == 2u || gs == 3u || gs == 5u || gs == 6u);
    if (title_mode3) {
        wr64_apply_title_mode3_scene_framebuffer_globals(rdram, swap_idx);
        static std::atomic<uint32_t> log_count{0};
        const uint32_t n = log_count.fetch_add(1) + 1;
        if (n <= 16u) {
            ultramodern::boot_log(
                "[wr64] scene_fb_fixup #%u title mode3 swap_idx=%u effective_B4=0x%08X "
                "effective_C4=0x%08X course=0x%08X\n",
                n,
                swap_idx,
                n64_mem32(rdram, kSceneFbPhys0Global),
                n64_mem32(rdram, kSceneFbAux0Global),
                course);
        }
        return;
    }
    if (swap_idx == 0u) {
        wr64_restore_scene_framebuffer_globals(rdram);
        return;
    }
    // Broken paths use D_801519B4 + (swap_idx * kSceneFbSwapStride) + row offset instead of
    // selecting D_801519BC. Publish an effective B4 so the arithmetic lands in course RAM.
    const uint32_t effective_b4 = course - swap_idx * kSceneFbSwapStride;
    n64_mem32(rdram, kSceneFbPhys0Global) = effective_b4;
    n64_mem32(rdram, kSceneFbEnd0Global) = effective_b4 + kSceneFbBankSpan;
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 8u) {
        ultramodern::boot_log(
            "[wr64] scene_fb_fixup #%u swap_idx=%u effective_B4=0x%08X course=0x%08X\n",
            n,
            swap_idx,
            effective_b4,
            course);
    }
}

static void wr64_set_scene_gfx_context(uint8_t* rdram) {
    uint32_t idx = n64_mem32(rdram, 0x800D4B0Cu);
    if (idx > 1u) {
        idx = 0;
        n64_mem32(rdram, 0x800D4B0Cu) = idx;
    }
    n64_mem32(rdram, 0x801AE948u) = 0x80198368u + idx * 0xB2F0u;
    n64_mem32(rdram, 0x801AE94Cu) = n64_mem32(rdram, 0x801AE948u);
    n64_mem32(rdram, 0x801AE950u) = 0;
}

[[maybe_unused]] static void wr64_advance_scene_gfx_context(uint8_t* rdram) {
    wr64_set_scene_gfx_context(rdram);
    n64_mem32(rdram, 0x800D4B0Cu) = n64_mem32(rdram, 0x800D4B0Cu) ^ 1u;
}

static void wr64_fixup_scene_gfx_pointer(uint8_t* rdram) {
    uint32_t scene = n64_mem32(rdram, 0x801AE948u);
    if (scene == 0) {
        wr64_set_scene_gfx_context(rdram);
        return;
    }
    // Stock paths sometimes leave a stripped RDRAM physical pointer (e.g. 0x00198368).
    if (scene < 0x80000000u) {
        if (scene >= 0x800000u) {
            scene |= 0x80000000u;
        } else if ((scene & 0x00FF0000u) == 0x00190000u || (scene & 0x00FF0000u) == 0x00180000u) {
            scene |= 0x80000000u;
        }
        n64_mem32(rdram, 0x801AE948u) = scene;
        n64_mem32(rdram, 0x801AE94Cu) = scene;
    }
}

static void wr64_ensure_gfx_bootstrap_for_task(uint8_t* rdram) {
    const uint32_t pool = n64_mem32(rdram, kGfxPoolGlobal);
    if (!is_kseg0_rdram_pointer(pool)) {
        return;
    }
    uint32_t dl_head = n64_mem32(rdram, kDisplayListHeadGlobal);
    if (!is_kseg0_rdram_pointer(dl_head)) {
        dl_head = pool;
        n64_mem32(rdram, kDisplayListHeadGlobal) = dl_head;
    }
    const uint32_t dl_bytes = (dl_head >= pool) ? (dl_head - pool) : 0u;
    // Stock SysMain asm leaves a fixed 0xC0-byte DL without gDPSetColorImage; RT64 sees fbPairs=0.
    if (dl_bytes >= 0x58u && dl_bytes != 0xC0u) {
        return;
    }
    n64_mem32(rdram, kDisplayListHeadGlobal) = pool;
    wr64_invoke_stock_gfx_bootstrap(rdram);
}

void wr64_prepare_gfx_task_for_main(uint8_t* rdram) {
    wr64_fixup_scene_framebuffer_bases(rdram);
    wr64_fixup_scene_gfx_pointer(rdram);
    wr64_seed_dl_ptr_table_if_needed(rdram);
    uint32_t task = n64_mem32(rdram, kSGfxTaskGlobal);
    if (!is_kseg0_rdram_pointer(task)) {
        wr64_sysmain_gfx_init_buffers(rdram);
        task = n64_mem32(rdram, kSGfxTaskGlobal);
    }
    const uint32_t pool = n64_mem32(rdram, kGfxPoolGlobal);
    if (!is_kseg0_rdram_pointer(pool) || !is_kseg0_rdram_pointer(task)) {
        return;
    }
    wr64_ensure_gfx_bootstrap_for_task(rdram);
    n64_mem32(rdram, kCurrentGfxTaskGlobal) = task;
    uint32_t dl_head = n64_mem32(rdram, kDisplayListHeadGlobal);
    if (!is_kseg0_rdram_pointer(dl_head)) {
        dl_head = pool;
        n64_mem32(rdram, kDisplayListHeadGlobal) = dl_head;
    }
    const uint32_t dl_start = pool;
    uint32_t data_size = 0;
    if (dl_head >= dl_start) {
        data_size = (dl_head - dl_start);
    }
    if (data_size == 0) {
        data_size = 8u;
    }
    n64_mem32(rdram, task + 0x00u) = 1u; // M_GFXTASK
    n64_mem32(rdram, task + 0x04u) = 0u;
    n64_mem32(rdram, task + 0x08u) = kRspbootText;
    n64_mem32(rdram, task + 0x0Cu) = kGspFast3DText - kRspbootText;
    n64_mem32(rdram, task + 0x10u) = kGspFast3DText;
    n64_mem32(rdram, task + 0x14u) = 0x1000u; // SP_UCODE_SIZE
    n64_mem32(rdram, task + 0x18u) = kGspFast3DData;
    n64_mem32(rdram, task + 0x1Cu) = 0x800u;  // SP_UCODE_DATA_SIZE
    n64_mem32(rdram, task + 0x20u) = kDramStack;
    n64_mem32(rdram, task + 0x24u) = 0x400u;   // SP_DRAM_STACK_SIZE8
    n64_mem32(rdram, task + 0x28u) = kTaskOutputBuffer;
    n64_mem32(rdram, task + 0x2Cu) = kTaskOutputBuffer + 0x6000u;
    n64_mem32(rdram, task + 0x30u) = dl_start;
    n64_mem32(rdram, task + 0x34u) = data_size;
    n64_mem32(rdram, task + 0x38u) = kOSYieldData;
    n64_mem32(rdram, task + 0x3Cu) = 0x900u; // OS_YIELD_DATA_SIZE (US)
    static std::atomic<uint32_t> gfx_dispatch_log{0};
    const uint32_t dispatch_n = gfx_dispatch_log.fetch_add(1) + 1;
    if (dispatch_n <= 16) {
        ultramodern::boot_log(
            "[wr64] Main gfx dispatch #%u task=0x%08X dl=0x%08X data_size=0x%X branch=0x%08X\n",
            dispatch_n,
            task,
            dl_start,
            data_size,
            wr64_resolve_scene_branch_dl(rdram));
        wr64_log_dl_summary(rdram, dl_start, data_size, dispatch_n);
    }
}

uint32_t vi_mesg_queue_addr(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kViEventQueue)) {
        return 0;
    }

    const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, kViEventQueue));
    if (mq->msgCount <= 0) {
        return 0;
    }

    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<intptr_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        return 0;
    }

    return kViEventQueue;
}

void clear_spurious_vi_stack_queue(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kViManagerStackTop)) {
        return;
    }
    if (n64_mem32(rdram, kViManagerStackTop + 0x10u) != 0) {
        std::memset(n64_vram(rdram, kViManagerStackTop), 0, sizeof(OSMesgQueue));
    }
    if (n64_mem32(rdram, kViMesgQueueGlobal) == kViManagerStackTop) {
        n64_mem32(rdram, kViMesgQueueGlobal) = 0;
    }
}

void ensure_vi_event_queue(uint8_t* rdram) {
    clear_spurious_vi_stack_queue(rdram);

    if (!is_kseg0_rdram_pointer(kViEventQueue) || !is_kseg0_rdram_pointer(kViEventMsgRing)) {
        return;
    }

    const int32_t existing_count = static_cast<int32_t>(n64_mem32(rdram, kViEventQueue + 0x10u));
    const uint32_t existing_buf = n64_mem32(rdram, kViEventQueue + 0x14u);
    if (existing_count > 0 && existing_buf == kViEventMsgRing) {
        return;
    }

    wr64_game_create_mesg_queue(rdram, kViEventQueue, kViEventMsgRing, 5);

    auto* retrace = reinterpret_cast<uint16_t*>(n64_vram(rdram, kViRetraceMesg));
    auto* counter = reinterpret_cast<uint16_t*>(n64_vram(rdram, kViCounterMesg));
    retrace[0] = 13;
    counter[0] = 14;

    n64_mem32(rdram, kOsViDevMgr) = 1u;
    n64_mem32(rdram, kOsViDevMgr + 0x8u) = kViEventQueue;
    n64_mem32(rdram, kOsViDevMgr + 0xCu) = kViEventQueue;
    n64_mem32(rdram, kViMesgQueueGlobal) = kViEventQueue;

    ultramodern::boot_log(
        "[wr64] vi_event_queue: created mq=0x%08X msg=0x%08X count=5\n",
        kViEventQueue,
        kViEventMsgRing);
}

uint32_t timer_handler_for_tv(uint8_t* rdram) {
    const uint32_t tv = n64_mem32(rdram, 0x80000300u);
    return (tv == 1) ? kTimerHandlerNtsc : kTimerHandlerPal;
}

bool is_vi_mesg_queue_pointer(uint32_t vaddr) {
    return vaddr == kViEventQueue;
}

bool is_timer_handler_pointer(uint32_t vaddr) {
    return vaddr == kTimerHandlerNtsc || vaddr == kTimerHandlerPal;
}

static std::atomic<bool> g_wr64_rsp_gfx_task_ready{false};
static std::atomic<bool> g_wr64_audio_init_pending{false};
static std::atomic<bool> g_wr64_audio_heap_ready{false};

bool trace_mesg_queue(uint32_t queue_vaddr, uint32_t call_index) {
    static std::atomic<uint32_t> vi_trace{0};
    static std::atomic<uint32_t> boot_trace{0};
    static std::atomic<uint32_t> main_trace{0};
    if (call_index <= 96) {
        return true;
    }
    if (queue_vaddr == kMainThreadMesgQueue) {
        return main_trace.fetch_add(1) < 24;
    }
    if (queue_vaddr == kViEventQueue || queue_vaddr == kBootMqVi || queue_vaddr == kBootMqEvents ||
        queue_vaddr == kBootMqMainSync) {
        return boot_trace.fetch_add(1) < 32;
    }
    return vi_trace.fetch_add(1) < 16;
}

bool is_boot_mesg_queue(uint32_t queue_vaddr) {
    // kBootMqTimer aliases gMainThreadMesgQueue — runtime VI recv is handled separately.
    return queue_vaddr == kBootMqVi || queue_vaddr == kBootMqEvents ||
           queue_vaddr == kBootMqMainSync;
}

bool is_scheduler_mesg_queue(uint32_t queue_vaddr) {
    return queue_vaddr == kBootMqVi || queue_vaddr == kBootMqEvents ||
           queue_vaddr == kBootMqMainSync;
}

static const char* wr64_boot_mq_tag(uint32_t queue_vaddr) {
    if (queue_vaddr == kBootMqVi) {
        return "boot_mq_audio";
    }
    if (queue_vaddr == kBootMqEvents) {
        return "boot_mq_events";
    }
    if (queue_vaddr == kBootMqMainSync) {
        return "boot_mq_sync";
    }
    return "boot_mq";
}

bool is_main_event_mesg_queue(uint32_t queue_vaddr) {
    return queue_vaddr == kMainThreadMesgQueue;
}

// Overlay loader (codeSEG_80097EC8) blocks on this queue; main and events threads share it.
// FIFO recv order is required — priority-ordered wake delivers PI completion to the wrong thread.
bool is_overlay_loader_mesg_queue(uint32_t queue_vaddr) {
    return queue_vaddr == kPiSecondaryMesgQueue;
}

int mesg_queue_depth(uint8_t* rdram, uint32_t queue_vaddr) {
    if (!is_kseg0_rdram_pointer(queue_vaddr)) {
        return 0;
    }
    const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, queue_vaddr));
    return static_cast<int>(mq->validCount);
}

static uint32_t wr64_running_thread_entry() {
    const PTR(OSThread) self = ultramodern::this_thread();
    if (self == NULLPTR) {
        return 0u;
    }
    const uint32_t thread_vaddr = static_cast<uint32_t>(self);
    const auto it = g_thread_entry_by_vaddr.find(thread_vaddr);
    return it != g_thread_entry_by_vaddr.end() ? it->second : 0u;
}

static void wr64_trace_game_state(uint8_t* rdram, const char* tag, uint32_t ra) {
    static std::atomic<uint32_t> last{0xFFFFFFFFu};
    const uint32_t cur = n64_mem32(rdram, kGameStateGlobal);
    const uint32_t dispatch = n64_mem32(rdram, kGameStateDispatchGlobal);
    const uint32_t prev = last.load(std::memory_order_relaxed);
    if (cur != prev || dispatch != cur) {
        last.store(cur, std::memory_order_relaxed);
        ultramodern::boot_log(
            "[wr64] STATE [%s] gGameState=%u raw=0x%08X dispatch=0x%08X ra=0x%08X\n",
            tag,
            wr64_game_state_byte_at(rdram, kGameStateGlobal),
            cur,
            dispatch,
            ra);
    }
}

static uint32_t wr64_resolve_seg8_pool_virt(uint8_t* rdram);

static void wr64_log_sysmain_post_events_recv(uint8_t* rdram, const recomp_context* ctx) {
    if (wr64_running_thread_entry() != kSysMainThreadEntry) {
        return;
    }

    uint32_t msg = 0;
    if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
        msg = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
    }
    if (msg != 0x29u) {
        return;
    }

    static std::atomic<uint32_t> hit{0};
    const uint32_t n = hit.fetch_add(1) + 1;
    if (n > 24u) {
        return;
    }

    const uint8_t gs = wr64_game_state_byte(rdram);
    const uint32_t gs_u = gs;
    const uint32_t gate = n64_mem32(rdram, kOverlayLoadGate);
    const uint32_t mode_idx = n64_mem32(rdram, kOverlayModeIdx);
    const uint32_t frame = n64_mem32(rdram, kFrameCounterGlobal);
    const uint32_t gs_handler = (gs_u < kGameStateHandlerCount)
        ? n64_mem32(rdram, kGameStateHandlerTableVram + gs_u * 4u)
        : 0u;
    const uint32_t gs_aux = (gs_u >= 2u && (gs_u - 2u) < kGsAuxHandlerCount)
        ? n64_mem32(rdram, kGsAuxHandlerTableVram + (gs_u - 2u) * 4u)
        : 0u;
    const uint32_t unk_load = (gs_u < kUnkGameLoadTableCount)
        ? n64_mem32(rdram, kUnkGameLoadTableVram + gs_u * 4u)
        : 0u;
    const uint32_t ovl_load = (gs_u < kGameLoadJumpTableCount)
        ? n64_mem32(rdram, kGameLoadJumpTableVram + gs_u * 4u)
        : 0u;

    ultramodern::boot_log(
        "[wr64] SysMain post-0x29 #%u frame=%u ra=0x%08X gGameState=%u gate=%u D_801CE638=%u "
        "gfx_busy=%u\n",
        n,
        frame,
        static_cast<uint32_t>(ctx->r31),
        gs,
        gate,
        mode_idx,
        wr64::gfx_pipeline_busy(rdram) ? 1u : 0u);
    ultramodern::boot_log(
        "[wr64] SysMain post-0x29 #%u segs "
        "D_80151984=0x%08X D_8015198C=0x%08X D_801519B4=0x%08X D_801519BC=0x%08X "
        "D_801519C4=0x%08X D_801519CC=0x%08X D_800D45F0=0x%08X seg8_resolved=0x%08X\n",
        n,
        n64_mem32(rdram, kSeg1BaseGlobal),
        n64_mem32(rdram, 0x8015198Cu),
        n64_mem32(rdram, 0x801519B4u),
        n64_mem32(rdram, 0x801519BCu),
        n64_mem32(rdram, 0x801519C4u),
        n64_mem32(rdram, 0x801519CCu),
        n64_mem32(rdram, kSeg8BaseGlobal),
        wr64_resolve_seg8_pool_virt(rdram));
    ultramodern::boot_log(
        "[wr64] SysMain post-0x29 #%u jt gs_tbl=0x%08X aux_tbl=0x%08X unk_load=0x%08X "
        "gameload=0x%08X dispatch=0x%08X\n",
        n,
        gs_handler,
        gs_aux,
        unk_load,
        ovl_load,
        n64_mem32(rdram, kGameStateDispatchGlobal));

    constexpr uint32_t kBadPtr = 0x80319FE0u;
    if (gs_handler == kBadPtr || gs_aux == kBadPtr || unk_load == kBadPtr || ovl_load == kBadPtr) {
        ultramodern::boot_log(
            "[wr64] SysMain post-0x29 #%u *** 0x80319FE0 in jump-table entry for gs=%u ***\n",
            n,
            gs);
    }
    for (uint32_t global = 0x80151964u; global <= 0x801519D4u; global += 4u) {
        if (n64_mem32(rdram, global) == kBadPtr) {
            ultramodern::boot_log(
                "[wr64] SysMain post-0x29 #%u *** 0x80319FE0 stored at 0x%08X ***\n",
                n,
                global);
        }
    }
    if (n64_mem32(rdram, kDisplayListHeadGlobal) == kBadPtr) {
        ultramodern::boot_log(
            "[wr64] SysMain post-0x29 #%u *** gDisplayListHead=0x80319FE0 ***\n",
            n);
    }
    const uint32_t pool_idx = n64_mem32(rdram, kGfxPoolIdx) & 1u;
    const uint32_t gfx_pool = n64_mem32(rdram, kGfxPoolGlobal);
    const uint32_t other_pool = kGfxPoolArr + ((pool_idx ^ 1u) & 1u) * kGfxPoolBytes;
    ultramodern::boot_log(
        "[wr64] SysMain post-0x29 #%u gfx pool_idx=%u gGfxPool=0x%08X other_pool=0x%08X "
        "gDisplayListHead=0x%08X sGfxTask=0x%08X pending_main=0x%08X\n",
        n,
        pool_idx,
        gfx_pool,
        other_pool,
        n64_mem32(rdram, kDisplayListHeadGlobal),
        n64_mem32(rdram, kSGfxTaskGlobal),
        g_wr64_pending_main_gfx_task.load(std::memory_order_relaxed));
}

static void wr64_on_boot_mq_events_recv_done(uint8_t* rdram, recomp_context* ctx) {
    uint32_t msg = 0;
    if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
        msg = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
    }
    if (msg == 0x29u) {
        g_sysmain_event_pending.store(false, std::memory_order_release);
    }
    wr64_fixup_scene_framebuffer_bases(rdram);
    wr64_log_sysmain_post_events_recv(rdram, ctx);
}

static void wr64_clear_sysmain_event_pending_on_consume(uint32_t queue, uint32_t received) {
    if (queue == kBootMqEvents && received == 0x29u) {
        g_sysmain_event_pending.store(false, std::memory_order_release);
    }
}

static void wr64_log_recv_block(uint8_t* rdram, uint32_t queue, uint32_t ra, uint32_t iter) {
    static std::atomic<uint32_t> block_log{0};
    const uint32_t n = block_log.fetch_add(1) + 1;
    if (n > 128u && (iter % 120u) != 1u) {
        return;
    }
    const PTR(OSThread) self = ultramodern::this_thread();
    const uint32_t tid = (self != NULLPTR) ? TO_PTR(OSThread, self)->id : 0u;
    ultramodern::boot_log(
        "[wr64] RECV block #%u iter=%u queue=0x%08X tid=%u entry=0x%08X depth=%d ra=0x%08X\n",
        n,
        iter,
        queue,
        tid,
        wr64_running_thread_entry(),
        mesg_queue_depth(rdram, queue),
        ra);
}

bool mesg_queue_is_flooded(uint8_t* rdram, uint32_t queue_vaddr) {
    if (!is_kseg0_rdram_pointer(queue_vaddr)) {
        return false;
    }
    const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, queue_vaddr));
    return mq->validCount > 1 || mq->validCount >= mq->msgCount - 1;
}

void wr64_cooperative_yield_if_flooded(uint8_t* rdram, uint32_t queue_vaddr) {
    if (!mesg_queue_is_flooded(rdram, queue_vaddr)) {
        return;
    }

    static thread_local uint32_t consecutive_flooded_ops = 0;
    consecutive_flooded_ops++;
    if (consecutive_flooded_ops < 2) {
        std::this_thread::yield();
        return;
    }

    consecutive_flooded_ops = 0;
    ultramodern::wait_for_host_frame_presented();
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    std::this_thread::yield();
}

static void wr64_pace_boot_scheduler_recv(uint8_t* rdram, uint32_t queue_vaddr) {
    if (!is_boot_mesg_queue(queue_vaddr)) {
        return;
    }
    static thread_local uint32_t rapid_boot_recvs = 0;
    rapid_boot_recvs++;
    if (rapid_boot_recvs < 4) {
        ultramodern::check_running_queue(rdram);
        return;
    }
    rapid_boot_recvs = 0;
    ultramodern::wait_for_host_frame_presented();
    ultramodern::check_running_queue(rdram);
}

constexpr uint32_t kSpStatusReg = 0xA4040010u;

void wr64_mark_sp_not_busy(uint8_t* rdram) {
    uint32_t& status = n64_mem32(rdram, kSpStatusReg);
    status &= ~0x1Cu;
}

static std::atomic<bool> g_sp_not_busy_pending{false};

void wr64_defer_sp_not_busy() {
    g_sp_not_busy_pending.store(true, std::memory_order_release);
}

void wr64_host_drain_scheduler_queue(uint8_t* rdram, uint32_t queue, recomp_context* ctx) {
    if (g_sp_not_busy_pending.exchange(false, std::memory_order_acq_rel)) {
        wr64_mark_sp_not_busy(rdram);
    }

    ultramodern::drain_external_messages(rdram);
    if (queue == kMainThreadMesgQueue || mesg_queue_depth(rdram, kMainThreadMesgQueue) > 0) {
        wr64_wake_main_thread(rdram);
    }

    const PTR(OSMesgQueue) mq = static_cast<PTR(OSMesgQueue)>(queue);
    ultramodern::wake_recv_blocked_threads(rdram, mq);
    (void)ctx;
}

void log_mesg_queue_state(const char* tag, uint8_t* rdram, uint32_t queue_vaddr) {
    if (!is_kseg0_rdram_pointer(queue_vaddr)) {
        ultramodern::boot_log( "[wr64] %s q=%08X (invalid)\n", tag, queue_vaddr);
        return;
    }

    const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, queue_vaddr));
    ultramodern::boot_log(
        "[wr64] %s q=%08X valid=%d first=%d count=%d msg=%08X blocked_recv=%08X blocked_send=%08X\n",
        tag,
        queue_vaddr,
        static_cast<int>(mq->validCount),
        static_cast<int>(mq->first),
        static_cast<int>(mq->msgCount),
        static_cast<uint32_t>(static_cast<intptr_t>(mq->msg)),
        static_cast<uint32_t>(static_cast<intptr_t>(mq->blocked_on_recv)),
        static_cast<uint32_t>(static_cast<intptr_t>(mq->blocked_on_send)));
}

void restore_timer_handler_field(uint8_t* rdram, uint32_t struct_addr) {
    uint32_t& field8 = n64_mem32(rdram, struct_addr + 0x8);
    if (is_vi_mesg_queue_pointer(field8) || !is_kseg0_rdram_pointer(field8) ||
        (field8 != kTimerHandlerNtsc && field8 != kTimerHandlerPal)) {
        field8 = timer_handler_for_tv(rdram);
    }
}

void restore_timer_handler_fields(uint8_t* rdram) {
    restore_timer_handler_field(rdram, kTimerAuxStruct);
    restore_timer_handler_field(rdram, kTimerRootStruct);

    const uint32_t active = n64_mem32(rdram, kSeg800F - 0x6F4C);
    if (is_kseg0_rdram_pointer(active) && active != kTimerAuxStruct && active != kTimerRootStruct) {
        restore_timer_handler_field(rdram, active);
    }
}

// Embedded osCreateMesgQueue @0x800C6310 seeds empty thread lists with this sentinel.
constexpr uint32_t kEmbeddedEmptyQueueHead = 0x800E9000u;

bool is_embedded_empty_queue_sentinel(uint32_t vaddr) {
    return vaddr == kEmbeddedEmptyQueueHead;
}

void sanitize_mesg_queue_heads(uint8_t* rdram, uint32_t queue_ptr) {
    if (!is_kseg0_rdram_pointer(queue_ptr)) {
        return;
    }
    uint32_t& recv_head = n64_mem32(rdram, queue_ptr + 0x0);
    uint32_t& send_head = n64_mem32(rdram, queue_ptr + 0x4);
    if (is_embedded_empty_queue_sentinel(recv_head)) {
        recv_head = 0;
    }
    if (is_embedded_empty_queue_sentinel(send_head)) {
        send_head = 0;
    }
}

static bool is_plausible_osthread_vaddr(uint32_t vaddr) {
    return vaddr >= 0x80010000u && vaddr < 0x80800000u;
}

void wr64_sanitize_mesg_queue_blocked_threads(uint8_t* rdram, uint32_t mq_addr) {
    sanitize_mesg_queue_heads(rdram, mq_addr);
    if (!is_kseg0_rdram_pointer(mq_addr)) {
        return;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, mq_addr));
    const auto fix_head = [&](PTR(OSThread)& head) {
        const uint32_t u = static_cast<uint32_t>(static_cast<intptr_t>(head));
        if (head != NULLPTR && !is_embedded_empty_queue_sentinel(u) && !is_plausible_osthread_vaddr(u)) {
            head = NULLPTR;
        }
    };
    fix_head(mq->blocked_on_recv);
    fix_head(mq->blocked_on_send);
}

// Queue inject without waking blocked_on_recv — WR64 recv bridges poll wr64_mesg_recv_fast.
void wr64_deliver_mesg_no_wake(uint8_t* rdram, uint32_t mq_addr, OSMesg msg) {
    if (!is_kseg0_rdram_pointer(mq_addr)) {
        return;
    }
    sanitize_mesg_queue_heads(rdram, mq_addr);
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, mq_addr));
    if (mq->msgCount <= 0 || mq->validCount >= mq->msgCount) {
        return;
    }
    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<intptr_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        return;
    }
    const s32 slot = (mq->first + mq->validCount) % mq->msgCount;
    n64_mem32(rdram, msg_buf + static_cast<uint32_t>(slot) * sizeof(uint32_t)) =
        static_cast<uint32_t>(msg);
    mq->validCount++;
    if (mq_addr == kBootMqEvents && static_cast<uint32_t>(msg) == 0x29u) {
        g_sysmain_event_pending.store(true, std::memory_order_release);
    }
}

[[maybe_unused]] void wr64_jam_mesg_no_wake(uint8_t* rdram, uint32_t mq_addr, OSMesg msg) {
    if (!is_kseg0_rdram_pointer(mq_addr)) {
        return;
    }
    sanitize_mesg_queue_heads(rdram, mq_addr);
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, mq_addr));
    if (mq->msgCount <= 0 || mq->validCount >= mq->msgCount) {
        return;
    }
    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<intptr_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        return;
    }
    mq->first = (mq->first + mq->msgCount - 1) % mq->msgCount;
    n64_mem32(rdram, msg_buf + static_cast<uint32_t>(mq->first) * sizeof(uint32_t)) =
        static_cast<uint32_t>(msg);
    mq->validCount++;
}

void fix_mesg_queue_struct(uint8_t* rdram, uint32_t queue_ptr) {
    if (queue_ptr != kViEventQueue || !is_kseg0_rdram_pointer(queue_ptr)) {
        return;
    }
    sanitize_mesg_queue_heads(rdram, queue_ptr);

    uint32_t& msg_buf = n64_mem32(rdram, queue_ptr + 0x14);
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        msg_buf = kViRetraceMesg;
    }
    if (n64_mem32(rdram, queue_ptr + 0x10) == 0) {
        n64_mem32(rdram, queue_ptr + 0x10) = 8;
    }
}

void init_boot_thread_gates(uint8_t* rdram) {
    const uint8_t main_before = n64_mem8(rdram, kBootGateMainThread);
    const uint8_t vi_before = n64_mem8(rdram, kBootGateViThread);
    const uint8_t events_before = n64_mem8(rdram, kBootGateEventsThread);
    // Always assert gates — audio boot can clobber D_800D4624 before SysMain is created.
    n64_mem8(rdram, kBootGateMainThread) = 1;
    n64_mem8(rdram, kBootGateViThread) = 1;
    n64_mem8(rdram, kBootGateEventsThread) = 1;
    wr64_set_scene_gfx_context(rdram);
    wr64_seed_dl_ptr_table_if_needed(rdram);
    ultramodern::boot_log(
        "[wr64] boot thread gates: main=0x%08X was=%u now=%u vi=0x%08X was=%u now=%u "
        "events=0x%08X was=%u now=%u\n",
        kBootGateMainThread,
        main_before,
        n64_mem8(rdram, kBootGateMainThread),
        kBootGateViThread,
        vi_before,
        n64_mem8(rdram, kBootGateViThread),
        kBootGateEventsThread,
        events_before,
        n64_mem8(rdram, kBootGateEventsThread));
}

void enable_boot_vi_delivery() {
    if (!g_allow_vi_pulse.exchange(true, std::memory_order_acq_rel)) {
        ultramodern::set_vi_retrace_mesg_enabled(true);
        ultramodern::boot_log("[wr64] boot VI: retrace mesg + present pulse enabled\n");
    }
}

static constexpr uint32_t kViMmioBase = 0xA4400000u;

static void wr64_sync_host_vi_regs_from_rdram(uint8_t* rdram) {
    const uint32_t rdram_origin = n64_mem32(rdram, kViMmioBase + 0x04u);
    if (rdram_origin == 0u) {
        return;
    }
    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    if (vi == nullptr) {
        return;
    }
    vi->VI_STATUS_REG = n64_mem32(rdram, kViMmioBase + 0x00u);
    vi->VI_ORIGIN_REG = n64_mem32(rdram, kViMmioBase + 0x04u);
    vi->VI_WIDTH_REG = n64_mem32(rdram, kViMmioBase + 0x08u);
    vi->VI_INTR_REG = n64_mem32(rdram, kViMmioBase + 0x0Cu);
    vi->VI_V_CURRENT_LINE_REG = n64_mem32(rdram, kViMmioBase + 0x10u);
    vi->VI_TIMING_REG = n64_mem32(rdram, kViMmioBase + 0x14u);
    vi->VI_V_SYNC_REG = n64_mem32(rdram, kViMmioBase + 0x18u);
    vi->VI_H_SYNC_REG = n64_mem32(rdram, kViMmioBase + 0x1Cu);
    vi->VI_LEAP_REG = n64_mem32(rdram, kViMmioBase + 0x20u);
    vi->VI_H_START_REG = n64_mem32(rdram, kViMmioBase + 0x24u);
    vi->VI_V_START_REG = n64_mem32(rdram, kViMmioBase + 0x28u);
    vi->VI_V_BURST_REG = n64_mem32(rdram, kViMmioBase + 0x2Cu);
    vi->VI_X_SCALE_REG = n64_mem32(rdram, kViMmioBase + 0x30u);
    vi->VI_Y_SCALE_REG = n64_mem32(rdram, kViMmioBase + 0x34u);
}

static bool wr64_host_vi_is_visible(const ultramodern::renderer::ViRegs& vi) {
    const bool blank = (vi.VI_STATUS_REG & 3u) == 0u;
    const uint32_t h_start = (vi.VI_H_START_REG >> 16) & 0x3FFu;
    return !blank && h_start > 0u;
}

static void wr64_seed_host_vi_ntsc_defaults(ultramodern::renderer::ViRegs* vi) {
    if (vi == nullptr) {
        return;
    }
    if ((vi->VI_STATUS_REG & 3u) == 0u) {
        vi->VI_STATUS_REG = (vi->VI_STATUS_REG & ~3u) | 2u;
    }
    if ((vi->VI_WIDTH_REG & 0xFFFu) == 0u) {
        vi->VI_WIDTH_REG = 0x140u;
    }
    if (((vi->VI_H_START_REG >> 16) & 0x3FFu) == 0u) {
        vi->VI_H_START_REG = 0x006C02ECu;
    }
    if (vi->VI_V_START_REG == 0u) {
        vi->VI_V_START_REG = 0x2501FFu;
    }
    if (vi->VI_Y_SCALE_REG == 0u) {
        vi->VI_Y_SCALE_REG = 0x400u;
    }
    if (vi->VI_X_SCALE_REG == 0u) {
        vi->VI_X_SCALE_REG = 0x200u;
    }
    if (vi->VI_TIMING_REG == 0u) {
        vi->VI_TIMING_REG = 0x03E52239u;
    }
    if (vi->VI_V_SYNC_REG == 0u) {
        vi->VI_V_SYNC_REG = 0x20Du;
    }
    if (vi->VI_H_SYNC_REG == 0u) {
        vi->VI_H_SYNC_REG = 0xC15u;
    }
    if (vi->VI_LEAP_REG == 0u) {
        vi->VI_LEAP_REG = 0x0C150C15u;
    }
    if (vi->VI_V_BURST_REG == 0u) {
        vi->VI_V_BURST_REG = 0xE0204u;
    }
}

static void wr64_ensure_host_vi_visible_impl(uint8_t* rdram) {
    wr64_sync_host_vi_regs_from_rdram(rdram);
    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    if (vi == nullptr) {
        return;
    }
    if (!wr64_host_vi_is_visible(*vi)) {
        osViBlack(0);
        ultramodern::commit_vi_swap();
        wr64_sync_host_vi_regs_from_rdram(rdram);
    }
    if (!wr64_host_vi_is_visible(*vi)) {
        if ((vi->VI_STATUS_REG & 3u) == 0u) {
            vi->VI_STATUS_REG = (vi->VI_STATUS_REG & ~3u) | 2u;
        }
        if (((vi->VI_H_START_REG >> 16) & 0x3FFu) == 0u) {
            vi->VI_H_START_REG = 0x006C02ECu;
        }
        if (vi->VI_ORIGIN_REG == 0u) {
            wr64_seed_framebuffers_if_needed(rdram);
            constexpr uint32_t kFbIdx = 0x80151948u;
            constexpr uint32_t kFbs = 0x801542C0u;
            const uint32_t fb_idx = n64_mem32(rdram, kFbIdx) & 3u;
            uint32_t fb = n64_mem32(rdram, kFbs + fb_idx * 4u);
            if (!is_kseg0_rdram_pointer(fb)) {
                fb = 0x8038F800u;
            }
            vi->VI_ORIGIN_REG = (fb & 0x1FFFFFFFu) + 0x280u;
        }
    }
    wr64_seed_host_vi_ntsc_defaults(vi);
    osViBlack(0);
    ultramodern::commit_vi_swap();
}

extern "C" void wr64_osViBlack_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t active = static_cast<uint32_t>(ctx->r4);
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log("[wr64] osViBlack(%u) #%u\n", active, n);
    }
    if (active) {
        osViBlack(1);
        ultramodern::commit_vi_swap();
    }
    // Title overlays call osViBlack(true) without a matching false before the next
    // frame. Leaving VI_STATE_BLACK latched forces hStart=0 and RT64 skips display.
    osViBlack(0);
    ultramodern::commit_vi_swap();
    wr64_ensure_host_vi_visible_impl(rdram);
}

extern "C" void wr64_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr64_seed_framebuffers_if_needed(rdram);
    uint32_t fb = static_cast<uint32_t>(ctx->r4);
    if (!is_kseg0_rdram_pointer(fb)) {
        const uint32_t idx = n64_mem32(rdram, kFramebuffersIdxGlobal) & 3u;
        fb = n64_mem32(rdram, kFrameBuffersGlobal + idx * 4u);
        if (!is_kseg0_rdram_pointer(fb)) {
            fb = 0x8038F800u;
        }
        ctx->r4 = fb;
    }
    if (n64_mem32(rdram, kDisplayModeGlobal) == 3u) {
        wr64_seed_display_mode_fbs_if_needed(rdram);
        // Stock SysMain uses osViSwapBuffer(D_800D45DC[D_800D45D8]) in mode 3.
        const uint32_t mode_idx = n64_mem32(rdram, kDisplayModeFbIdxGlobal) & 1u;
        const uint32_t mode_fb = n64_mem32(rdram, kDisplayModeFbArray + mode_idx * 4u);
        if (is_kseg0_rdram_pointer(mode_fb)) {
            fb = mode_fb;
            ctx->r4 = fb;
        }
    }
    static std::atomic<uint32_t> swap_log{0};
    const uint32_t n = swap_log.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log(
            "[wr64] osViSwapBuffer fb=0x%08X idx=%u\n",
            fb,
            n64_mem32(rdram, kFramebuffersIdxGlobal));
        wr64_log_framebuffer_sample(rdram, fb, n);
        wr64_log_boot_progress(rdram, "vi-swap");
    }
    osViSwapBuffer_recomp(rdram, ctx);
    if (n64_mem32(rdram, kDisplayModeGlobal) == 3u) {
        wr64::commit_mode3_draw_framebuffer(rdram, fb & 0x00FFFFFFu);
    }
    // Stock hardware: osViSwapBuffer queues the next FB; VI_ORIGIN latches on retrace.
    // commit_vi_swap() here applied the swap immediately and put VI one frame ahead of RDP.
    if (n <= 16u) {
        const uint32_t cur = static_cast<uint32_t>(osViGetCurrentFramebuffer());
        const uint32_t next = static_cast<uint32_t>(osViGetNextFramebuffer());
        const ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        const uint32_t origin = (vi != nullptr) ? (vi->VI_ORIGIN_REG & 0x00FFFFFFu) : 0u;
        ultramodern::boot_log(
            "[wr64] osViSwapBuffer #%u: queued=0x%08X osVi{cur=0x%08X,next=0x%08X} VI_ORIGIN=0x%08X (live until retrace)\n",
            n,
            fb,
            cur,
            next,
            origin);
    }
}

static void wr64_sync_mode3_vi_swap(uint8_t* rdram) {
    if (n64_mem32(rdram, kDisplayModeGlobal) != 3u) {
        return;
    }
    wr64_seed_display_mode_fbs_if_needed(rdram);
    const uint32_t idx = n64_mem32(rdram, kDisplayModeFbIdxGlobal) & 1u;
    const uint32_t fb = n64_mem32(rdram, kDisplayModeFbArray + idx * 4u);
    wr64::commit_mode3_draw_framebuffer(rdram, fb & 0x00FFFFFFu);
}

extern "C" void wr64_static_0_800980C8(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    init_boot_thread_gates(rdram);
}

// boot_dma1 menu walk — covered by recompiled boot_dma1_801E5464 (label @ 0x801E5470).

extern "C" void boot_dma1_801E5464(uint8_t* rdram, recomp_context* ctx);
extern "C" void boot_dma1_801E6A4C(uint8_t* rdram, recomp_context* ctx);
extern "C" void boot_dma1_801E71A8(uint8_t* rdram, recomp_context* ctx);

extern "C" void wr64_boot_dma_noop(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

extern "C" void wr64_boot_dma1_801E6A4C(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

extern "C" void wr64_boot_dma1_801E71A8(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

// boot_dma1 menu walk entry @ 0x801E5470 — fix scene/DL globals before stock body.
extern "C" void wr64_boot_dma1_801E5470(uint8_t* rdram, recomp_context* ctx) {
    wr64_fixup_scene_gfx_pointer(rdram);
    wr64_seed_dl_ptr_table_if_needed(rdram);
    boot_dma1_801E5464(rdram, ctx);
}

extern "C" void wr64_static_1_801EAFB4(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    ctx->r2 = ADD32(S32(0x8023 << 16), -0x7588);
    ctx->r14 = MEM_H(ctx->r2, 0);
    if (ctx->r14 == 0) {
        return;
    }
    ctx->r15 = MEM_W(ctx->r2, 4);
    ctx->r8 = MEM_W(ctx->r2, 8);
    ctx->r24 = ADD32(ctx->r15, 1);
    ctx->r1 = SIGNED(ctx->r24) < SIGNED(ctx->r8) ? 1 : 0;
    if (ctx->r1 != 0) {
        MEM_W(4, ctx->r2) = ctx->r24;
        return;
    }
    MEM_W(4, ctx->r2) = ctx->r24;
    ctx->r9 = MEM_H(ctx->r2, 2);
    ctx->r1 = ADD32(0, 4);
    MEM_W(4, ctx->r2) = 0;
    ctx->r10 = ADD32(ctx->r9, 1);
    MEM_H(2, ctx->r2) = ctx->r10;
    ctx->r11 = MEM_H(ctx->r2, 2);
    if (ctx->r11 != ctx->r1) {
        return;
    }
    MEM_H(2, ctx->r2) = 0;
}

uint32_t patch_null_thread_entry(uint32_t entry_pc, OSPri pri) {
    if (entry_pc != 0) {
        return entry_pc;
    }
    switch (pri) {
        case 10:
            return 0x80046DA0u; // events consumer
        case 20:
            return 0x80047B20u; // VI consumer
        case 100:
            return 0x80047530u; // main game loop (codeSEG_80047530)
        case 254:
            return 0x800C68F4u; // libultra VI manager
        default:
            return 0;
    }
}

extern "C" void wr64_osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osStartThread_recomp(uint8_t* rdram, recomp_context* ctx);
static void wr64_osStartThread_impl(uint8_t* rdram, recomp_context* ctx, bool allow_defer);
static void wr64_osStartThread_force(uint8_t* rdram, recomp_context* ctx);
static void wr64_start_deferred_libultra_managers(uint8_t* rdram);
static void wr64_start_pending_child_threads(uint8_t* rdram);

extern "C" void wr64_osCreateThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    uint32_t entry_pc = static_cast<uint32_t>(ctx->r6);
    const uint32_t thread_vaddr = static_cast<uint32_t>(ctx->r4);
    const OSId id = static_cast<OSId>(ctx->r5);
    const OSPri pri = static_cast<OSPri>(MEM_W(0x14, ctx->r29));
    const uint32_t patched = patch_null_thread_entry(entry_pc, pri);
    if (patched != entry_pc) {
        entry_pc = patched;
        ctx->r6 = entry_pc;
        ultramodern::boot_log(
            "[wr64] osCreateThread patched null entry: thread=0x%08X pri=%u -> 0x%08X\n",
            thread_vaddr,
            static_cast<unsigned>(pri),
            entry_pc);
    }

    OSPri effective_pri = pri;
    if (entry_pc == 0x800C68F4u && pri > 20) {
        effective_pri = 20;
        MEM_W(0x14, ctx->r29) = effective_pri;
        ultramodern::boot_log(
            "[wr64] osCreateThread: cap VI manager pri %u -> %u for boot progress\n",
            static_cast<unsigned>(pri),
            static_cast<unsigned>(effective_pri));
    }
    // PI manager (0x800CC450) at pri 150 preempts Main_IdleThread before it can osStartThread(Main).
    if (entry_pc == 0x800CC450u && pri > 10) {
        effective_pri = 10;
        MEM_W(0x14, ctx->r29) = effective_pri;
        ultramodern::boot_log(
            "[wr64] osCreateThread: cap PI manager pri %u -> %u for boot progress\n",
            static_cast<unsigned>(pri),
            static_cast<unsigned>(effective_pri));
    }

    if (entry_pc != 0 && is_kseg0_rdram_pointer(thread_vaddr)) {
        g_thread_entry_by_vaddr[thread_vaddr] = entry_pc;
    }
    if (entry_pc == 0x800CC450u) {
        ensure_pi_manager_globals(rdram);
    }

    static std::atomic<uint32_t> create_count{0};
    const uint32_t n = create_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log(
            "[wr64] osCreateThread #%u thread=0x%08X id=%d entry=0x%08X pri=%u sp=0x%08X\n",
            n,
            thread_vaddr,
            static_cast<int>(id),
            entry_pc,
            static_cast<unsigned>(effective_pri),
            static_cast<uint32_t>(MEM_W(0x10, ctx->r29)));
    }

    recomp::invoke_thread_create_callback(rdram, ctx);
    osCreateThread(rdram,
        static_cast<int32_t>(ctx->r4),
        id,
        static_cast<int32_t>(ctx->r6),
        static_cast<int32_t>(ctx->r7),
        static_cast<int32_t>(MEM_W(0x10, ctx->r29)),
        effective_pri);

    // Main_IdleThread gates child threads on D_800D462x; force-start when gates are set.
    // Recompiled osStartThread calls may not reach our HLE during nested osCreateThread boot.
    if (entry_pc == kMainThreadEntry) {
        const uint8_t gate = n64_mem8(rdram, kBootGateMainThread);
        ultramodern::boot_log(
            "[wr64] Main_Thread created: D_800D4620=0x%02X\n",
            gate);
        if (gate != 0) {
            recomp_context start_ctx{};
            start_ctx.r4 = thread_vaddr;
            wr64_osStartThread_recomp(rdram, &start_ctx);
        }
    } else if (entry_pc == kAudioThreadEntry) {
        const uint8_t gate = n64_mem8(rdram, kBootGateViThread);
        ultramodern::boot_log(
            "[wr64] Audio thread created: D_800D4628=0x%02X (deferred — audio heap not ready)\n",
            gate);
        // Stock starts audio here, but SysAudio_AudioThreadEntry crashes until loadAudioTable
        // + audio heap init complete. Main_Thread should osStartThread when ready; if that
        // never fires, audio stays off (VI tick msgs on 0x801540E8 are overwritten harmlessly).
        if (gate != 0) {
            g_pending_child_thread_starts.push_back(thread_vaddr);
        }
    } else if (entry_pc == kSysMainThreadEntry) {
        n64_mem8(rdram, kBootGateEventsThread) = 1;
        const uint8_t gate = n64_mem8(rdram, kBootGateEventsThread);
        ultramodern::boot_log(
            "[wr64] SysMain thread created: D_800D4624=0x%02X (deferred start)\n",
            gate);
        if (gate != 0) {
            g_pending_child_thread_starts.push_back(thread_vaddr);
        }
    }
}

extern "C" void wr64_codeSEG_800C6310(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mq = static_cast<uint32_t>(ctx->r4);
    uint32_t msg_buf = static_cast<uint32_t>(ctx->r5);
    int32_t count = static_cast<int32_t>(ctx->r6);
    if (!is_kseg0_rdram_pointer(mq)) {
        return;
    }
    if (mq == kPiCmdQueue && msg_buf == kPiCmdMesgBufStock) {
        msg_buf = kPiCmdMesgBufSafe;
    }
    const int32_t existing_count = static_cast<int32_t>(n64_mem32(rdram, mq + 0x10u));
    const uint32_t existing_msg = n64_mem32(rdram, mq + 0x14u);
    if (mq == kMainThreadMesgQueue) {
        if (count <= 0) {
            count = 16;
        }
        if (!is_kseg0_rdram_pointer(msg_buf)) {
            msg_buf = kMainThreadMesgBuf;
        }
        if (existing_count == count && existing_msg == msg_buf) {
            return;
        }
        if (existing_count <= 0 && is_kseg0_rdram_pointer(existing_msg)) {
            auto* existing_mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, mq));
            n64_mem32(rdram, mq + 0x10u) = static_cast<uint32_t>(count);
            n64_mem32(rdram, mq + 0x14u) = msg_buf;
            if (existing_mq->validCount > 0) {
                static std::atomic<uint32_t> repair_count{0};
                const uint32_t n = repair_count.fetch_add(1) + 1;
                if (n <= 16) {
                    ultramodern::boot_log(
                        "[wr64] main_mq: osCreateMesgQueue repair (valid=%d)\n",
                        static_cast<int>(existing_mq->validCount));
                }
            }
            return;
        }
    }
    const uint32_t recv_head = n64_mem32(rdram, mq + 0x0u);
    const uint32_t send_head = n64_mem32(rdram, mq + 0x4u);
    if (existing_count != 0 && existing_msg == msg_buf &&
        is_embedded_empty_queue_sentinel(recv_head) &&
        is_embedded_empty_queue_sentinel(send_head) &&
        (mq == kPiSecondaryMesgQueue || mq == kPiEventsMesgQueue)) {
        return;
    }
    // Inlined game codeSEG_800C6310 — never call the patched symbol (recursion).
    n64_mem32(rdram, mq + 0x0u) = kEmbeddedEmptyQueueHead;
    n64_mem32(rdram, mq + 0x4u) = kEmbeddedEmptyQueueHead;
    n64_mem32(rdram, mq + 0x8u) = 0u;
    n64_mem32(rdram, mq + 0xCu) = 0u;
    n64_mem32(rdram, mq + 0x10u) = static_cast<uint32_t>(count);
    n64_mem32(rdram, mq + 0x14u) = msg_buf;
}

extern "C" void wr64_codeSEG_800B8CB0(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    static std::atomic<uint32_t> access_count{0};
    const uint32_t n = access_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log( "[wr64] pi_get_access #%u (immediate grant)\n", n);
    }
    if (is_kseg0_rdram_pointer(kPiAccessClientQueue) &&
        is_kseg0_rdram_pointer(kPiAccessClientMesgBuf)) {
        if (n64_mem32(rdram, kPiAccessClientQueue + 0x10u) == 0) {
            wr64_game_create_mesg_queue(rdram, kPiAccessClientQueue, kPiAccessClientMesgBuf, 1);
        }
        auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kPiAccessClientQueue));
        if (mq->validCount == 0) {
            ultramodern::deliver_mesg_immediate(
                rdram,
                static_cast<PTR(OSMesgQueue)>(kPiAccessClientQueue),
                static_cast<OSMesg>(0));
        }
    }
    ctx->r2 = 0;
}

static void wr64_start_deferred_libultra_managers(uint8_t* rdram) {
    (void)rdram;
    if (g_deferred_libultra_threads.empty()) {
        return;
    }
    // PI/VI managers stay deferred — wr64_osPiRawStartDma HLE handles cart DMA during boot.
    // Force-starting the PI manager thread runs codeSEG_800C5A00 mid-recv and corrupts Main state.
    ultramodern::boot_log(
        "[wr64] leaving %zu libultra manager thread(s) deferred (HLE DMA)\n",
        g_deferred_libultra_threads.size());
}

static void wr64_ensure_boot_scheduler_queues(uint8_t* rdram) {
    auto ensure = [&](uint32_t mq, uint32_t msg_buf, int32_t count) {
        if (!is_kseg0_rdram_pointer(mq) || !is_kseg0_rdram_pointer(msg_buf)) {
            return;
        }
        if (static_cast<int32_t>(n64_mem32(rdram, mq + 0x10u)) == 0) {
            wr64_game_create_mesg_queue(rdram, mq, msg_buf, count);
        }
    };
    ensure(kBootMqVi, 0x80154250u, 1);
    ensure(kBootMqEvents, 0x80154254u, 1);
    ensure(kBootMqMainSync, 0x80154258u, 1);
}

static void ensure_main_thread_mesg_queue(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kMainThreadMesgQueue) ||
        !is_kseg0_rdram_pointer(kMainThreadMesgBuf)) {
        return;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kMainThreadMesgQueue));
    const int32_t count = static_cast<int32_t>(n64_mem32(rdram, kMainThreadMesgQueue + 0x10u));
    const uint32_t msg_buf = n64_mem32(rdram, kMainThreadMesgQueue + 0x14u);
    if (count <= 0 || !is_kseg0_rdram_pointer(msg_buf)) {
        // PI cmd ring init can clobber msgCount/msg; repair in place if messages remain.
        if (mq->validCount > 0 && mq->validCount <= 16) {
            static std::atomic<uint32_t> repair_count{0};
            const uint32_t n = repair_count.fetch_add(1) + 1;
            if (n <= 16) {
                ultramodern::boot_log(
                    "[wr64] main_mq: repair fields (valid=%d count=%d msg=0x%08X)\n",
                    static_cast<int>(mq->validCount),
                    count,
                    msg_buf);
            }
            n64_mem32(rdram, kMainThreadMesgQueue + 0x10u) = 16u;
            n64_mem32(rdram, kMainThreadMesgQueue + 0x14u) = kMainThreadMesgBuf;
            sanitize_mesg_queue_heads(rdram, kMainThreadMesgQueue);
            return;
        }
        static std::atomic<uint32_t> recreate_count{0};
        const uint32_t n = recreate_count.fetch_add(1) + 1;
        if (n <= 16) {
            ultramodern::boot_log(
                "[wr64] main_mq: recreate queue (count=%d msg=0x%08X)\n",
                count,
                msg_buf);
        }
        wr64_game_create_mesg_queue(rdram, kMainThreadMesgQueue, kMainThreadMesgBuf, 16);
        sanitize_mesg_queue_heads(rdram, kMainThreadMesgQueue);
    }
}

static void wr64_osStartThread_force(uint8_t* rdram, recomp_context* ctx) {
    wr64_osStartThread_impl(rdram, ctx, false);
}

static void wr64_start_pending_child_threads(uint8_t* rdram) {
    if (g_pending_child_thread_starts.empty()) {
        return;
    }
    // SysMain (pri 10) before audio (pri 20); PI manager must run before SysMain DMA.
    wr64_start_deferred_libultra_managers(rdram);
    std::vector<uint32_t> sysmain_first;
    std::vector<uint32_t> audio_second;
    for (const uint32_t thread_vaddr : g_pending_child_thread_starts) {
        const auto it = g_thread_entry_by_vaddr.find(thread_vaddr);
        const uint32_t entry_pc = it != g_thread_entry_by_vaddr.end() ? it->second : 0;
        if (entry_pc == kSysMainThreadEntry) {
            sysmain_first.push_back(thread_vaddr);
        } else if (entry_pc == kAudioThreadEntry) {
            audio_second.push_back(thread_vaddr);
        }
    }
    g_pending_child_thread_starts.clear();
    for (const uint32_t thread_vaddr : sysmain_first) {
        recomp_context start_ctx{};
        start_ctx.r4 = thread_vaddr;
        wr64_osStartThread_force(rdram, &start_ctx);
    }
    for (const uint32_t thread_vaddr : audio_second) {
        recomp_context start_ctx{};
        start_ctx.r4 = thread_vaddr;
        wr64_osStartThread_force(rdram, &start_ctx);
    }
}

static std::atomic<bool> g_child_threads_bootstrapped{false};
static std::atomic<bool> g_defer_sysmain_bootstrap{false};

static void wr64_flush_deferred_sysmain_bootstrap(uint8_t* rdram) {
    if (!g_defer_sysmain_bootstrap.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    wr64_start_pending_child_threads(rdram);
}

static void wr64_maybe_bootstrap_child_threads(uint8_t* rdram);

static void wr64_main_on_vi_retrace(uint8_t* rdram) {
    recomp_context send_ctx{};
    send_ctx.r4 = kBootMqVi;
    send_ctx.r5 = 0x1Fu;
    send_ctx.r6 = OS_MESG_NOBLOCK;
    wr64_bridge_osSendMesg(rdram, &send_ctx);

    int32_t counter = static_cast<int32_t>(n64_mem32(rdram, kMainViFrameCounter));
    int32_t threshold = static_cast<int32_t>(n64_mem32(rdram, kMainViFrameThreshold));
    if (threshold <= 0 || threshold > 16) {
        threshold = 1;
        n64_mem32(rdram, kMainViFrameThreshold) = 1u;
    }
    if (counter < 0 || counter > 1'000'000) {
        counter = static_cast<int32_t>(n64_mem32(rdram, kMainViFrameLastSync));
        if (counter < 0 || counter > 1'000'000) {
            counter = 0;
        }
    }
    counter++;
    n64_mem32(rdram, kMainViFrameCounter) = static_cast<uint32_t>(counter);

    const int32_t last_sync = static_cast<int32_t>(n64_mem32(rdram, kMainViFrameLastSync));
    if ((counter - last_sync) < threshold) {
        return;
    }
    if (wr64_sysmain_event_outstanding(rdram)) {
        static std::atomic<uint32_t> skip_count{0};
        const uint32_t skip_n = skip_count.fetch_add(1) + 1;
        if (skip_n <= 32) {
            ultramodern::boot_log(
                "[wr64] main VI retrace: skip 0x29 (outstanding) counter=%d thresh=%d\n",
                counter,
                threshold);
        }
        return;
    }

    n64_mem32(rdram, kMainViFrameLastSync) = static_cast<uint32_t>(counter);
    const uint32_t target = n64_mem32(rdram, kMainViFrameTarget);
    n64_mem32(rdram, kMainViFrameThreshold) = target != 0u ? target : 1u;

    send_ctx.r4 = kBootMqEvents;
    send_ctx.r5 = 0x29u;
    send_ctx.r6 = OS_MESG_NOBLOCK;
    wr64_bridge_osSendMesg(rdram, &send_ctx);

    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 32 && static_cast<int32_t>(send_ctx.r2) == 0) {
        ultramodern::boot_log(
            "[wr64] main VI retrace #%u -> SysMain 0x29 (counter=%d thresh=%d pending=%u)\n",
            n,
            counter,
            threshold,
            g_sysmain_event_pending.load(std::memory_order_relaxed) ? 1u : 0u);
    }
}

static void wr64_main_handle_vi_message(uint8_t* rdram, uint32_t received) {
    if (received != 0x19u) {
        return;
    }
    wr64_maybe_bootstrap_child_threads(rdram);
    wr64_main_on_vi_retrace(rdram);
}

static void wr64_maybe_bootstrap_child_threads(uint8_t* rdram) {
    if (g_child_threads_bootstrapped.exchange(true)) {
        return;
    }
    if (g_pending_child_thread_starts.empty() && g_deferred_libultra_threads.empty()) {
        g_child_threads_bootstrapped.store(false);
        return;
    }
    ultramodern::boot_log("[wr64] bootstrapping deferred child/manager threads\n");
    wr64_ensure_boot_scheduler_queues(rdram);
    // Start SysMain on the next Main osSendMesg so VI handler can post 0x1F/0x29 first.
    g_defer_sysmain_bootstrap.store(true, std::memory_order_release);
}

extern "C" void wr64_osStartThread_recomp(uint8_t* rdram, recomp_context* ctx) {
    wr64_osStartThread_impl(rdram, ctx, true);
}

static void wr64_osStartThread_impl(uint8_t* rdram, recomp_context* ctx, bool allow_defer) {
    const uint32_t thread_vaddr = static_cast<uint32_t>(ctx->r4);
    uint32_t entry_pc = 0;
    uint32_t priority = 0;
    if (is_kseg0_rdram_pointer(thread_vaddr)) {
        priority = n64_mem32(rdram, thread_vaddr + 0x4);
        const auto it = g_thread_entry_by_vaddr.find(thread_vaddr);
        if (it != g_thread_entry_by_vaddr.end()) {
            entry_pc = it->second;
        }
    }
    ultramodern::boot_log(
        "[wr64] osStartThread thread=0x%08X entry=0x%08X pri=%u%s\n",
        thread_vaddr,
        entry_pc,
        priority,
        allow_defer ? "" : " (force)");
    if (entry_pc == kViManagerEntry) {
        init_libultra_vi_state(rdram);
        ensure_vi_event_queue(rdram);
        ensure_libultra_timer_list(rdram);
        ensure_libultra_virt_count(rdram);
        if (n64_mem32(rdram, kSeg800F - 0x6F50) == 0) {
            init_libultra_timer_root(rdram);
        }
    }
    recomp::invoke_thread_create_callback(rdram, ctx);

    // Defer VI/PI manager threads during Main_IdleThread so boot can finish thread setup.
    const PTR(OSThread) self = ultramodern::this_thread();
    if (allow_defer && (entry_pc == kViManagerEntry || entry_pc == kPiManagerEntry) && self != NULLPTR &&
        is_kseg0_rdram_pointer(thread_vaddr)) {
        const uint32_t self_vaddr = static_cast<uint32_t>(self);
        const uint32_t target_pri = n64_mem32(rdram, thread_vaddr + 0x4);
        const uint32_t running_pri =
            is_kseg0_rdram_pointer(self_vaddr) ? n64_mem32(rdram, self_vaddr + 0x4) : 255u;
        if (target_pri <= running_pri) {
            ultramodern::boot_log(
                "[wr64] osStartThread: defer %s (target pri %u <= running pri %u)\n",
                entry_pc == kViManagerEntry ? "VI manager" : "PI manager",
                target_pri,
                running_pri);
            g_deferred_libultra_threads.push_back(thread_vaddr);
            return;
        }
    }

    if (entry_pc == kMainThreadEntry) {
        ultramodern::boot_log("[wr64] osStartThread: starting Main_Thread (0x80047530)\n");
    } else if (entry_pc == kSysMainThreadEntry) {
        ultramodern::boot_log("[wr64] osStartThread: starting SysMain_Thread (0x80046DA0)\n");
    } else if (entry_pc == kAudioThreadEntry) {
        ultramodern::boot_log("[wr64] osStartThread: starting Audio thread (0x80047B20)\n");
    } else if (entry_pc == kViManagerEntry) {
        g_vi_manager_started.store(true, std::memory_order_release);
    }

    osStartThread(rdram, static_cast<int32_t>(ctx->r4));
}

extern "C" void wr64_osContInit_recomp(uint8_t* rdram, recomp_context* ctx) {
    osContInit_recomp(rdram, ctx);
    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    if (n <= 8) {
        const uint32_t mq = static_cast<uint32_t>(ctx->r4);
        const uint8_t mask =
            is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))
                ? n64_mem8(rdram, static_cast<uint32_t>(ctx->r5))
                : 0u;
        ultramodern::boot_log(
            "[wr64] osContInit #%u mq=0x%08X mask=0x%02X ret=%lld\n",
            n,
            mq,
            mask,
            static_cast<long long>(ctx->r2));
    }
}

extern "C" void wr64_osContStartReadData_recomp(uint8_t* rdram, recomp_context* ctx) {
    osContStartReadData_recomp(rdram, ctx);
    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    if (n <= 16) {
        ultramodern::boot_log(
            "[wr64] osContStartReadData #%u mq=0x%08X ret=%lld\n",
            n,
            static_cast<uint32_t>(ctx->r4),
            static_cast<long long>(ctx->r2));
    }
}

extern "C" void wr64_osViSwapBuffer_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx);

void register_wr64_function_patches() {
    if (stock_boot_enabled()) {
        return;
    }
    static bool registered = false;
    if (registered) {
        return;
    }
    registered = true;
    recomp::overlays::add_loaded_function(0x800980C8, wr64_static_0_800980C8);
    recomp::mods::patch_function_entry(codeSEG_800C6DE0, wr64_codeSEG_800C6DE0);
    recomp::mods::patch_function_entry(codeSEG_800CB7D0, wr64_codeSEG_800CB7D0);
    recomp::mods::patch_function_entry(codeSEG_800C6420, wr64_osCreateThread_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C6570, wr64_osStartThread_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C6310, wr64_codeSEG_800C6310);
    recomp::mods::patch_function_entry(codeSEG_800C5C60, wr64_bridge_osRecvMesg);
    recomp::mods::patch_function_entry(codeSEG_800C57A0, wr64_bridge_osSendMesg);
    recomp::mods::patch_function_entry(codeSEG_800C63B0, wr64_osViSetEvent_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C5DF0, wr64_codeSEG_800C5DF0);
    recomp::mods::patch_function_entry(codeSEG_800C6770, wr64_osCreateViManager);
    recomp::mods::patch_function_entry(codeSEG_800C6AD0, wr64_osViBlack_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C59B0, wr64_osViSwapBuffer_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C6B40, osViGetCurrentFramebuffer_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C5E60, osViSetSpecialFeatures_recomp);
    recomp::mods::patch_function_entry(codeSEG_800CBF50, wr64_codeSEG_800CBF50);
    recomp::mods::patch_function_entry(codeSEG_800CB9C0, wr64_osPiRawStartDma);
    // osContInit / osContStartReadData — avoid __osSiRawStartDma (same vaddr as PI pi_raw) during SysMain.
    recomp::mods::patch_function_entry(codeSEG_800C7020, wr64_osContInit_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C5A00, wr64_osContStartReadData_recomp);
    recomp::mods::patch_function_entry(codeSEG_800C5AC4, osContGetReadData_recomp);
    // guOrtho/guOrthoF — recompiled COP1 hits NAN_CHECK in the scale loop (func_80091F50).
    recomp::mods::patch_function_entry(codeSEG_800C7380, wr64_guOrthoF);
    recomp::mods::patch_function_entry(codeSEG_800C74D4, wr64_guOrtho);
    // osPiStartDma — game_dma_copy / GameLoad_LoadCodeseg; vanilla path needs PI manager thread.
    recomp::mods::patch_function_entry(codeSEG_800CA370, wr64_osPiStartDma);
    // game_dma_copy — force HLE ROM DMA (SysMain GameLoad + gfx segment loads).
    recomp::mods::patch_function_entry(codeSEG_80097EC8, wr64_codeSEG_80097EC8);
    // GameLoad_LoadOverlay entry (thin wrapper); jump-table body stays stock @ 0x80098240.
    recomp::mods::patch_function_entry(codeSEG_80098208, wr64_codeSEG_80098208);
    recomp::mods::patch_function_entry(codeSEG_80047C38, wr64_codeSEG_80047C38);
    recomp::mods::patch_function_entry(codeSEG_80048854, wr64_codeSEG_80048854);
    recomp::mods::patch_function_entry(codeSEG_8004A130, wr64_codeSEG_8004A130);
    recomp::mods::patch_function_entry(codeSEG_80047EE0, wr64_codeSEG_80047EE0);
    recomp::mods::patch_function_entry(codeSEG_800474E4, wr64_codeSEG_800474E4);
    recomp::mods::patch_function_entry(codeSEG_800B8CB0, wr64_codeSEG_800B8CB0);
    // Audio — thread entry HLE (VI consumer + task pump); n_alSynRemovePlayer stubbed until heap live.
    recomp::mods::patch_function_entry(codeSEG_80047B20, wr64_codeSEG_80047B20);
    recomp::mods::patch_function_entry(codeSEG_80047B00, wr64_codeSEG_80047B00);
    recomp::mods::patch_function_entry(codeSEG_800BF370, wr64_codeSEG_800BF370);
    recomp::mods::patch_function_entry(codeSEG_800C2FDC, wr64_codeSEG_800C2FDC);
    recomp::mods::patch_function_entry(codeSEG_800C3034, wr64_codeSEG_800C3034);
    recomp::overlays::add_loaded_function(0x800C3044, wr64_codeSEG_800C3044);
    // Controller update — avoid SI/overlay races during early SysMain.
    recomp::mods::patch_function_entry(codeSEG_8004A2B4, wr64_codeSEG_8004A2B4);
    // Gfx bootstrap + buffer init — stock 468E0 used D_1000000 then color image; HLE fixes Z-buffer bind.
    recomp::mods::patch_function_entry(codeSEG_800468E0, wr64_codeSEG_800468E0);
    recomp::mods::patch_function_entry(codeSEG_80046D2C, wr64_codeSEG_80046D2C);
    recomp::mods::patch_function_entry(codeSEG_800922E4, wr64_codeSEG_800922E4);
    recomp::mods::patch_function_entry(codeSEG_80092CF0, wr64_codeSEG_80092CF0);
    // osSpTaskLoad / osSpTaskStartGo @ 0x800C615C / 0x800C62BC — route Gfx tasks to RT64.
    recomp::mods::patch_function_entry(codeSEG_800C615C, wr64_osSpTaskLoad);
    recomp::mods::patch_function_entry(codeSEG_800C62BC, wr64_osSpTaskStartGo);
    // osSetEventMesg — register SP/DP/SI events with ultramodern (not libultra BSS tab).
    recomp::mods::patch_function_entry(codeSEG_800C6340, wr64_osSetEventMesg);
    recomp::overlays::add_loaded_function(0x800C6D00, wr64_codeSEG_800C6D00);
    // Internal target of the 0x800CAB50 exception vector stub (not a separate symbol).
    recomp::overlays::add_loaded_function(0x800CAB60, codeSEG_800CAB50);
    recomp::overlays::add_loaded_function(0x800CC370, wr64_pi_manager_dma_handler);
    recomp::overlays::add_loaded_function(0x801EAFB4, wr64_static_1_801EAFB4);
    // boot_dma1 jump-table targets in unrecompiled course/bank RAM (e.g. 0x802D0980).
    recomp::overlays::add_loaded_function(0x801E5470, wr64_boot_dma1_801E5470);
    recomp::overlays::add_loaded_function(0x802D0980, wr64_boot_dma_noop);
    // State-6 jump-table target LOOKUP_FUNC(0x802C5800) — log and route title states to ovl_i3.
    recomp::overlays::add_loaded_function(0x802C5800, wr64_dispatch_802C5800);
    recomp::overlays::add_loaded_function(0x80092654, wr64_jt_80092654);
    recomp::overlays::add_loaded_function(0x80092928, static_0_80092928);
    recomp::mods::patch_function_entry(codeSEG_800926F4, wr64_codeSEG_800926F4);
}

void init_pi_cart_handle(uint8_t* rdram) {
    auto* handle = reinterpret_cast<OSPiHandle*>(n64_vram(rdram, recomp::cart_handle));
    std::memset(handle, 0, sizeof(OSPiHandle));
    handle->type = 0;
    handle->baseAddress = recomp::rom_base | 0xA0000000u;
    handle->domain = 0;
    // Let embedded osCreatePiManager (codeSEG_800CB900) set 0x800E90C0 (pi manager active).
}

// game_dma_copy stores KSEG0 via osK0ToK1 (800C9E20), but PI HLE paths may pass raw phys.
static gpr normalize_dram_vaddr(gpr dram_addr) {
    const uint32_t u = static_cast<uint32_t>(dram_addr);
    if (u >= 0x80000000u) {
        return dram_addr;
    }
    if (u < 0x00800000u) {
        return static_cast<gpr>(static_cast<int32_t>(u | 0x80000000u));
    }
    return dram_addr;
}

// Scratch stack for one-shot HLE calls into recompiled game code (not any live OSThread stack).
constexpr uint32_t kHleInvokeStackTop = 0x80150E00u;

static void wr64_invoke_stock_gfx_bootstrap(uint8_t* rdram) {
    recomp_context ctx{};
    ctx.r29 = kHleInvokeStackTop;
    wr64_codeSEG_800468E0(rdram, &ctx);
}

void wr64_fixup_codeseg_gap_bss(uint8_t* rdram) {
    constexpr uint32_t kGapStart = 0x80227A60u;
    constexpr uint32_t kGapEnd = 0x80228E10u;
    std::memset(n64_vram(rdram, kGapStart), 0, kGapEnd - kGapStart);
}

static bool is_valid_wr64_codeseg_virt(uint32_t vaddr) {
    return vaddr >= 0x80200000u && vaddr < 0x80800000u;
}

static uint32_t wr64_default_codeseg84() {
    return 0x80228E10u;
}

void wr64_seed_segment_globals_if_needed(uint8_t* rdram) {
    constexpr uint32_t kPool = 0x801CE6B0u;
    constexpr uint32_t kGfxChunkAligned = (0x00008290u + 0xFu) & ~0xFu;

    uint32_t seg84 = static_cast<uint32_t>(
        normalize_dram_vaddr(static_cast<gpr>(n64_mem32(rdram, 0x80151984u))));
    if (!is_valid_wr64_codeseg_virt(seg84)) {
        const uint32_t seg70 = static_cast<uint32_t>(
            normalize_dram_vaddr(static_cast<gpr>(n64_mem32(rdram, 0x80151970u))));
        if (is_valid_wr64_codeseg_virt(seg70)) {
            constexpr uint32_t kCodesegStart = 0x801DAFA0u;
            seg84 = seg70 + (wr64_default_codeseg84() - kCodesegStart);
            seg84 = (seg84 + 0xFu) & ~0xFu;
        } else {
            seg84 = wr64_default_codeseg84();
        }
    }
    // gSPSegment(1, D_80151984) reads RAM directly; normalized 0x80228E10 must be stored,
    // not a stripped 0x00228E10 that passes is_valid only after normalize_dram_vaddr().
    if (n64_mem32(rdram, 0x80151984u) != seg84) {
        n64_mem32(rdram, 0x80151984u) = seg84;
    }

    constexpr uint32_t kDefaultSeg8c = 0x802310A0u;
    uint32_t seg8c = static_cast<uint32_t>(
        normalize_dram_vaddr(static_cast<gpr>(n64_mem32(rdram, 0x8015198Cu))));
    if (!is_valid_wr64_codeseg_virt(seg8c)) {
        seg8c = kDefaultSeg8c;
    }
    if (n64_mem32(rdram, 0x8015198Cu) != seg8c) {
        n64_mem32(rdram, 0x8015198Cu) = seg8c;
    }

    uint32_t pool_seg = static_cast<uint32_t>(
        normalize_dram_vaddr(static_cast<gpr>(n64_mem32(rdram, kPool + 4u))));
    if (!is_valid_wr64_codeseg_virt(pool_seg)) {
        n64_mem32(rdram, kPool + 0x00u) = 0;
        n64_mem32(rdram, kPool + 4u) = seg84;
        n64_mem32(rdram, kPool + 0x08u) = 0;
        n64_mem32(rdram, kPool + 0x0Cu) = 0;
        const uint32_t seg_ac = n64_mem32(rdram, 0x801519ACu);
        if (seg_ac != 0) {
            n64_mem32(rdram, kPool + 0x10u) = seg_ac;
        }
        pool_seg = seg84;
    } else if (n64_mem32(rdram, kPool + 4u) != pool_seg) {
        n64_mem32(rdram, kPool + 4u) = pool_seg;
    }

    const uint32_t expected_gfx = seg84 + kGfxChunkAligned;
    if (n64_mem32(rdram, 0x800DCE90u) != expected_gfx) {
        n64_mem32(rdram, 0x800DCE90u) = expected_gfx;
    }

    static uint32_t last_seg84 = 0;
    static uint32_t last_pool = 0;
    static uint32_t last_gfx = 0;
    const uint32_t cur_seg84 = n64_mem32(rdram, 0x80151984u);
    const uint32_t cur_pool = n64_mem32(rdram, kPool + 4u);
    const uint32_t cur_gfx = n64_mem32(rdram, 0x800DCE90u);
    if (cur_seg84 != last_seg84 || cur_pool != last_pool || cur_gfx != last_gfx) {
        last_seg84 = cur_seg84;
        last_pool = cur_pool;
        last_gfx = cur_gfx;
        ultramodern::boot_log(
            "[wr64] seed globals: D_80151984=0x%08X D_801CE6B0[1]=0x%08X D_800DCE90=0x%08X\n",
            cur_seg84,
            cur_pool,
            cur_gfx);
    }
    wr64_preload_boot_prog_overlay(rdram);
    wr64_fixup_scene_gfx_pointer(rdram);
    wr64_seed_dl_ptr_table_if_needed(rdram);
}

static void wr64_maybe_force_boot_transition(uint8_t* rdram) {
    const char* force = std::getenv("WR64_FORCE_BOOT_TRANSITION");
    if (force == nullptr || force[0] == '\0' || force[0] == '0') {
        return;
    }
    if (n64_mem32(rdram, kOverlayLoadGate) != 0) {
        return;
    }
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    recomp_context ctx{};
    ctx.r29 = kHleInvokeStackTop;
    segment_1B1FB0_802C7510(rdram, &ctx);
    wr64_log_boot_progress(rdram, "forced-boot-transition");
    ultramodern::boot_log(
        "[wr64] WR64_FORCE_BOOT_TRANSITION: invoked segment_1B1FB0_802C7510\n");
}

void wr64_invoke_post_gfx_init(uint8_t* rdram) {
    static std::atomic<bool> done{false};
    if (done.exchange(true)) {
        return;
    }
    if (const char* skip = std::getenv("WR64_SKIP_POST_GFX_INIT")) {
        if (skip[0] != '\0' && skip[0] != '0') {
            ultramodern::boot_log(
                "[wr64] post-Gfx#1: skipped (WR64_SKIP_POST_GFX_INIT set)\n");
            return;
        }
    }
    wr64_fixup_codeseg_gap_bss(rdram);
    wr64_seed_segment_globals_if_needed(rdram);
    wr64_seed_dl_ptr_table_if_needed(rdram);
    wr64_fixup_scene_gfx_pointer(rdram);
    wr64_preload_boot_prog_overlay(rdram);
    wr64_apply_boot_display_mode_if_needed(rdram);
    wr64_log_boot_progress(rdram, "post-Gfx#1");
    ultramodern::boot_log(
        "[wr64] post-Gfx#1: seeded segment globals + DL ptr table + boot prog overlay\n");
    wr64_maybe_force_boot_transition(rdram);
}

uint32_t resolve_cart_physical(uint8_t* rdram, const OSIoMesg& mb) {
    uint32_t physical = mb.devAddr & 0x1FFFFFFFu;
    if (physical >= recomp::rom_base) {
        return physical;
    }
    if (mb.piHandle != 0) {
        const uint32_t handle_vaddr = static_cast<uint32_t>(mb.piHandle);
        if (is_kseg0_rdram_pointer(handle_vaddr)) {
            const auto* handle = reinterpret_cast<const OSPiHandle*>(n64_vram(rdram, handle_vaddr));
            physical = (handle->baseAddress | mb.devAddr) & 0x1FFFFFFFu;
            if (physical >= recomp::rom_base) {
                return physical;
            }
        }
    }
    return recomp::rom_base + (mb.devAddr & 0x0FFFFFFFu);
}

void init_libultra_mmio_stubs(uint8_t* rdram) {
    auto map_kseg1 = [&](uint32_t vaddr) -> uint8_t* {
        return wr64_rdram_byte_ptr(rdram, vaddr);
    };
    // PI @ 0xA4400000 (codeSEG_800CB7D0 / 800CBF50) and SP status @ 0xA4040000.
    std::memset(map_kseg1(0xA4400000u), 0, 0x20);
    std::memset(map_kseg1(0xA4040000u), 0, 0x20);
    // PI extended regs @ 0xA4600000 (codeSEG_800CC370 raw DMA path).
    std::memset(map_kseg1(0xA4600000u), 0, 0x20);
}

void wr64_game_create_mesg_queue(uint8_t* rdram, uint32_t mq, uint32_t msg_buf, int32_t count) {
    recomp_context ctx{};
    ctx.r4 = mq;
    ctx.r5 = msg_buf;
    ctx.r6 = count;
    wr64_codeSEG_800C6310(rdram, &ctx);
}

void wr64_ensure_audio_mesg_queue_pointers(uint8_t* rdram) {
    auto ensure_ptr = [&](uint32_t ptr_slot, uint32_t queue_obj) {
        uint32_t& ptr = n64_mem32(rdram, ptr_slot);
        if (!is_kseg0_rdram_pointer(ptr)) {
            ptr = queue_obj;
        }
    };
    ensure_ptr(kPtrAudioTaskStartQueue, kAudioTaskStartQueueObj);
    ensure_ptr(kPtrThreadCmdProcQueue, kThreadCmdProcQueueObj);
    ensure_ptr(kPtrAudioSpecQueue, kAudioSpecQueueObj);
    ensure_ptr(kPtrAudioResetQueue, kAudioResetQueueObj);
}

void wr64_nudge_audio_vi_queue(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kBootMqVi)) {
        return;
    }
    ultramodern::deliver_mesg_immediate(
        rdram,
        static_cast<PTR(OSMesgQueue)>(kBootMqVi),
        static_cast<OSMesg>(0x1Fu));
}

void wr64_try_audio_load_init(uint8_t* rdram) {
    static std::atomic<bool> requested{false};
    if (requested.exchange(true)) {
        return;
    }
    g_wr64_audio_init_pending.store(true, std::memory_order_release);
    ultramodern::boot_log("[wr64] AudioLoad_Init scheduled (first Gfx RSP task)\n");
    wr64_nudge_audio_vi_queue(rdram);
}

void ensure_pi_aux_mesg_queues(uint8_t* rdram) {
    auto init_if_needed = [&](uint32_t mq, uint32_t msg_buf, s32 count) {
        if (!is_kseg0_rdram_pointer(mq) || !is_kseg0_rdram_pointer(msg_buf)) {
            return;
        }
        const int32_t msg_count = static_cast<int32_t>(n64_mem32(rdram, mq + 0x10u));
        const uint32_t existing_buf = n64_mem32(rdram, mq + 0x14u);
        if (msg_count != 0 && existing_buf == msg_buf) {
            return;
        }
        if (mq == kPiCmdQueue && existing_buf == kPiCmdMesgBufStock) {
            msg_buf = kPiCmdMesgBufSafe;
        }
        wr64_game_create_mesg_queue(rdram, mq, msg_buf, count);
        ultramodern::boot_log(
            "[wr64] pi_manager: created mesg queue mq=0x%08X msg=0x%08X count=%d\n",
            mq,
            msg_buf,
            count);
    };
    // kPiSecondaryMesgQueue is created by main (codeSEG_80047530); do not touch it here.
    init_if_needed(kPiEventsMesgQueue, kPiEventsMesgBuf, 1);
    // osCreatePiManager cmd queue — relocate msg ring away from main-thread queue structs.
    init_if_needed(kPiCmdQueue, kPiCmdMesgBufSafe, 0x40);
}

void ensure_pi_manager_globals(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kPiManagerGlobal)) {
        return;
    }
    ensure_pi_aux_mesg_queues(rdram);

    uint32_t& cmd_q = n64_mem32(rdram, kPiManagerGlobal + 0x8u);
    uint32_t& stack_q = n64_mem32(rdram, kPiManagerGlobal + 0xCu);
    uint32_t& access_q = n64_mem32(rdram, kPiManagerGlobal + 0x10u);
    uint32_t& handler = n64_mem32(rdram, kPiManagerGlobal + 0x14u);

    if (cmd_q == 0) {
        cmd_q = kPiCmdQueue;
    }
    if (stack_q == 0) {
        stack_q = kSeg801E - 0x54D0u; // PI manager thread stack mesg queue (0x801DAB30)
    }
    if (access_q == 0) {
        access_q = kSeg801E - 0x5368u; // 0x801DAC98
    }
    if (handler != kPiManagerHandler) {
        ultramodern::boot_log(
            "[wr64] pi_manager: handler 0x%08X -> 0x%08X\n",
            handler,
            kPiManagerHandler);
        handler = kPiManagerHandler;
    }
    if (n64_mem32(rdram, kPiManagerActiveFlag) == 0) {
        n64_mem32(rdram, kPiManagerActiveFlag) = 1u;
    }
}

void log_pi_io_message(uint8_t* rdram, uint32_t mesg_vaddr) {
    if (!is_kseg0_rdram_pointer(mesg_vaddr)) {
        return;
    }
    const uint16_t type = static_cast<uint16_t>(n64_mem32(rdram, mesg_vaddr) >> 16);
    const uint32_t w0 = n64_mem32(rdram, mesg_vaddr);
    const uint32_t w1 = n64_mem32(rdram, mesg_vaddr + 4u);
    const uint32_t w2 = n64_mem32(rdram, mesg_vaddr + 8u);
    const uint32_t w3 = n64_mem32(rdram, mesg_vaddr + 0xCu);
    ultramodern::boot_log(
        "[wr64] pi_io_mesg @0x%08X type=%u words=[0x%08X 0x%08X 0x%08X 0x%08X]\n",
        mesg_vaddr,
        type,
        w0,
        w1,
        w2,
        w3);
}

bool is_timer_list_head_valid(uint8_t* rdram, uint32_t head) {
    if (!is_kseg0_rdram_pointer(head)) {
        return false;
    }
    const uint32_t next = n64_mem32(rdram, head);
    const uint32_t prev = n64_mem32(rdram, head + 4);
    return next == head || prev == head;
}

// VI manager BSS: only seed timer tables; viEventQueue is owned by osCreateViManager.
void init_libultra_vi_state(uint8_t* rdram) {
    std::memset(n64_vram(rdram, kViThreadData), 0, 0x200);

    const uint32_t tick = osGetCount();
    auto mem32 = [&](uint32_t vaddr) -> uint32_t& { return n64_mem32(rdram, vaddr); };
    auto init_handler_table = [&](uint32_t handler) {
        std::memset(n64_vram(rdram, handler), 0, 0x80);
        mem32(handler + 0x4) = tick;
        mem32(handler + 0x28) = tick;
        mem32(handler + 0x3C) = tick;
        std::memcpy(n64_vram(rdram, handler + 0x14), n64_vram(rdram, handler), 0x14);
    };
    init_handler_table(kTimerHandlerNtsc);
    init_handler_table(kTimerHandlerPal);
}

void ensure_libultra_timer_list(uint8_t* rdram) {
    constexpr uint32_t kTimerCtrl = kSeg800F - 0x71C0u;

    const uint32_t list_head = n64_mem32(rdram, kSeg800F - 0x6FC0);
    if (is_timer_list_head_valid(rdram, list_head)) {
        return;
    }

    std::memset(n64_vram(rdram, kTimerCtrl), 0, 0x20);
    n64_mem32(rdram, kTimerCtrl) = kTimerCtrl;
    n64_mem32(rdram, kTimerCtrl + 4) = kTimerCtrl;
    n64_mem32(rdram, kSeg800F - 0x6FC0) = kTimerCtrl;
}

bool is_valid_virt_count_node(uint8_t* rdram, uint32_t node) {
    // BSS often contains 0x80000000 which passes a naive kseg0 check but faults on +0x4.
    if (node < 0x800E8000u || node >= 0x80800000u) {
        return false;
    }
    const uint16_t node_type =
        *reinterpret_cast<uint16_t*>(n64_vram(rdram, node + 0x10));
    return node_type == 4;
}

// Mirrors osInitialize @0x800C6E8C: copy IPL3 data at 0x800CAB50 into low RDRAM vectors.
void copy_osinit_ipl3_tables(uint8_t* rdram) {
    constexpr uint32_t kIpl3 = 0x800D0000u - 0x54B0u;
    constexpr uint32_t kDst0 = 0x80000000u;
    constexpr uint32_t kDst1 = 0x80000080u;
    constexpr uint32_t kDst2 = 0x80000100u;
    constexpr uint32_t kDst3 = 0x80000180u;
    for (uint32_t off = 0; off < 0x10u; off += 4u) {
        n64_mem32(rdram, kDst0 + off) = n64_mem32(rdram, kIpl3 + off);
        n64_mem32(rdram, kDst1 + off) = n64_mem32(rdram, kIpl3 + 0x10u + off);
        n64_mem32(rdram, kDst2 + off) = n64_mem32(rdram, kIpl3 + off);
        n64_mem32(rdram, kDst3 + off) = n64_mem32(rdram, kIpl3 + 0x10u + off);
    }
}

void init_virt_count_node(uint8_t* rdram, uint32_t node) {
    std::memset(n64_vram(rdram, node), 0, 0x30);
    n64_mem32(rdram, node + 0x4) = osGetCount();
    *reinterpret_cast<uint16_t*>(n64_vram(rdram, node + 0x10)) = 4;
    n64_mem32(rdram, node + 0x0) = 0;
}

void ensure_libultra_virt_count(uint8_t* rdram) {
    // libultra osInitialize / __osSetTimer walk these globals (loadCodeSEG ~0x800CB030):
    //   -0x6FF8: virt-count freelist head
    //   -0x6FF0: active virt-count node pointer
    constexpr uint32_t kVirtFreelistHead = kSeg800F - 0x6FF8u;
    constexpr uint32_t kVirtActiveNode = kSeg800F - 0x6FF0u;
    constexpr uint32_t kVirtCountNode = kSeg800F - 0x7180u;

    uint32_t node = n64_mem32(rdram, kVirtActiveNode);
    if (!is_valid_virt_count_node(rdram, node)) {
        init_virt_count_node(rdram, kVirtCountNode);
        n64_mem32(rdram, kVirtActiveNode) = kVirtCountNode;
        n64_mem32(rdram, kVirtFreelistHead) = kVirtCountNode;
        return;
    }

    n64_mem32(rdram, node + 0x4) = osGetCount();

    const uint32_t freelist = n64_mem32(rdram, kVirtFreelistHead);
    if (!is_valid_virt_count_node(rdram, freelist)) {
        n64_mem32(rdram, kVirtFreelistHead) = node;
    }
}

void init_libultra_timer_root(uint8_t* rdram) {
    constexpr uint32_t kSetTimerRoot = kSeg800F - 0x6FB0u;
    constexpr uint32_t kSetTimerAux = kSetTimerRoot + 0x30u;

    auto mem32 = [&](uint32_t vaddr) -> uint32_t& { return n64_mem32(rdram, vaddr); };
    auto mem16 = [&](uint32_t vaddr) -> uint16_t& {
        return *reinterpret_cast<uint16_t*>(n64_vram(rdram, vaddr));
    };

    const uint32_t tick = osGetCount();
    std::memset(n64_vram(rdram, kSetTimerRoot), 0, 0x60);
    mem32(kSetTimerRoot + 0x4) = tick;
    mem16(kSetTimerRoot + 0x2) = 1;
    mem16(kSetTimerAux + 0x32) = 1;

    mem32(kSeg800F - 0x6F50) = kSetTimerRoot;
    mem32(kSeg800F - 0x6F4C) = kSetTimerAux;
    mem32(kSeg800F - 0x7070) = 0;

    const uint32_t os_tv_type = mem32(0x80000300u);
    mem32(kSeg800F - 0x6F48) = os_tv_type != 0 ? os_tv_type : 1u;

    if (os_tv_type == 1) {
        mem32(kSetTimerAux + 0x8) = kSeg800F - 0x6EB0u;
        mem32(kSeg800F - 0x6F44) = 0x02E6D354u;
    } else {
        mem32(kSetTimerAux + 0x8) = kSeg800F - 0x6E60u;
        mem32(kSeg800F - 0x6F44) = 0x02E6025Cu;
    }
    mem16(kSetTimerAux + 0x0) = 0x20;
}

// Libultra globals in the 0x800F0000 segment.
void init_libultra_timer_globals(uint8_t* rdram, bool full_boot_init) {
    std::lock_guard lock(g_timer_path_mutex);
    init_libultra_mmio_stubs(rdram);

    if (full_boot_init) {
        init_pi_cart_handle(rdram);
        init_libultra_vi_state(rdram);
    }

    const uint32_t timer_root = n64_mem32(rdram, kSeg800F - 0x6F50);
    if (timer_root == 0) {
        init_libultra_timer_root(rdram);
    } else if (is_kseg0_rdram_pointer(timer_root)) {
        n64_mem32(rdram, timer_root + 0x4) = osGetCount();
    }

    ensure_libultra_timer_list(rdram);
    ensure_libultra_virt_count(rdram);
    restore_timer_handler_fields(rdram);
}

void pulse_vi_and_pi(uint8_t* rdram) {
    if (rdram == nullptr || !ultramodern::is_game_started()) {
        return;
    }
    if (!g_entrypoint_complete.load(std::memory_order_acquire)) {
        return;
    }
    if (!g_vi_init_complete.load(std::memory_order_acquire)) {
        return;
    }
    if (!g_allow_vi_pulse.load(std::memory_order_acquire)) {
        return;
    }
    if (!g_main_thread_mq_ready.load(std::memory_order_acquire)) {
        return;
    }
    if (!g_vi_manager_started.load(std::memory_order_acquire)) {
        return;
    }

    const uint32_t vi_queue = vi_mesg_queue_addr(rdram);
    if (!is_kseg0_rdram_pointer(vi_queue)) {
        return;
    }
    {
        const auto* mq = reinterpret_cast<const OSMesgQueue*>(n64_vram(rdram, vi_queue));
        // One VI per host frame: do not stack vblanks faster than the game consumes them.
        if (mq->msgCount <= 0 || mq->validCount > 0) {
            return;
        }
    }
    static bool odd_field = false;
    odd_field = !odd_field;
    const OSMesg msg = odd_field ? static_cast<OSMesg>(static_cast<int32_t>(kViRetraceMesg)) :
                                   static_cast<OSMesg>(static_cast<int32_t>(kViCounterMesg));
    static std::atomic<uint32_t> vi_pulse_count{0};
    const uint32_t pulse_n = vi_pulse_count.fetch_add(1) + 1;
    if (pulse_n <= 16) {
        ultramodern::boot_log(
            "[wr64] vi_pulse #%u queue=0x%08X msg=0x%08X (hardware inject)\n",
            pulse_n,
            vi_queue,
            static_cast<uint32_t>(static_cast<intptr_t>(msg)));
        log_mesg_queue_state("vi_pulse before", rdram, vi_queue);
    }
    // Events thread: lock-free RDRAM write (hardware VI), then async host wake.
    ultramodern::deliver_mesg_immediate(rdram, vi_queue, msg);
    if (pulse_n <= 16) {
        log_mesg_queue_state("vi_pulse after", rdram, vi_queue);
    }

    uint32_t& pi_status = n64_mem32(rdram, kPiStatusReg);
    pi_status |= 1u;
}

static std::atomic<bool> g_wr64_rt64_workload_busy{false};

static bool wr64_gfx_hw_pipeline_busy(uint8_t* rdram) {
    if (g_wr64_rt64_workload_busy.load(std::memory_order_acquire)) {
        return true;
    }
    if (n64_mem32(rdram, kGfxTaskActiveGlobal) != 0u) {
        return true;
    }
    if (n64_mem32(rdram, kGfxTaskDpPendingGlobal) != 0u) {
        return true;
    }
    return false;
}

static bool wr64_gfx_pipeline_busy_impl(uint8_t* rdram) {
    if (wr64_gfx_hw_pipeline_busy(rdram)) {
        return true;
    }
    return wr64_main_mq_has_gfx_task_pending(rdram);
}

static void wr64_block_until_gfx_idle(uint8_t* rdram) {
    // Only wait for in-flight SP/DP/RT64 work. A gfx-task message queued on Main's
    // mq is normal at the top of SysMain's loop (stock does not drain it before 0x29).
    // Running Main from inside SysMain's osRecvMesg to dequeue that message reenters
    // the title path and has crashed after frame 3.
    while (wr64_gfx_hw_pipeline_busy(rdram)) {
        ultramodern::process_deferred_rsp_completions(rdram);
        ultramodern::run_next_thread_and_wait(rdram);
        std::this_thread::yield();
    }
}

} // namespace

static std::atomic<bool> g_nonempty_swapchain_presented{false};
static std::atomic<uint32_t> g_skip_update_screen_present_count{0};

void wr64::notify_nonempty_swapchain_present() {
    g_nonempty_swapchain_presented.store(true, std::memory_order_release);
    recompui::dismiss_game_loading_overlay();
}

bool wr64::nonempty_swapchain_presented() {
    return g_nonempty_swapchain_presented.load(std::memory_order_acquire);
}

void wr64::reset_nonempty_swapchain_presented() {
    g_nonempty_swapchain_presented.store(false, std::memory_order_release);
}

void wr64::set_skip_next_update_screen_present(bool skip) {
    if (skip) {
        // Gfx send_dl can be followed by several VI 0x19 updateScreen() calls.
        g_skip_update_screen_present_count.fetch_add(4u, std::memory_order_release);
    }
}

bool wr64::consume_skip_next_update_screen_present() {
    uint32_t count = g_skip_update_screen_present_count.load(std::memory_order_acquire);
    while (count > 0u) {
        if (g_skip_update_screen_present_count.compare_exchange_weak(
                count, count - 1u, std::memory_order_acq_rel, std::memory_order_acquire)) {
            return true;
        }
    }
    return false;
}

void wr64::set_gfx_workload_busy(bool busy) {
    g_wr64_rt64_workload_busy.store(busy, std::memory_order_release);
}

bool wr64::gfx_pipeline_busy(uint8_t* rdram) {
    return wr64_gfx_pipeline_busy_impl(rdram);
}

void wr64::wait_hw_gfx_idle(uint8_t* rdram) {
    uint32_t waits = 0;
    uint32_t busy_spins = 0;
    while (wr64_gfx_hw_pipeline_busy(rdram)) {
        // While RT64 send_dl is copying RDRAM framebuffers, do not run other game
        // threads — they can touch the same buffers and fault (e.g. rdram+0x100319fe0).
        if (!g_wr64_rt64_workload_busy.load(std::memory_order_acquire)) {
            busy_spins = 0;
            ultramodern::process_deferred_rsp_completions(rdram);
            ultramodern::run_next_thread_and_wait(rdram);
        } else {
            ++busy_spins;
            if (busy_spins >= 4096u) {
                ultramodern::boot_log(
                    "[wr64] wait_hw_gfx_idle: clearing stuck rt64_workload_busy after %u spins\n",
                    busy_spins);
                g_wr64_rt64_workload_busy.store(false, std::memory_order_release);
                busy_spins = 0;
            } else {
                std::this_thread::yield();
            }
        }
        if (++waits <= 16u) {
            ultramodern::boot_log(
                "[wr64] wait_hw_gfx_idle #%u active=%u dp=%u rt64=%u\n",
                waits,
                n64_mem32(rdram, kGfxTaskActiveGlobal),
                n64_mem32(rdram, kGfxTaskDpPendingGlobal),
                g_wr64_rt64_workload_busy.load(std::memory_order_relaxed) ? 1u : 0u);
        }
    }
}

void wr64::finish_gfx_task(uint8_t* rdram) {
    g_wr64_rt64_workload_busy.store(false, std::memory_order_release);
    n64_mem32(rdram, kGfxTaskActiveGlobal) = 0u;
    n64_mem32(rdram, kGfxTaskDpPendingGlobal) = 0u;
    n64_mem32(rdram, 0x800D4600u) = 0u;
}

void wr64::ensure_host_vi_visible(uint8_t* rdram) {
    wr64_ensure_host_vi_visible_impl(rdram);
}

void wr64::log_framebuffer_sample(uint8_t* rdram, uint32_t fb, uint32_t tag) {
    wr64_log_framebuffer_sample(rdram, fb, tag);
}

static std::atomic<uint32_t> g_last_mode3_draw_phys{0};
static std::atomic<uint32_t> g_last_mode3_draw_width{0};

uint32_t wr64::mode3_host_vi_origin_for_color_fb(uint8_t* rdram, uint32_t color_phys, uint32_t width_pixels) {
    (void)rdram;
    uint32_t width = width_pixels;
    if (width == 0u) {
        const ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        width = 320u;
        if (vi != nullptr) {
            width = vi->VI_WIDTH_REG & 0xFFFu;
            if (width == 0u) {
                width = 320u;
            }
        }
    }
    // RT64 VI::fbAddress() subtracts one 16bpp row (width * 2) from VI origin.
    return (color_phys & 0x00FFFFFFu) + width * 2u;
}

void wr64::set_last_mode3_draw_phys(uint32_t rdp_color_phys) {
    g_last_mode3_draw_phys.store(rdp_color_phys & 0x00FFFFFFu, std::memory_order_release);
}

uint32_t wr64::last_mode3_draw_phys() {
    return g_last_mode3_draw_phys.load(std::memory_order_acquire);
}

void wr64::set_last_mode3_draw_width(uint32_t rdp_color_width) {
    g_last_mode3_draw_width.store(rdp_color_width, std::memory_order_release);
}

uint32_t wr64::last_mode3_draw_width() {
    return g_last_mode3_draw_width.load(std::memory_order_acquire);
}

uint32_t wr64::mode3_present_color_phys(uint8_t* rdram, uint32_t rdp_color_phys) {
    (void)rdram;
    // Always present the RDP color buffer for this workload. Sibling-buffer substitution
    // (rdp=0x002D4000 -> 0x0036A000) showed stale DMA stripes as colourful garbage.
    return rdp_color_phys & 0x00FFFFFFu;
}

void wr64::sync_host_vi_to_mode3_draw(uint8_t* rdram, uint32_t rdp_color_phys, uint32_t rdp_color_width) {
    if (n64_mem32(rdram, kDisplayModeGlobal) != 3u) {
        return;
    }
    wr64_seed_display_mode_fbs_if_needed(rdram);
    uint32_t phys = rdp_color_phys & 0x00FFFFFFu;
    if (phys == 0u) {
        return;
    }
    const uint32_t fb0_phys = n64_mem32(rdram, kDisplayModeFbArray + 0u) & 0x00FFFFFFu;
    const uint32_t fb1_phys = n64_mem32(rdram, kDisplayModeFbArray + 4u) & 0x00FFFFFFu;
    if (phys != fb0_phys && phys != fb1_phys) {
        return;
    }
    phys = wr64::mode3_present_color_phys(rdram, phys);
    const uint32_t fb_kseg0 = 0x80000000u | phys;
    recomp_context swap_ctx{};
    swap_ctx.r4 = fb_kseg0;
    osViSwapBuffer_recomp(rdram, &swap_ctx);
    ultramodern::commit_vi_swap();
    const uint32_t origin = mode3_host_vi_origin_for_color_fb(rdram, phys, rdp_color_width);
    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    if (vi != nullptr) {
        vi->VI_ORIGIN_REG = origin;
        n64_mem32(rdram, kViMmioBase + 0x00u) = vi->VI_STATUS_REG;
        n64_mem32(rdram, kViMmioBase + 0x04u) = origin;
        n64_mem32(rdram, kViMmioBase + 0x08u) = vi->VI_WIDTH_REG;
        n64_mem32(rdram, kViMmioBase + 0x24u) = vi->VI_H_START_REG;
    }
    wr64_ensure_host_vi_visible_impl(rdram);
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 16u) {
        ultramodern::boot_log(
            "[wr64] mode3 VI present sync color=0x%08X origin=0x%08X width=%u\n",
            fb_kseg0,
            origin,
            rdp_color_width);
    }
}

void wr64::commit_mode3_draw_framebuffer(uint8_t* rdram, uint32_t rdp_color_phys) {
    if (n64_mem32(rdram, kDisplayModeGlobal) != 3u) {
        return;
    }
    wr64_seed_display_mode_fbs_if_needed(rdram);
    const uint32_t phys = rdp_color_phys & 0x00FFFFFFu;
    if (phys == 0u) {
        return;
    }
    const uint32_t fb0_phys = n64_mem32(rdram, kDisplayModeFbArray + 0u) & 0x00FFFFFFu;
    const uint32_t fb1_phys = n64_mem32(rdram, kDisplayModeFbArray + 4u) & 0x00FFFFFFu;
    if (phys != fb0_phys && phys != fb1_phys) {
        return;
    }
    // Do not call osViSwapBuffer here. The game swaps front/back on the Main thread;
    // forcing a swap from Gfx/present paths races message queues and corrupts state.
    const uint32_t origin = mode3_host_vi_origin_for_color_fb(rdram, phys);
    ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
    if (vi != nullptr) {
        vi->VI_ORIGIN_REG = origin;
        n64_mem32(rdram, kViMmioBase + 0x04u) = origin;
    }
    wr64_ensure_host_vi_visible_impl(rdram);
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 16u) {
        const uint32_t cur = static_cast<uint32_t>(osViGetCurrentFramebuffer());
        ultramodern::boot_log(
            "[wr64] mode3 VI commit color=0x%08X origin=0x%08X (osVi was 0x%08X)\n",
            0x80000000u | phys,
            origin,
            cur);
    }
}

bool hle_pi_cmd_queue_mesg(uint8_t* rdram, uint32_t mesg_vaddr);
bool wr64_pi_mgr_queue(uint32_t queue);
bool grant_bss_pi_access(uint8_t* rdram);
void ensure_bootstrap_timer_message(uint8_t* rdram);
void ensure_bootstrap_main_sync_message(uint8_t* rdram);
void ensure_sysmain_event_message(uint8_t* rdram);
bool wr64_mesg_recv_fast_queue(uint32_t queue);
bool wr64_try_grant_stack_pi_access(uint8_t* rdram, uint32_t queue);
bool wr64_mesg_recv_fast(uint8_t* rdram, recomp_context* ctx);
bool wr64_mesg_send_noblock_fast(uint8_t* rdram, recomp_context* ctx);

extern "C" void wr64_on_enter_800C5C60(uint8_t* rdram, recomp_context* ctx);
extern "C" void wr64_on_sp_task_started(uint8_t* rdram, gpr task_ptr);

void ensure_pi_device_ready(uint8_t* rdram, recomp_context* ctx);

extern "C" void wr64_bridge_osRecvMesg(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> recv_count{0};
    const uint32_t n = recv_count.fetch_add(1) + 1;
    const uint32_t queue = static_cast<uint32_t>(ctx->r4);
    if (!is_kseg0_rdram_pointer(queue)) {
        ultramodern::boot_log( "[wr64] osRecvMesg: invalid queue 0x%08X\n", queue);
        ctx->r2 = -1;
        return;
    }
    sanitize_mesg_queue_heads(rdram, queue);
    if (is_boot_mesg_queue(queue)) {
        wr64_ensure_boot_scheduler_queues(rdram);
    }
    if (queue == kMainThreadMesgQueue) {
        ensure_main_thread_mesg_queue(rdram);
    }
    const int32_t flags = static_cast<int32_t>(ctx->r6);
    const bool trace = trace_mesg_queue(queue, n);

    if (trace) {
        ultramodern::boot_log(
            "[wr64] osRecvMesg enter #%u queue=0x%08X flags=%d msg_out=0x%08X\n",
            n,
            queue,
            flags,
            static_cast<uint32_t>(ctx->r5));
        log_mesg_queue_state("RECV enter", rdram, queue);
    }

    // Timer/queue fixups only while mesg_queue_mutex is free; must not span a blocking recv.
    wr64_on_enter_800C5C60(rdram, ctx);
    if (queue == kMainThreadMesgQueue) {
        ensure_main_thread_mesg_queue(rdram);
        sanitize_mesg_queue_heads(rdram, queue);
    }

    const int depth_before = mesg_queue_depth(rdram, queue);
    const auto log_recv_fast_exit = [&]() {
        if (!trace) {
            return;
        }
        uint32_t received = 0;
        if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
            received = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
        }
        const char* path = (queue == kBootMqVi) ? "recv_audio" : "recv_fast";
        ultramodern::boot_log(
            "[wr64] osRecvMesg exit  #%u queue=0x%08X ret=%lld msg=0x%08X (%s)\n",
            n,
            queue,
            static_cast<long long>(ctx->r2),
            received,
            path);
        log_mesg_queue_state("RECV exit", rdram, queue);
    };
    const auto finish_recv_fast = [&]() {
        log_recv_fast_exit();
    };

    // Main_Thread VI loop: pace to host frames and run SysMain/audio between retraces.
    if (queue == kMainThreadMesgQueue && flags == OS_MESG_BLOCK) {
        ensure_bootstrap_timer_message(rdram);
        if (wr64_mesg_recv_fast(rdram, ctx)) {
            finish_recv_fast();
            uint32_t received = 0;
            if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
                received = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
            }
            if (received == 0x19u) {
                wr64_main_handle_vi_message(rdram, received);
            }
            return;
        }
        while (!wr64_mesg_recv_fast(rdram, ctx)) {
            if (wr64_host_hle_should_stop()) {
                ctx->r2 = -1;
                return;
            }
            static uint32_t block_iter = 0;
            wr64_log_recv_block(rdram, queue, static_cast<uint32_t>(ctx->r31), ++block_iter);
            ultramodern::drain_external_messages(rdram);
            ultramodern::process_deferred_rsp_completions(rdram);
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            // Gfx in flight: process deferred completions, then cooperatively yield.
            // Avoid check_running_queue here — preempting Main while SysMain mutates
            // the same DL pools has caused RDRAM races on the title screen.
            if (n64_mem32(rdram, 0x800D4604u) != 0) {
                ultramodern::process_deferred_rsp_completions(rdram);
                if (wr64_mesg_recv_fast(rdram, ctx)) {
                    break;
                }
                ultramodern::run_next_thread_and_wait(rdram);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            if (wr64_main_mq_has_gfx_task_pending(rdram) ||
                g_wr64_pending_main_gfx_task.load(std::memory_order_acquire) != 0u) {
                ultramodern::process_deferred_rsp_completions(rdram);
                if (wr64_mesg_recv_fast(rdram, ctx)) {
                    break;
                }
                ultramodern::run_next_thread_and_wait(rdram);
                std::this_thread::sleep_for(std::chrono::microseconds(200));
                continue;
            }
            ultramodern::wait_for_host_frame_presented();
            ultramodern::drain_external_messages(rdram);
            ultramodern::process_deferred_rsp_completions(rdram);
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            ultramodern::run_next_thread_and_wait(rdram);
        }
        finish_recv_fast();
        uint32_t received = 0;
        if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
            received = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
        }
        if (received == 0x19u) {
            wr64_main_handle_vi_message(rdram, received);
        }
        return;
    }

    // Lock-free consume when hardware/HLE already delivered (skip ultramodern mutex).
    if (wr64_mesg_recv_fast(rdram, ctx)) {
        if (queue == kBootMqEvents) {
            wr64_block_until_gfx_idle(rdram);
            wr64_on_boot_mq_events_recv_done(rdram, ctx);
        }
        if (queue == kMainThreadMesgQueue) {
            uint32_t received = 0;
            if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
                received = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
            }
            wr64_main_handle_vi_message(rdram, received);
        }
        if (is_boot_mesg_queue(queue)) {
            wr64_pace_boot_scheduler_recv(rdram, queue);
        }
        finish_recv_fast();
        return;
    }

    if (flags == OS_MESG_BLOCK && wr64_try_grant_stack_pi_access(rdram, queue) &&
        wr64_mesg_recv_fast(rdram, ctx)) {
        finish_recv_fast();
        return;
    }

    if (queue == kViEventQueue && flags == OS_MESG_BLOCK &&
        g_allow_vi_pulse.load(std::memory_order_acquire)) {
        // Present-driven VI inject only after first Gfx task; boot uses stock VI delivery.
        while (!wr64_mesg_recv_fast(rdram, ctx)) {
            ultramodern::wait_for_host_frame_presented();
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            std::this_thread::yield();
        }
        finish_recv_fast();
        return;
    }

    // osContInit / PI events share D_801540D0 during SysMain init.
    if (queue == kPiEventsMesgQueue && flags == OS_MESG_BLOCK) {
        wr64_ensure_boot_scheduler_queues(rdram);
        while (!wr64_mesg_recv_fast(rdram, ctx)) {
            if (static_cast<int32_t>(n64_mem32(rdram, queue + 0x10u)) == 0) {
                wr64_game_create_mesg_queue(rdram, queue, kPiEventsMesgBuf, 1);
            }
            ultramodern::send_si_message(rdram);
            wr64_deliver_mesg_no_wake(
                rdram,
                queue,
                static_cast<OSMesg>(static_cast<int32_t>(0xB)));
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            ultramodern::run_next_thread_and_wait(rdram);
        }
        finish_recv_fast();
        return;
    }

    if (is_boot_mesg_queue(queue) && flags == OS_MESG_BLOCK) {
        wr64_ensure_boot_scheduler_queues(rdram);
        if (queue == kBootMqMainSync) {
            static std::atomic<uint32_t> sync_wait{0};
            const uint32_t n = sync_wait.fetch_add(1) + 1;
            if (n <= 16u) {
                ultramodern::boot_log(
                    "[wr64] SysMain sync recv wait #%u queue=0x%08X depth=%d tid=%u entry=0x%08X ra=0x%08X\n",
                    n,
                    queue,
                    mesg_queue_depth(rdram, queue),
                    (ultramodern::this_thread() != NULLPTR)
                        ? TO_PTR(OSThread, ultramodern::this_thread())->id
                        : 0u,
                    wr64_running_thread_entry(),
                    static_cast<uint32_t>(ctx->r31));
            }
        }
        // SysMain blocks on 0x33 until Main finishes DP; inject only for sync queue.
        // Pace to host frames first so 0x29/0x33 are not delivered faster than VI.
        while (!wr64_mesg_recv_fast(rdram, ctx)) {
            if (wr64_host_hle_should_stop()) {
                ctx->r2 = -1;
                return;
            }
            static uint32_t boot_block_iter = 0;
            wr64_log_recv_block(rdram, queue, static_cast<uint32_t>(ctx->r31), ++boot_block_iter);
            ultramodern::process_deferred_rsp_completions(rdram);
            ultramodern::wait_for_host_frame_presented();
            ultramodern::drain_external_messages(rdram);
            ultramodern::process_deferred_rsp_completions(rdram);
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            if (queue == kBootMqMainSync) {
                if (!wr64::gfx_pipeline_busy(rdram)) {
                    ensure_bootstrap_main_sync_message(rdram);
                }
            } else if (queue == kBootMqEvents) {
                if (!wr64::gfx_pipeline_busy(rdram)) {
                    ensure_sysmain_event_message(rdram);
                }
            }
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            ultramodern::run_next_thread_and_wait(rdram);
            std::this_thread::yield();
        }
        if (queue == kBootMqMainSync) {
            while (wr64::gfx_pipeline_busy(rdram)) {
                ultramodern::run_next_thread_and_wait(rdram);
                std::this_thread::yield();
            }
        } else if (queue == kBootMqEvents) {
            wr64_block_until_gfx_idle(rdram);
            wr64_on_boot_mq_events_recv_done(rdram, ctx);
        }
        finish_recv_fast();
        return;
    }

    if (queue == kPiCmdQueue && flags == OS_MESG_BLOCK) {
        while (!wr64_mesg_recv_fast(rdram, ctx)) {
            if (wr64_host_hle_should_stop()) {
                ctx->r2 = -1;
                return;
            }
            ultramodern::run_next_thread_and_wait(rdram);
        }
        finish_recv_fast();
        return;
    }

    if ((queue == kPiAccessQueue || queue == kPiStackMesgQueue) && flags == OS_MESG_BLOCK) {
        while (!wr64_mesg_recv_fast(rdram, ctx)) {
            if (wr64_host_hle_should_stop()) {
                ctx->r2 = -1;
                return;
            }
            if (queue == kPiAccessQueue) {
                grant_bss_pi_access(rdram);
            } else {
                wr64_try_grant_stack_pi_access(rdram, queue);
            }
            // Consume grant before yielding — run_next_thread_and_wait can park forever
            // when the running queue is empty and we never registered on blocked_on_recv.
            if (wr64_mesg_recv_fast(rdram, ctx)) {
                break;
            }
            ultramodern::run_next_thread_and_wait(rdram);
        }
        finish_recv_fast();
        return;
    }

    if (is_overlay_loader_mesg_queue(queue)) {
        ctx->r2 = ultramodern::recv_mesg_fifo(
            rdram,
            static_cast<PTR(OSMesgQueue)>(queue),
            static_cast<PTR(OSMesg)>(ctx->r5),
            flags);
        if (ctx->r2 != 0 && wr64_mesg_recv_fast(rdram, ctx)) {
            finish_recv_fast();
            return;
        }
    } else if (wr64_mesg_recv_fast_queue(queue)) {
        ctx->r2 = osRecvMesg(rdram,
                             static_cast<int32_t>(ctx->r4),
                             static_cast<int32_t>(ctx->r5),
                             static_cast<int32_t>(ctx->r6));
        if (ctx->r2 != 0 && wr64_mesg_recv_fast(rdram, ctx)) {
            finish_recv_fast();
            return;
        }
    } else {
        ctx->r2 = osRecvMesg(rdram,
                             static_cast<int32_t>(ctx->r4),
                             static_cast<int32_t>(ctx->r5),
                             static_cast<int32_t>(ctx->r6));
    }

    if (ctx->r2 == 0 && flags == OS_MESG_BLOCK && depth_before > 0) {
        wr64_cooperative_yield_if_flooded(rdram, queue);
    }
    if (is_main_event_mesg_queue(queue) && ctx->r2 == 0 && flags == OS_MESG_BLOCK) {
        ultramodern::check_running_queue(rdram);
    }

    if (trace) {
        uint32_t received = 0;
        if (ctx->r5 != 0 && is_kseg0_rdram_pointer(static_cast<uint32_t>(ctx->r5))) {
            received = n64_mem32(rdram, static_cast<uint32_t>(ctx->r5));
        }
        ultramodern::boot_log(
            "[wr64] osRecvMesg exit  #%u queue=0x%08X ret=%lld msg=0x%08X\n",
            n,
            queue,
            static_cast<long long>(ctx->r2),
            received);
        log_mesg_queue_state("RECV exit", rdram, queue);
    }
}

extern "C" void wr64_bridge_osSendMesg(uint8_t* rdram, recomp_context* ctx) {
    wr64_flush_deferred_sysmain_bootstrap(rdram);
    static std::atomic<uint32_t> send_count{0};
    const uint32_t n = send_count.fetch_add(1) + 1;
    const uint32_t queue = static_cast<uint32_t>(ctx->r4);
    if (!is_kseg0_rdram_pointer(queue)) {
        ultramodern::boot_log( "[wr64] osSendMesg: invalid queue 0x%08X\n", queue);
        ctx->r2 = -1;
        return;
    }
    if (queue == kPiCmdQueue || queue == kPiAccessQueue ||
        queue == kPiStackMesgQueue) {
        ensure_pi_manager_globals(rdram);
        ensure_pi_device_ready(rdram, ctx);
    }
    sanitize_mesg_queue_heads(rdram, queue);
    if (queue == kMainThreadMesgQueue) {
        ensure_main_thread_mesg_queue(rdram);
        sanitize_mesg_queue_heads(rdram, queue);
    }
    const uint32_t msg = static_cast<uint32_t>(ctx->r5);
    if (queue == kPiCmdQueue && is_kseg0_rdram_pointer(msg)) {
        if (n <= 16) {
            log_pi_io_message(rdram, msg);
        }
        hle_pi_cmd_queue_mesg(rdram, msg);
    }
    const int32_t flags = static_cast<int32_t>(ctx->r6);
    const bool trace = trace_mesg_queue(queue, n);
    const bool scheduler_mq = is_scheduler_mesg_queue(queue);

    if (trace) {
        ultramodern::boot_log(
            "[wr64] osSendMesg enter #%u queue=0x%08X msg=0x%08X flags=%d\n",
            n,
            queue,
            msg,
            flags);
        log_mesg_queue_state("SEND enter", rdram, queue);
    }

    // Overlay loader queue: hardware inject + FIFO wake (matches recv_mesg_fifo).
    // Never use noblock_fast here — it can enqueue without waking blocked_on_recv==0.
    if (is_overlay_loader_mesg_queue(queue)) {
        auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
        if (mq->validCount > 0) {
            ctx->r2 = 0;
            if (trace) {
                ultramodern::boot_log(
                    "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (overlay_skip_dup)\n",
                    n,
                    queue,
                    static_cast<long long>(ctx->r2));
            }
            return;
        }
        ultramodern::deliver_mesg_immediate_tail(
            rdram,
            static_cast<PTR(OSMesgQueue)>(queue),
            static_cast<OSMesg>(static_cast<int32_t>(msg)));
        ctx->r2 = 0;
        if (trace) {
            ultramodern::boot_log(
                "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (overlay_deliver)\n",
                n,
                queue,
                static_cast<long long>(ctx->r2));
            log_mesg_queue_state("SEND exit", rdram, queue);
        }
        return;
    }

    if (queue == kPiCmdQueue ||
        ((queue == kPiAccessQueue || queue == kPiStackMesgQueue) && flags == OS_MESG_NOBLOCK)) {
        auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
        if (mq->validCount > 0) {
            ctx->r2 = 0;
            if (trace) {
                ultramodern::boot_log(
                    "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (pi_deliver_skip)\n",
                    n,
                    queue,
                    static_cast<long long>(ctx->r2));
            }
            return;
        }
        ultramodern::deliver_mesg_immediate(
            rdram,
            static_cast<PTR(OSMesgQueue)>(queue),
            static_cast<OSMesg>(static_cast<int32_t>(msg)));
        ctx->r2 = 0;
        if (trace) {
            ultramodern::boot_log(
                "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (pi_deliver)\n",
                n,
                queue,
                static_cast<long long>(ctx->r2));
            log_mesg_queue_state("SEND exit", rdram, queue);
        }
        return;
    }

    if (queue == kMainThreadMesgQueue && flags == OS_MESG_NOBLOCK) {
        if (msg == kGfxTaskMesg) {
            if (wr64_should_coalesce_gfx_task_send(rdram)) {
                ctx->r2 = -1;
                if (trace) {
                    ultramodern::boot_log(
                        "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (main_mq_gfx_coalesce)\n",
                        n,
                        queue,
                        static_cast<long long>(ctx->r2));
                }
                return;
            }
            wr64_prepare_gfx_task_for_main(rdram);
        }
        auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
        if (mq->msgCount > 0 && mq->validCount >= mq->msgCount) {
            ctx->r2 = -1;
            if (trace) {
                ultramodern::boot_log(
                    "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (main_mq_full)\n",
                    n,
                    queue,
                    static_cast<long long>(ctx->r2));
            }
            return;
        }
        if (msg == kGfxTaskMesg) {
            wr64_publish_pending_main_gfx_task(n64_mem32(rdram, kSGfxTaskGlobal));
            ultramodern::deliver_mesg_immediate(
                rdram,
                static_cast<PTR(OSMesgQueue)>(queue),
                static_cast<OSMesg>(static_cast<int32_t>(msg)));
        } else {
            wr64_deliver_mesg_no_wake(
                rdram,
                queue,
                static_cast<OSMesg>(static_cast<int32_t>(msg)));
        }
        // SP/DP/audio events use the same queue; Main polls recv_fast (no blocked_on_recv).
        wr64_wake_main_thread(rdram);
        ctx->r2 = 0;
        if (trace) {
            ultramodern::boot_log(
                "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (main_mq_deliver)\n",
                n,
                queue,
                static_cast<long long>(ctx->r2));
            log_mesg_queue_state("SEND exit", rdram, queue);
        }
        return;
    }

    if (is_boot_mesg_queue(queue) && flags == OS_MESG_NOBLOCK) {
        wr64_ensure_boot_scheduler_queues(rdram);
        auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
        if (mq->msgCount <= 0) {
            ctx->r2 = -1;
            if (trace) {
                ultramodern::boot_log(
                    "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (boot_mq_invalid)\n",
                    n,
                    queue,
                    static_cast<long long>(ctx->r2));
            }
            return;
        }
        if (mq->validCount >= mq->msgCount) {
            if (g_allow_vi_pulse.load(std::memory_order_acquire) && mq->validCount > 0) {
                if (trace && queue == kBootMqVi) {
                    ultramodern::boot_log(
                        "[wr64] osSendMesg #%u queue=0x%08X (boot_mq_audio_overwrite) dropping prior VI tick msg\n",
                        n,
                        queue);
                }
                mq->first = static_cast<int16_t>((mq->first + 1) % mq->msgCount);
                mq->validCount--;
            } else {
                ctx->r2 = -1;
                if (trace) {
                    ultramodern::boot_log(
                        "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (boot_mq_full)\n",
                        n,
                        queue,
                        static_cast<long long>(ctx->r2));
                }
                return;
            }
        }
        if (queue == kBootMqEvents && msg == 0x29u && wr64_sysmain_event_outstanding(rdram)) {
            ctx->r2 = 0;
            if (trace) {
                ultramodern::boot_log(
                    "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (sysmain_0x29_pending)\n",
                    n,
                    queue,
                    static_cast<long long>(ctx->r2));
            }
            return;
        }
        wr64_deliver_mesg_no_wake(
            rdram,
            queue,
            static_cast<OSMesg>(static_cast<int32_t>(msg)));
        if (queue == kBootMqEvents || queue == kBootMqMainSync) {
            wr64_wake_sysmain_thread(rdram);
        }
        ctx->r2 = 0;
        if (trace) {
            ultramodern::boot_log(
                "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (%s)\n",
                n,
                queue,
                static_cast<long long>(ctx->r2),
                wr64_boot_mq_tag(queue));
            log_mesg_queue_state("SEND exit", rdram, queue);
        }
        return;
    }

    if (wr64_mesg_send_noblock_fast(rdram, ctx)) {
        if (trace) {
            ultramodern::boot_log(
                "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld (noblock_fast)\n",
                n,
                queue,
                static_cast<long long>(ctx->r2));
            log_mesg_queue_state("SEND exit", rdram, queue);
        }
        return;
    }

    if (scheduler_mq) {
        ultramodern::wake_recv_blocked_threads(rdram, static_cast<PTR(OSMesgQueue)>(queue));
    }

    ctx->r2 = osSendMesg(rdram,
                         static_cast<int32_t>(ctx->r4),
                         static_cast<OSMesg>(ctx->r5),
                         static_cast<int32_t>(ctx->r6));

    if (scheduler_mq && static_cast<int32_t>(ctx->r2) == -1) {
        wr64_cooperative_yield_if_flooded(rdram, queue);
        wr64_host_drain_scheduler_queue(rdram, queue, ctx);
        ctx->r2 = osSendMesg(rdram,
                             static_cast<int32_t>(ctx->r4),
                             static_cast<OSMesg>(ctx->r5),
                             static_cast<int32_t>(ctx->r6));
    }

    if (ctx->r2 == 0) {
        wr64_cooperative_yield_if_flooded(rdram, queue);
    }

    if (trace) {
        ultramodern::boot_log(
            "[wr64] osSendMesg exit  #%u queue=0x%08X ret=%lld\n",
            n,
            queue,
            static_cast<long long>(ctx->r2));
        log_mesg_queue_state("SEND exit", rdram, queue);
    }
}

enum class PiDmaCompletion {
    None,
    // PI manager (__osDevMgrMain) waits on evtQueue, then posts ioMesg to retQueue.
    PiInterrupt,
    // osEPiStartDma HLE: caller blocks on retQueue for the OSIoMesg pointer.
    RetQueueMesg,
};

bool execute_pi_dma_transfer(
    uint8_t* rdram,
    const char* tag,
    const OSIoMesg& mb,
    uint32_t call_index,
    PiDmaCompletion completion,
    uint32_t mesg_vaddr,
    bool apply_overlays);

constexpr OSMesg kPiEventMesg = static_cast<OSMesg>(static_cast<int32_t>(0x22222222u));
constexpr OSMesg kBootTimerBootstrapMesg = static_cast<OSMesg>(static_cast<int32_t>(0x19u));
constexpr OSMesg kBootMainSyncBootstrapMesg = static_cast<OSMesg>(static_cast<int32_t>(0x33u));

// Main thread bootstrap: osSetTimer/msg 0x19 via 800C63B0 must reach kBootMqTimer before the
// scheduler loop can post 0x1F (VI) and 0x29 (events). Inject once when the queue drains.
void ensure_bootstrap_timer_message(uint8_t* rdram) {
    if (wr64_host_hle_should_stop() || !wr64_boot_hle_inject_enabled()) {
        return;
    }
    if (!is_kseg0_rdram_pointer(kBootMqTimer)) {
        return;
    }
    static std::atomic<bool> bootstrap_injected{false};
    if (bootstrap_injected.load(std::memory_order_acquire)) {
        return;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kBootMqTimer));
    if (mq->validCount > 0) {
        return;
    }
    bootstrap_injected.store(true, std::memory_order_release);
    static std::atomic<uint32_t> inject_count{0};
    const uint32_t n = inject_count.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log( "[wr64] boot_timer_inject #%u msg=0x19 queue=0x%08X\n", n, kBootMqTimer);
    }
    wr64_deliver_mesg_no_wake(
        rdram, kBootMqTimer, kBootTimerBootstrapMesg);
}

// SysMain recv on kBootMqMainSync waits for Main's DP handler to osSendMesg 0x33.
void ensure_bootstrap_main_sync_message(uint8_t* rdram) {
    if (wr64_host_hle_should_stop()) {
        return;
    }
    if (wr64::gfx_pipeline_busy(rdram) || g_allow_vi_pulse.load(std::memory_order_acquire)) {
        return;
    }
    if (!is_kseg0_rdram_pointer(kBootMqMainSync)) {
        return;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kBootMqMainSync));
    if (mq->validCount > 0) {
        return;
    }
    if (wr64_boot_hle_inject_enabled()) {
        static std::atomic<bool> bootstrap_injected{false};
        if (bootstrap_injected.load(std::memory_order_acquire)) {
            return;
        }
        bootstrap_injected.store(true, std::memory_order_release);
    }
    static std::atomic<uint32_t> inject_count{0};
    const uint32_t n = inject_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log(
            "[wr64] boot_main_sync_inject #%u msg=0x33 queue=0x%08X\n",
            n,
            kBootMqMainSync);
    }
    wr64_deliver_mesg_no_wake(
        rdram,
        kBootMqMainSync,
        kBootMainSyncBootstrapMesg);
}

// SysMain_Thread game loop blocks on kBootMqEvents for Main's 0x29 (VI-throttled).
// Pace to host frames and inject once per wait if Main has not posted yet.
void ensure_sysmain_event_message(uint8_t* rdram) {
    if (wr64_host_hle_should_stop()) {
        return;
    }
    if (wr64::gfx_pipeline_busy(rdram)) {
        return;
    }
    if (!is_kseg0_rdram_pointer(kBootMqEvents)) {
        return;
    }
    wr64_ensure_boot_scheduler_queues(rdram);
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kBootMqEvents));
    if (mq->msgCount <= 0) {
        return;
    }
    if (mq->validCount > 0) {
        return;
    }
    if (wr64_sysmain_event_outstanding(rdram)) {
        return;
    }
    static std::atomic<uint32_t> inject_count{0};
    const uint32_t n = inject_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log(
            "[wr64] sysmain_event_inject #%u msg=0x29 queue=0x%08X\n",
            n,
            kBootMqEvents);
    }
    wr64_deliver_mesg_no_wake(
        rdram,
        kBootMqEvents,
        static_cast<OSMesg>(static_cast<int32_t>(0x29)));
}

extern "C" void wr64_codeSEG_8004A2B4(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    recomp_context recv_ctx{};
    recv_ctx.r4 = kPiEventsMesgQueue;
    recv_ctx.r5 = kContSiMesg;
    recv_ctx.r6 = 1; // OS_MESG_BLOCK
    if (wr64_mesg_recv_fast(rdram, &recv_ctx)) {
        // SI completion delivered.
    }

    recomp_context pad_ctx{};
    pad_ctx.r4 = kControllersPad;
    osContGetReadData_recomp(rdram, &pad_ctx);

    const uint8_t mask = n64_mem8(rdram, kContMaskByte);
    const uint8_t effective_mask = mask != 0 ? mask : 1u;
    uint8_t bit = 1;
    for (int i = 0; i < 4; ++i) {
        if (effective_mask & bit) {
            const uint32_t ctrl = kControllerOne + static_cast<uint32_t>(i) * 0xAu;
            const uint32_t pad = kControllersPad + static_cast<uint32_t>(i) * 6u;
            const uint16_t prev_btn = n64_mem16(rdram, ctrl + 0u);
            const uint16_t new_btn = n64_mem16(rdram, pad + 0u);
            const uint16_t delta = static_cast<uint16_t>(new_btn ^ prev_btn);
            n64_mem16(rdram, ctrl + 6u) = prev_btn;
            n64_mem16(rdram, ctrl + 0u) = new_btn;
            n64_mem16(rdram, ctrl + 2u) = static_cast<uint16_t>(new_btn & delta);
            n64_mem16(rdram, ctrl + 4u) = static_cast<uint16_t>(prev_btn & delta);
            n64_mem8(rdram, ctrl + 8u) = n64_mem8(rdram, pad + 2u);
            n64_mem8(rdram, ctrl + 9u) = n64_mem8(rdram, pad + 3u);
        }
        bit = static_cast<uint8_t>(bit << 1);
    }

    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log(
            "[wr64] SysUtils_UpdateControllers #%u mask=0x%02X btn0=0x%04X\n",
            n,
            mask,
            static_cast<unsigned>(n64_mem16(rdram, kControllerOne)));
    }
}

static void wr64_append_gfx_u64(uint8_t* rdram, uint32_t& head, uint32_t w0, uint32_t w1);
static void wr64_append_sp_segment(uint8_t* rdram, uint32_t& head, uint32_t seg, uint32_t base);
static void wr64_append_sp_branch_dl(uint8_t* rdram, uint32_t& head, uint32_t dl_virt);
static void wr64_append_dp_pipe_sync(uint8_t* rdram, uint32_t& head);
extern "C" void wr64_guOrthoF_impl(float mf[4][4], float l, float r, float b, float t, float n, float f, float scale);
void wr64_guMtxF2L(uint8_t* rdram, uint32_t m_vaddr, const float mf[4][4]);

[[maybe_unused]] static void wr64_ensure_prog_overlay_loaded(uint8_t* rdram) {
    static std::atomic<bool> loaded{false};
    if (loaded.load(std::memory_order_acquire)) {
        return;
    }
    const std::span<const uint8_t> rom = recomp::get_rom();
    if (kProgOverlayRom + kProgOverlaySize > rom.size()) {
        return;
    }
    std::memcpy(n64_vram(rdram, kProgOverlayVram), rom.data() + kProgOverlayRom, kProgOverlaySize);
    loaded.store(true, std::memory_order_release);
    ultramodern::boot_log(
        "[wr64] prog overlay: loaded ROM 0x%08X -> 0x%08X (%u bytes)\n",
        kProgOverlayRom,
        kProgOverlayVram,
        kProgOverlaySize);
}

[[maybe_unused]] static void wr64_append_dp_set_fill_color(uint8_t* rdram, uint32_t& head, uint32_t color) {
    wr64_append_gfx_u64(rdram, head, 0xF7000000u, color);
}

[[maybe_unused]] static void wr64_append_dp_fill_rectangle(
    uint8_t* rdram,
    uint32_t& head,
    uint32_t xl,
    uint32_t yl,
    uint32_t xh,
    uint32_t yh) {
    const uint32_t w0 = (0xF6u << 24) | ((yh & 0x3FFu) << 12) | (yl & 0x3FFu);
    const uint32_t w1 = ((xh & 0x3FFu) << 12) | (xl & 0x3FFu);
    wr64_append_gfx_u64(rdram, head, w0, w1);
}

// func_800926F4 jump-table targets include epilogue labels (e.g. 0x80092928) not in func_map.
// Title states 5/6 have no decomp cases — stock jumps straight to the shared epilogue.
extern "C" void wr64_codeSEG_800926F4(uint8_t* rdram, recomp_context* ctx) {
    const uint8_t gs = wr64_game_state_byte(rdram);
    if (gs == 5u || gs == 6u) {
        return;
    }
    const uint32_t idx = static_cast<uint32_t>(gs) - 2u;
    if (idx >= kGsAuxHandlerCount) {
        static_0_80092928(rdram, ctx);
        return;
    }
    const uint32_t target = n64_mem32(rdram, kGsAuxHandlerTableVram + idx * 4u);
    if (target == 0x80092928u) {
        static_0_80092928(rdram, ctx);
        return;
    }
    static std::atomic<uint32_t> unhandled{0};
    if (unhandled.fetch_add(1) < 8u) {
        ultramodern::boot_log("[wr64] 926F4: unhandled gs=%u target=0x%08X\n", gs, target);
    }
}

// Title-state jump-table target (stock: jal 0x802C5800 @ 0x80092654).
extern "C" void wr64_jt_80092654(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> jt_calls{0};
    const uint32_t n = jt_calls.fetch_add(1) + 1;
    if (n <= 16u) {
        ultramodern::boot_log("[wr64] jt_80092654 #%u ra=0x%08X\n", n, static_cast<uint32_t>(ctx->r31));
    }
    wr64_dispatch_802C5800(rdram, ctx);
}

extern "C" void wr64_dispatch_802C5800(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    const uint8_t gs = wr64_game_state_byte(rdram);
    const uint32_t mode = n64_mem32(rdram, kOverlayModeIdx);
    const uint32_t gate = n64_mem32(rdram, kOverlayLoadGate);
    if (n <= 16u) {
        ultramodern::boot_log(
            "[wr64] 802C5800 dispatch #%u gs=%u gate=%u D_801CE638=%u ra=0x%08X\n",
            n,
            gs,
            gate,
            mode,
            static_cast<uint32_t>(ctx->r31));
    }
    if (gs == 5u || gs == 6u) {
        ovl_i3_802C5800(rdram, ctx);
    } else {
        segment_1B1FB0_802C5800(rdram, ctx);
    }
    if (n <= 16u) {
        ultramodern::boot_log("[wr64] 802C5800 dispatch #%u done\n", n);
    }
}

extern "C" void wr64_codeSEG_80092CF0(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> call_count{0};
    const uint32_t call_n = call_count.fetch_add(1) + 1;
    wr64_block_until_gfx_idle(rdram);
    const uint32_t pool = n64_mem32(rdram, kGfxPoolGlobal);
    wr64_trace_game_state(rdram, "92CF0 enter", static_cast<uint32_t>(ctx->r31));
    const uint8_t game_state = wr64_game_state_byte(rdram);
    if (call_n <= 16u && (game_state == 5u || game_state == 6u)) {
        const uint32_t gs_idx = static_cast<uint32_t>(game_state);
        ultramodern::boot_log(
            "[wr64] 92CF0 #%u jt: gs_tbl[%u]=0x%08X aux_tbl[%u]=0x%08X "
            "unk_load[%u]=0x%08X D_800D45F0=0x%08X seg8_resolved=0x%08X\n",
            call_n,
            gs_idx,
            n64_mem32(rdram, kGameStateHandlerTableVram + gs_idx * 4u),
            gs_idx,
            n64_mem32(rdram, kGsAuxHandlerTableVram + gs_idx * 4u),
            gs_idx,
            n64_mem32(rdram, kUnkGameLoadTableVram + gs_idx * 4u),
            n64_mem32(rdram, kSeg8BaseGlobal),
            wr64_resolve_seg8_pool_virt(rdram));
    }
    if (wr64_host_hle_should_stop()) {
        ctx->r2 = ctx->r4;
        return;
    }
    wr64_apply_boot_display_mode_if_needed(rdram);
    wr64_fixup_seg8_pool_pointer(rdram);
    wr64_fixup_scene_framebuffer_bases(rdram);
    wr64_fixup_scene_gfx_pointer(rdram);
    uint32_t head = static_cast<uint32_t>(ctx->r4);
    if (!is_kseg0_rdram_pointer(head)) {
        ctx->r2 = head;
        return;
    }
    if (n64_mem32(rdram, 0x801AE948u) == 0) {
        wr64_set_scene_gfx_context(rdram);
    }
    const uint32_t scene_ctx = n64_mem32(rdram, 0x801AE948u);
    if (scene_ctx != 0) {
        wr64_append_sp_segment(rdram, head, 5, wr64_kseg0_to_phys(scene_ctx));
    }
    const bool overlay_gate = n64_mem32(rdram, kOverlayLoadGate) != 0;
    if (game_state == 5u || game_state == 6u || overlay_gate) {
        if (overlay_gate && (game_state == 5u || game_state == 6u)) {
            wr64_maybe_unk_game_load_before_overlay(rdram, ctx);
        }
        recomp_context overlay_ctx = *ctx;
        overlay_ctx.r4 = head;
        const uint32_t head_before = head;
        segment_1B1FB0_802C5BA4(rdram, &overlay_ctx);
        head = overlay_ctx.r2;
        n64_mem32(rdram, kDisplayListHeadGlobal) = head;
        ctx->r2 = head;
        wr64_repair_title_game_state(rdram, game_state);
        if (call_n <= 16u) {
            const uint32_t overlay_bytes =
                (head >= head_before) ? (head - head_before) : 0u;
            const uint32_t total_bytes = (head >= pool) ? (head - pool) : 0u;
            ultramodern::boot_log(
                "[wr64] 92CF0 overlay #%u gs=%u gate=%u overlay_bytes=0x%X total_dl=0x%X\n",
                call_n,
                wr64_game_state_byte(rdram),
                overlay_gate ? 1u : 0u,
                overlay_bytes,
                total_bytes);
        }
        wr64_trace_game_state(rdram, "92CF0 exit", static_cast<uint32_t>(ctx->r31));
        return;
    }
    codeSEG_80092CF0(rdram, ctx);
    wr64_trace_game_state(rdram, "92CF0 exit", static_cast<uint32_t>(ctx->r31));
}

extern "C" void wr64_codeSEG_800922E4(uint8_t* rdram, recomp_context* ctx) {
    static thread_local int call_depth = 0;
    struct DepthGuard {
        int& depth;
        explicit DepthGuard(int& d) : depth(d) { ++depth; }
        ~DepthGuard() { --depth; }
    };
    if (call_depth > 0) {
        static std::atomic<uint32_t> reentry_log{0};
        if (reentry_log.fetch_add(1) < 4u) {
            ultramodern::boot_log(
                "[wr64] 922E4: blocked reentry depth=%d gs=%u ra=0x%08X\n",
                call_depth,
                wr64_game_state_byte(rdram),
                static_cast<uint32_t>(ctx->r31));
        }
        return;
    }
    DepthGuard depth_guard(call_depth);

    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    wr64_fixup_gfx_matrix_pointer(rdram);
    wr64_seed_segment_globals_if_needed(rdram);
    wr64_refresh_runtime_jump_tables(rdram);
    wr64_reregister_title_dispatch();
    const uint8_t gs = wr64_game_state_byte(rdram);
    const uint32_t gs_u = gs;
    const uint32_t jt_idx = (gs_u >= 2u) ? (gs_u - 2u) : 0u;
    if (n <= 16u) {
        if (!wr64_jump_table_low_entry_plausible(rdram, kGameStateHandlerTableDispatchVram, jt_idx)) {
            ultramodern::boot_log(
                "[wr64] 922E4 #%u: bad RDRAM jt dispatch[%u]=0x%08X — repairing aliases (delta=0x%X)\n",
                n,
                jt_idx,
                n64_mem32(rdram, kGameStateHandlerTableDispatchVram + jt_idx * 4u),
                kGameStateJumpTableMirrorDelta);
            wr64_repair_jump_table_rdram_aliases(rdram);
        }
        ultramodern::boot_log(
            "[wr64] 922E4 enter #%u gs=%u jt_dispatch[gs-2]=0x%08X jt_high[gs]=0x%08X ra=0x%08X\n",
            n,
            gs,
            (jt_idx < kGameStateHandlerCount)
                ? n64_mem32(rdram, kGameStateHandlerTableDispatchVram + jt_idx * 4u)
                : 0u,
            (gs_u < kGameStateHandlerCount)
                ? n64_mem32(rdram, kGameStateHandlerTableVram + gs_u * 4u)
                : 0u,
            static_cast<uint32_t>(ctx->r31));
    }
    if (gs == 5u || gs == 6u) {
        codeSEG_8004A2B4(rdram, ctx);
        codeSEG_8006A264(rdram, ctx);
        if (n64_mem32(rdram, kOverlayLoadGate) != 0u) {
            wr64_codeSEG_800926F4(rdram, ctx);
        }
        codeSEG_80092938(rdram, ctx);
        wr64_jt_80092654(rdram, ctx);
        if (n <= 16u) {
            ultramodern::boot_log("[wr64] 922E4 exit #%u (title shortcut)\n", n);
        }
        return;
    }
    codeSEG_800922E4(rdram, ctx);
    if (n <= 16u) {
        ultramodern::boot_log("[wr64] 922E4 exit #%u\n", n);
    }
}

extern "C" void wr64_codeSEG_80046D2C(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> frame_count{0};
    const uint32_t frame = frame_count.fetch_add(1) + 1;
    const uint32_t caller_ra = static_cast<uint32_t>(ctx->r31);
    // Pool/task rotation only — stock func_80046850 toggles D_800D45D8, then 468E0 binds color.
    wr64_fixup_scene_framebuffer_bases(rdram);
    wr64_wait_pending_main_gfx_dispatched(rdram);
    wr64::wait_hw_gfx_idle(rdram);
    wr64_sysmain_gfx_init_buffers(rdram);
    wr64_fixup_scene_framebuffer_bases(rdram);
    wr64_fixup_gfx_matrix_pointer(rdram);
    wr64_seed_segment_globals_if_needed(rdram);
    // Stock dispatch reads jump tables at +0x20000 alias VRAMs; re-seed after Gfx each frame.
    wr64_sync_game_state_dispatch(rdram);
    wr64_seed_game_state_jump_table(rdram);
    wr64_seed_gs_aux_jump_table(rdram);
    wr64_seed_asset_load_jump_tables(rdram);
    wr64_refresh_runtime_jump_tables(rdram);
    wr64_reregister_title_dispatch();
    const uint8_t gs_for_jt = wr64_game_state_byte(rdram);
    if (gs_for_jt >= 2u) {
        const uint32_t jt_idx = static_cast<uint32_t>(gs_for_jt) - 2u;
        if (!wr64_jump_table_low_entry_plausible(rdram, kGameStateHandlerTableDispatchVram, jt_idx)) {
            if (frame <= 16u) {
                ultramodern::boot_log(
                    "[wr64] 46D2C frame #%u: RDRAM jt dispatch[%u]=0x%08X invalid before 922E4 "
                    "(delta=0x%X ra=0x%08X) — repair\n",
                    frame,
                    jt_idx,
                    n64_mem32(rdram, kGameStateHandlerTableDispatchVram + jt_idx * 4u),
                    kGameStateJumpTableMirrorDelta,
                    caller_ra);
            }
            wr64_repair_jump_table_rdram_aliases(rdram);
        }
    }
    if (frame <= 16u) {
        const uint8_t gs = wr64_game_state_byte(rdram);
        const uint32_t gs_u = gs;
        ultramodern::boot_log(
            "[wr64] 46D2C pre-922E4 frame #%u gs=%u gate=%u mode_idx=%u aux6=0x%08X "
            "unk_load=0x%08X dispatch=0x%08X mode_fb_idx=%u ra=0x%08X\n",
            frame,
            gs,
            n64_mem32(rdram, kOverlayLoadGate),
            n64_mem32(rdram, kOverlayModeIdx),
            n64_mem32(rdram, kGsAuxHandlerTableVram + 4u * 4u),
            (gs_u < kUnkGameLoadTableCount)
                ? n64_mem32(rdram, kUnkGameLoadTableVram + gs_u * 4u)
                : 0u,
            n64_mem32(rdram, kGameStateDispatchGlobal),
            n64_mem32(rdram, kDisplayModeFbIdxGlobal),
            caller_ra);
        const uint32_t pool_idx = n64_mem32(rdram, kGfxPoolIdx) & 1u;
        const uint32_t other_pool = kGfxPoolArr + ((pool_idx ^ 1u) & 1u) * kGfxPoolBytes;
        ultramodern::boot_log(
            "[wr64] 46D2C pre-922E4 frame #%u segs "
            "D_8015198C=0x%08X D_801519B4=0x%08X D_801519BC=0x%08X "
            "gGfxPool=0x%08X other_pool=0x%08X\n",
            frame,
            n64_mem32(rdram, 0x8015198Cu),
            n64_mem32(rdram, 0x801519B4u),
            n64_mem32(rdram, 0x801519BCu),
            n64_mem32(rdram, kGfxPoolGlobal),
            other_pool);
    }
}

static void wr64_append_gfx_u64(uint8_t* rdram, uint32_t& head, uint32_t w0, uint32_t w1) {
    if (!is_kseg0_rdram_pointer(head)) {
        return;
    }
    n64_mem32(rdram, head + 0u) = w0;
    n64_mem32(rdram, head + 4u) = w1;
    head += 8u;
}

static void wr64_append_sp_segment(uint8_t* rdram, uint32_t& head, uint32_t seg, uint32_t base) {
    const uint32_t w0 = (0xBCu << 24) | ((seg * 4u) << 8) | 0x06u;
    wr64_append_gfx_u64(rdram, head, w0, base);
}

[[maybe_unused]] static void wr64_append_sp_branch_dl(uint8_t* rdram, uint32_t& head, uint32_t dl_virt) {
    wr64_append_gfx_u64(rdram, head, 0x06000000u, dl_virt);
}

static void wr64_append_dp_pipe_sync(uint8_t* rdram, uint32_t& head) {
    wr64_append_gfx_u64(rdram, head, 0xE7000000u, 0u);
}

static void wr64_append_dp_set_color_image(uint8_t* rdram, uint32_t& head, uint32_t width, uint32_t addr) {
    const uint32_t phys = wr64_kseg0_to_phys(addr);
    const uint32_t w0 = (0xFFu << 24) | (0u << 21) | (2u << 19) | (width - 1u);
    wr64_append_gfx_u64(rdram, head, w0, phys);
}

static void wr64_append_color_image_for_mode(uint8_t* rdram, uint32_t& head) {
    const uint32_t mode = n64_mem32(rdram, kDisplayModeGlobal);
    const uint32_t fb = wr64_resolve_color_framebuffer(rdram);
    if (!is_kseg0_rdram_pointer(fb)) {
        return;
    }
    if (mode == 1u || mode == 2u) {
        wr64_append_sp_segment(rdram, head, 4, wr64_kseg0_to_phys(fb));
    }
    wr64_append_dp_pipe_sync(rdram, head);
    const uint32_t width = (mode == 3u) ? 640u : 320u;
    wr64_append_dp_set_color_image(rdram, head, width, fb);
}

extern "C" void wr64_codeSEG_800468E0(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    wr64_fixup_scene_framebuffer_bases(rdram);
    wr64_seed_segment_globals_if_needed(rdram);
    wr64_seed_framebuffers_if_needed(rdram);
    if (n64_mem32(rdram, 0x801AE948u) == 0) {
        wr64_set_scene_gfx_context(rdram);
    }

    uint32_t head = n64_mem32(rdram, kDisplayListHeadGlobal);
    if (!is_kseg0_rdram_pointer(head)) {
        head = n64_mem32(rdram, kGfxPoolGlobal);
        if (!is_kseg0_rdram_pointer(head)) {
            head = kGfxPoolArr;
        }
        n64_mem32(rdram, kDisplayListHeadGlobal) = head;
    }

    const uint32_t pool_start = n64_mem32(rdram, kGfxPoolGlobal);
    const uint32_t dl_start = is_kseg0_rdram_pointer(pool_start) ? pool_start : head;
    if (is_kseg0_rdram_pointer(head) && is_kseg0_rdram_pointer(pool_start) && head > pool_start) {
        const uint32_t existing_bytes = (head - pool_start);
        if (existing_bytes >= 0x58u && existing_bytes != 0xC0u) {
            return;
        }
        head = pool_start;
        n64_mem32(rdram, kDisplayListHeadGlobal) = head;
    }

    wr64_append_sp_segment(rdram, head, 0, 0u);
    wr64_append_sp_segment(rdram, head, 1, n64_mem32(rdram, kSeg1BaseGlobal));
    wr64_append_sp_segment(rdram, head, 2, wr64_kseg0_to_phys(kSeg2PtrGlobal));
    wr64_append_sp_segment(rdram, head, 3, wr64_kseg0_to_phys(n64_mem32(rdram, kGfxPoolGlobal)));
    wr64_append_sp_segment(rdram, head, 7, wr64_kseg0_to_phys(n64_mem32(rdram, kSeg7MatrixGlobal)));
    wr64_append_sp_segment(rdram, head, 8, n64_mem32(rdram, kSeg8BaseGlobal));
    wr64_append_sp_segment(rdram, head, 13, n64_mem32(rdram, kSeg13BaseGlobal));
    wr64_append_sp_segment(rdram, head, 14, n64_mem32(rdram, kSeg14BaseGlobal));
    const uint32_t scene_ctx = n64_mem32(rdram, 0x801AE948u);
    if (scene_ctx != 0) {
        wr64_append_sp_segment(rdram, head, 5, wr64_kseg0_to_phys(scene_ctx));
    }

    wr64_apply_boot_display_mode_if_needed(rdram);
    wr64_seed_display_mode_fbs_if_needed(rdram);
    wr64_fixup_seg8_pool_pointer(rdram);

    // Skip gSPDisplayList(D_1000000): the sub-DL runs func_80093AFC and leaves RT64's RDP
    // color image on the Z-buffer (0x802A0000). Stock recovers via a later gDPSetColorImage;
    // title path uses func_802C5BA4 after bootstrap instead.
    wr64_append_color_image_for_mode(rdram, head);

    n64_mem32(rdram, kDisplayListHeadGlobal) = head;

    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n <= 8) {
        const uint32_t bootstrap_bytes = (head >= dl_start) ? (head - dl_start) : 0u;
        const uint32_t color_fb = wr64_resolve_color_framebuffer(rdram);
        const uint32_t swap_idx = n64_mem32(rdram, kDisplayModeFbIdxGlobal) & 1u;
        ultramodern::boot_log(
            "[wr64] GfxBootstrap #%u mode=%u swap_idx=%u gFB_idx=%u "
            "D_800D45DC={0x%08X,0x%08X} head=0x%08X bootstrap_bytes=0x%X "
            "color_fb=0x%08X seg1=0x%08X\n",
            n,
            n64_mem32(rdram, kDisplayModeGlobal),
            swap_idx,
            n64_mem32(rdram, kFramebuffersIdxGlobal) & 3u,
            n64_mem32(rdram, kDisplayModeFbArray + 0u),
            n64_mem32(rdram, kDisplayModeFbArray + 4u),
            head,
            bootstrap_bytes,
            color_fb,
            n64_mem32(rdram, kSeg1BaseGlobal));
    }
}

extern "C" void wr64_codeSEG_80046BF4(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    uint32_t head = n64_mem32(rdram, kDisplayListHeadGlobal);
    if (!is_kseg0_rdram_pointer(head)) {
        static std::atomic<uint32_t> skip_count{0};
        const uint32_t n = skip_count.fetch_add(1) + 1;
        if (n <= 8) {
            ultramodern::boot_log("[wr64] GfxFullSync skip (null dl head)\n");
        }
        return;
    }
    n64_mem32(rdram, head + 0x00u) = 0xE9000000u; // gDPFullSync
    n64_mem32(rdram, head + 0x04u) = 0u;
    n64_mem32(rdram, head + 0x08u) = 0xB8000000u; // gSPEndDisplayList
    n64_mem32(rdram, head + 0x0Cu) = 0u;
    n64_mem32(rdram, kDisplayListHeadGlobal) = head + 0x10u;
}

extern "C" void wr64_codeSEG_80046C30(uint8_t* rdram, recomp_context* ctx) {
    wr64_codeSEG_80046BF4(rdram, ctx);
    wr64_prepare_gfx_task_for_main(rdram);
}

extern "C" void wr64_codeSEG_80046CF8(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    wr64_prepare_gfx_task_for_main(rdram);
    wr64_seed_framebuffers_if_needed(rdram);
    // Mode 2 copies via gFrameBuffers[3]; keep boot on mode 3 (osViSwapBuffer path).
    if (n64_mem32(rdram, kDisplayModeGlobal) == 2u) {
        n64_mem32(rdram, kDisplayModeGlobal) = 3u;
    }
    const uint32_t task = n64_mem32(rdram, kSGfxTaskGlobal);
    if (!is_kseg0_rdram_pointer(task)) {
        return;
    }
    n64_mem32(rdram, kCurrentGfxTaskGlobal) = task;
    static std::atomic<uint32_t> send_count{0};
    const uint32_t n = send_count.fetch_add(1) + 1;
    if (n <= 16) {
        ultramodern::boot_log("[wr64] SendGfxTask #%u task=0x%08X\n", n, task);
    }
    if (wr64_should_coalesce_gfx_task_send(rdram)) {
        if (n <= 16) {
            ultramodern::boot_log("[wr64] SendGfxTask #%u coalesced (gfx pending)\n", n);
        }
        return;
    }
    wr64_publish_pending_main_gfx_task(task);
    ultramodern::deliver_mesg_immediate(
        rdram,
        static_cast<PTR(OSMesgQueue)>(kMainThreadMesgQueue),
        static_cast<OSMesg>(static_cast<int32_t>(kGfxTaskMesg)));
    wr64_wake_main_thread(rdram);
    ultramodern::check_running_queue(rdram);
}

extern "C" void wr64_codeSEG_80097E68(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    // D_800DAB38 DMA queue — stock path runs when not patched.
}

extern "C" void wr64_codeSEG_80095A28(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    // Overlay segment loader; defer until GameLoad HLE is fully wired.
}

extern "C" void wr64_codeSEG_80095050(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    // unk_game_load — stock path runs when not patched.
}

extern "C" void wr64_codeSEG_80098208(uint8_t* rdram, recomp_context* ctx) {
    const uint8_t gs = wr64_game_state_byte(rdram);
    const int ovl_idx = wr64_gameload_overlay_index(gs);
    if (ovl_idx < 0) {
        return;
    }

    wr64_seed_goverlay_table(rdram);
    const Wr64OverlayRow& ovl = kWr64OverlayRows[static_cast<size_t>(ovl_idx)];
    uint32_t size = ovl.rom_end - ovl.rom_start;
    size = (size + 1u) & ~1u;

    static std::atomic<uint32_t> load_count{0};
    const uint32_t n = load_count.fetch_add(1) + 1;
    ultramodern::boot_log(
        "[wr64] GameLoad_LoadOverlay HLE #%u gs=%u ovl=%d rom=[0x%08X,0x%08X) size=0x%08X dram=0x%08X\n",
        n,
        gs,
        ovl_idx,
        ovl.rom_start,
        ovl.rom_end,
        size,
        ovl.text_start);

    wr64_drain_pi_mesg_queue_if_full(rdram);

    OSIoMesg mesg{};
    mesg.hdr.retQueue = static_cast<PTR(OSMesgQueue)>(kPiSecondaryMesgQueue);
    mesg.dramAddr = static_cast<PTR(void)>(static_cast<int32_t>(ovl.text_start));
    mesg.devAddr = ovl.rom_start;
    mesg.size = size;

    if (!execute_pi_dma_transfer(
            rdram, "ovl_dma", mesg, n, PiDmaCompletion::RetQueueMesg, kOverlayIoMesg, true)) {
        ultramodern::boot_log("[wr64] GameLoad_LoadOverlay HLE #%u: DMA skipped/failed\n", n);
    }

    if (ovl.bss_end > ovl.bss_start) {
        std::memset(
            wr64_rdram_byte_ptr(rdram, ovl.bss_start),
            0,
            static_cast<size_t>(ovl.bss_end - ovl.bss_start));
    }

    recomp::overlays::add_loaded_function(kOverlayVram, wr64_dispatch_802C5800);
    (void)ctx;
}

extern "C" void wr64_codeSEG_8004A130(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    recomp_context init_ctx{};
    init_ctx.r4 = kPiEventsMesgQueue;
    init_ctx.r5 = kContMaskByte;
    init_ctx.r6 = kContStatusArray;
    osContInit_recomp(rdram, &init_ctx);

    uint8_t mask = n64_mem8(rdram, kContMaskByte);
    int32_t count = 0;
    uint8_t bit = 1;
    for (int i = 0; i < 4; ++i) {
        if (mask & bit) {
            n64_mem32(rdram, kContIndexMap + static_cast<uint32_t>(count) * 4u) =
                static_cast<uint32_t>(i);
            ++count;
        }
        bit = static_cast<uint8_t>(bit << 1);
    }
    if (count == 0) {
        // Host osContInit can report mask=0 before SDL input is polled; WR64 title logic
        // gates on D_80154344 and overlay code checks D_80154344==0.
        mask = 1;
        n64_mem8(rdram, kContMaskByte) = mask;
        n64_mem32(rdram, kContIndexMap) = 0u;
        count = 1;
    }
    n64_mem32(rdram, kContCountGlobal) = static_cast<uint32_t>(count);

    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log(
            "[wr64] SysUtils_ContInitialize: mask=0x%02X controllers=%d%s\n",
            mask,
            count,
            count == 1 && mask == 1 ? " (default port0)" : "");
    }
}

extern "C" void wr64_codeSEG_80048854(uint8_t* rdram, recomp_context* ctx) {
    wr64_seed_framebuffers_if_needed(rdram);
    static std::atomic<uint32_t> call_count{0};
    const uint32_t n = call_count.fetch_add(1) + 1;
    if (n <= 8) {
        ultramodern::boot_log("[wr64] SysUtils_MatrixLookAt HLE impl #%u\n", n);
    }

    // Port of refs/Wave-Race-64/src/sys/sys_utils.c:SysUtils_MatrixLookAt.
    // Signature: (Mtx* out_mtx, MtxF* scratch_mtxf,
    //            f32 dir_x, f32 dir_y, f32 dir_z,
    //            f32 up_x,  f32 up_y,  f32 up_z,
    //            f32 pos_x, f32 pos_y, f32 pos_z)
    const uint32_t out_mtx = static_cast<uint32_t>(ctx->r4);
    const uint32_t scratch_mtxf = static_cast<uint32_t>(ctx->r5);
    if (!is_kseg0_rdram_pointer(out_mtx) || !is_kseg0_rdram_pointer(scratch_mtxf)) {
        return;
    }

    auto arg_f = [&](uint32_t u32bits) -> float {
        union {
            float f32;
            uint32_t u32;
        } v;
        v.u32 = u32bits;
        return v.f32;
    };

    const float dir_x = arg_f(static_cast<uint32_t>(ctx->r6));
    const float dir_y = arg_f(static_cast<uint32_t>(ctx->r7));
    const float dir_z = arg_f(MEM_W(0x10, ctx->r29));
    const float up_x = arg_f(MEM_W(0x14, ctx->r29));
    const float up_y = arg_f(MEM_W(0x18, ctx->r29));
    const float up_z = arg_f(MEM_W(0x1C, ctx->r29));
    const float pos_x = arg_f(MEM_W(0x20, ctx->r29));
    const float pos_y = arg_f(MEM_W(0x24, ctx->r29));
    const float pos_z = arg_f(MEM_W(0x28, ctx->r29));

    float* mf = reinterpret_cast<float*>(n64_vram(rdram, scratch_mtxf));

    auto sq = [](float x) { return x * x; };
    float temp = sq(dir_x) + sq(dir_y) + sq(dir_z);
    if (temp == 0.0f) {
        return;
    }
    temp = 1.0f / std::sqrt(temp);

    // mf[2][0..2] = normalized dir.
    mf[2 * 4 + 0] = temp * dir_x;
    mf[2 * 4 + 1] = temp * dir_y;
    mf[2 * 4 + 2] = temp * dir_z;

    // Right = up × dir.
    const float sp50 = (up_y * dir_z) - (up_z * dir_y);
    const float sp4c = (up_z * dir_x) - (up_x * dir_z);
    const float sp48 = (up_x * dir_y) - (up_y * dir_x);
    temp = sq(sp50) + sq(sp4c) + sq(sp48);
    if (temp == 0.0f) {
        return;
    }
    temp = 1.0f / std::sqrt(temp);
    mf[0 * 4 + 0] = temp * sp50;
    mf[0 * 4 + 1] = temp * sp4c;
    mf[0 * 4 + 2] = temp * sp48;

    // Up2 = dir × right.
    const float sp44 = (dir_y * sp48) - (dir_z * sp4c);
    const float sp40 = (dir_z * sp50) - (dir_x * sp48);
    const float sp3c = (dir_x * sp4c) - (dir_y * sp50);
    temp = sq(sp44) + sq(sp40) + sq(sp3c);
    if (temp == 0.0f) {
        return;
    }
    temp = 1.0f / std::sqrt(temp);
    mf[1 * 4 + 0] = temp * sp44;
    mf[1 * 4 + 1] = temp * sp40;
    mf[1 * 4 + 2] = temp * sp3c;

    // Translation comes from args 8..A in this function.
    mf[3 * 4 + 0] = pos_x;
    mf[3 * 4 + 1] = pos_y;
    mf[3 * 4 + 2] = pos_z;

    // Last column/row.
    mf[0 * 4 + 3] = 0.0f;
    mf[1 * 4 + 3] = 0.0f;
    mf[2 * 4 + 3] = 0.0f;
    mf[3 * 4 + 3] = 1.0f;

    // Convert float matrix into fixed-point Mtx.
    recomp_context conv{};
    conv.r4 = out_mtx;
    conv.r5 = scratch_mtxf;
    wr64_codeSEG_80047EE0(rdram, &conv);
}

extern "C" void wr64_codeSEG_80047EE0(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t dest = static_cast<uint32_t>(ctx->r4);
    const uint32_t src = static_cast<uint32_t>(ctx->r5);
    if (!is_kseg0_rdram_pointer(dest) || !is_kseg0_rdram_pointer(src)) {
        static std::atomic<uint32_t> skip_count{0};
        const uint32_t n = skip_count.fetch_add(1) + 1;
        if (n <= 8) {
            ultramodern::boot_log(
                "[wr64] SysUtils_MtxFToMtx skip dest=0x%08X src=0x%08X\n",
                dest,
                src);
        }
        return;
    }
    // 4x4 float matrix -> fixed-point Mtx (matches decomp SysUtils_MtxFToMtx).
    float* mf = reinterpret_cast<float*>(n64_vram(rdram, src));
    int16_t* mtx = reinterpret_cast<int16_t*>(n64_vram(rdram, dest));
    for (int i = 3; i >= 0; --i) {
        for (int j = 3; j >= 0; --j) {
            const int32_t temp = static_cast<int32_t>(mf[i * 4 + j] * 65536.0f);
            mtx[i * 8 + j] = static_cast<int16_t>(temp >> 16);
            mtx[i * 8 + j + 16] = static_cast<int16_t>(temp & 0xFFFF);
        }
    }
}

extern "C" void wr64_codeSEG_800474E4(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    uint32_t task_vaddr = g_wr64_pending_main_gfx_task.exchange(0, std::memory_order_acq_rel);
    if (!is_kseg0_rdram_pointer(task_vaddr)) {
        wr64_prepare_gfx_task_for_main(rdram);
        task_vaddr = n64_mem32(rdram, kCurrentGfxTaskGlobal);
    }
    if (!is_kseg0_rdram_pointer(task_vaddr)) {
        static std::atomic<uint32_t> skip_count{0};
        const uint32_t n = skip_count.fetch_add(1) + 1;
        if (n <= 16) {
            ultramodern::boot_log(
                "[wr64] gfx: skip func_800474E4 (gCurrentGfxTask=0x%08X)\n",
                task_vaddr);
        }
        return;
    }
    static std::atomic<uint32_t> dispatch_count{0};
    const uint32_t n = dispatch_count.fetch_add(1) + 1;
    if (n <= 16) {
        const uint32_t dl_ptr = n64_mem32(rdram, task_vaddr + 0x30u);
        const uint32_t dl_size = n64_mem32(rdram, task_vaddr + 0x34u);
        ultramodern::boot_log(
            "[wr64] Main SpTaskStartGo #%u task=0x%08X dl=0x%08X data_size=0x%X\n",
            n,
            task_vaddr,
            dl_ptr,
            dl_size);
    }
    recomp_context go{};
    go.r4 = task_vaddr;
    wr64::set_gfx_workload_busy(true);
    wr64_osSpTaskStartGo(rdram, &go);
    n64_mem32(rdram, 0x800D4600u) = 3u;
    n64_mem32(rdram, 0x800D4604u) = 1u;
}

extern "C" void wr64_codeSEG_80047B00(uint8_t* rdram, recomp_context* ctx) {
    wr64_codeSEG_800BF370(rdram, ctx);
}

// SysAudio_AudioThreadEntry HLE — VI consumer + audio task pump (init runs from first Gfx task).
extern "C" void wr64_codeSEG_80047B20(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    static std::atomic<uint32_t> tick_count{0};
    static uint32_t s_audio_task_vaddr = 0;

    while (true) {
        if (g_wr64_audio_init_pending.exchange(false, std::memory_order_acq_rel)) {
            wr64_ensure_audio_mesg_queue_pointers(rdram);
            ultramodern::boot_log("[wr64] Audio thread: AudioLoad_Init (0x800BA100)\n");
            recomp_context init_ctx{};
            codeSEG_800BA100(rdram, &init_ctx);
            g_wr64_audio_heap_ready.store(true, std::memory_order_release);
            ultramodern::boot_log("[wr64] Audio thread: AudioLoad_Init complete\n");
            continue;
        }

        recomp_context recv_ctx{};
        recv_ctx.r4 = kBootMqVi;
        recv_ctx.r5 = kAudioTaskMesgOut;
        recv_ctx.r6 = OS_MESG_NOBLOCK;
        wr64_bridge_osRecvMesg(rdram, &recv_ctx);
        if (static_cast<int32_t>(recv_ctx.r2) != 0) {
            ultramodern::run_next_thread_and_wait(rdram);
            continue;
        }

        if (!g_wr64_audio_heap_ready.load(std::memory_order_acquire)) {
            continue;
        }

        const uint32_t n = tick_count.fetch_add(1) + 1;

        if (s_audio_task_vaddr != 0) {
            n64_mem32(rdram, kCurrentAudioTask) = s_audio_task_vaddr;
            recomp_context notify_ctx{};
            notify_ctx.r4 = kMainThreadMesgQueue;
            notify_ctx.r5 = 0x16u;
            notify_ctx.r6 = OS_MESG_NOBLOCK;
            wr64_bridge_osSendMesg(rdram, &notify_ctx);
            if (n <= 16u) {
                ultramodern::boot_log(
                    "[wr64] Audio thread: dispatch prior task 0x%08X -> Main msg 0x16\n",
                    s_audio_task_vaddr);
            }
        }

        recomp_context create_ctx{};
        codeSEG_800C4C40(rdram, &create_ctx);
        s_audio_task_vaddr = static_cast<uint32_t>(create_ctx.r2);

        if (n <= 16u) {
            uint32_t msg = 0;
            if (is_kseg0_rdram_pointer(kAudioTaskMesgOut)) {
                msg = n64_mem32(rdram, kAudioTaskMesgOut);
            }
            ultramodern::boot_log(
                "[wr64] Audio thread tick #%u msg=0x%08X new_task=0x%08X\n",
                n,
                msg,
                s_audio_task_vaddr);
        }
    }
}

extern "C" void wr64_codeSEG_800BF370(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    if (n64_mem32(rdram, kSequenceChannelPtr) == 0) {
        n64_mem32(rdram, kSequenceChannelPtr) = kSequenceChannelNone;
    }
    // n_alSynRemovePlayer is not safe until audio is live; skip the stock body.
}

extern "C" void wr64_codeSEG_800C2FDC(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

extern "C" void wr64_codeSEG_800C3034(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

extern "C" void wr64_codeSEG_800C3044(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
}

#if 0  // Legacy func_80091F50 HLE — stock recompiled path used.
extern "C" void wr64_codeSEG_80091F50(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    wr64_seed_segment_globals_if_needed(rdram);
    constexpr uint32_t kPool = 0x801CE6B0u;
    constexpr uint32_t kGfxPoolIdxLocal = 0x8011F8E0u;
    constexpr uint32_t kGfxMatrixBase = 0x801CB6C8u;
    constexpr uint32_t kGfxMatrixSecondary = 0x801CCE60u;
    constexpr uint32_t kGfxChunkAligned = 0x8290u;
    const uint32_t seg84 = n64_mem32(rdram, 0x80151984u);
    n64_mem32(rdram, kPool + 0x00u) = 0;
    n64_mem32(rdram, kPool + 0x04u) = seg84;
    n64_mem32(rdram, kPool + 0x08u) = 0;
    n64_mem32(rdram, kPool + 0x0Cu) = 0;
    const uint32_t seg_ac = n64_mem32(rdram, 0x801519ACu);
    if (seg_ac != 0) {
        n64_mem32(rdram, kPool + 0x10u) = seg_ac;
    }
    n64_mem32(rdram, kPool + 0x14u) = 0;
    n64_mem32(rdram, kPool + 0x18u) = 0;
    n64_mem32(rdram, kPool + 0x1Cu) = 0;
    n64_mem32(rdram, kPool + 0x20u) = 0;
    n64_mem32(rdram, kPool + 0x24u) = 0;
    n64_mem32(rdram, kPool + 0x34u) = 0x800D45F0u; // osVirtualToPhysical stub: KSEG0
    n64_mem32(rdram, kPool + 0x38u) = 0;
    n64_mem32(rdram, kPool + 0x48u) = 0x800D45E4u;
    n64_mem32(rdram, kPool + 0x4Cu) = 0x800D45E8u;
    if (seg84 != 0) {
        n64_mem32(rdram, 0x800DCE90u) = seg84 + kGfxChunkAligned;
    }
    const uint32_t matrix_slot = n64_mem32(rdram, kGfxPoolIdxLocal) & 1u;
    n64_mem32(rdram, 0x801CE5F8u) = kGfxMatrixBase + matrix_slot * 0x17A0u;
    n64_mem32(rdram, 0x801CE5FCu) = 0;
    n64_mem32(rdram, 0x801CE628u) = 0;
    n64_mem32(rdram, 0x801CE62Cu) = 0;
    n64_mem32(rdram, 0x801CE72Eu) = 1;

    // Stock func_80091F50 calls guOrtho twice; without projection matrices every tri is clipped.
    float mf[4][4];
    wr64_guOrthoF_impl(mf, 0.0f, 319.0f, 239.0f, 0.0f, 100.0f, -100.0f, 1.0f);
    wr64_guMtxF2L(rdram, kGfxMatrixBase, mf);
    wr64_guMtxF2L(rdram, kGfxMatrixSecondary, mf);
    wr64_guMtxF2L(rdram, kGfxMatrixBase + matrix_slot * 0x17A0u, mf);

    static std::atomic<bool> logged{false};
    if (!logged.exchange(true)) {
        ultramodern::boot_log("[wr64] func_80091F50 HLE: guOrtho projection + D_800DCE90 applied\n");
    }
}
#endif

// PI manager BSS access queue (0x801DAC78): grant cart access when a thread blocks on osPiGetAccess.
bool grant_bss_pi_access(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kPiAccessQueue)) {
        return false;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kPiAccessQueue));
    if (mq->msgCount != 1 || mq->validCount != 0) {
        return false;
    }
    static std::atomic<uint32_t> grant_count{0};
    const uint32_t n = grant_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log( "[wr64] bss_pi_grant #%u queue=0x%08X\n", n, kPiAccessQueue);
    }
    wr64_deliver_mesg_no_wake(
        rdram, static_cast<uint32_t>(kPiAccessQueue), static_cast<OSMesg>(0));
    return true;
}

void pi_interrupt_notify(uint8_t* rdram) {
    ensure_pi_aux_mesg_queues(rdram);
    if (!is_kseg0_rdram_pointer(kPiEventsMesgQueue)) {
        return;
    }
    static std::atomic<uint32_t> interrupt_count{0};
    const uint32_t n = interrupt_count.fetch_add(1) + 1;
    if (n <= 64) {
        ultramodern::boot_log( "[wr64] pi_interrupt #%u -> queue=0x%08X\n", n, kPiEventsMesgQueue);
    }
    wr64_deliver_mesg_no_wake(rdram, kPiEventsMesgQueue, kPiEventMesg);
}

constexpr OSMesg kSiEventMesg = static_cast<OSMesg>(static_cast<int32_t>(0xB));

void si_interrupt_notify(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kPiEventsMesgQueue)) {
        return;
    }
    static std::atomic<uint32_t> si_count{0};
    const uint32_t n = si_count.fetch_add(1) + 1;
    if (n <= 64) {
        ultramodern::boot_log("[wr64] si_interrupt #%u -> queue=0x%08X\n", n, kPiEventsMesgQueue);
    }
    wr64_deliver_mesg_no_wake(rdram, kPiEventsMesgQueue, kSiEventMesg);
}

bool is_pif_cmd_buffer(uint32_t cmd_buf) {
    // __osContPifRam @ kPiCmdBuf; osContInit / EEP paths also use __osEepPifRam @ 0x801DACB0.
    constexpr uint32_t kEepPifRam = 0x801DACB0u;
    return (cmd_buf >= kPiCmdBuf && cmd_buf < kPiCmdBuf + 0x40u) ||
           (cmd_buf >= kEepPifRam && cmd_buf < kEepPifRam + 0x40u);
}

static uint32_t resolve_game_dma_dram(uint8_t* rdram, uint32_t rom_addr, uint32_t dram_hint) {
    uint32_t dram = static_cast<uint32_t>(normalize_dram_vaddr(static_cast<gpr>(dram_hint)));
    if (dram >= 0x80150000u && dram < 0x80800000u) {
        return dram;
    }
    wr64_seed_segment_globals_if_needed(rdram);
    constexpr uint32_t kCodesegRam = 0x801DAFA0u;
    if (rom_addr == 0x000A95D0u) {
        return kCodesegRam;
    }
    if (rom_addr >= 0x000FE320u) {
        const uint32_t seg = n64_mem32(rdram, 0x8015198Cu);
        if (seg >= 0x80150000u) {
            return static_cast<uint32_t>(normalize_dram_vaddr(static_cast<gpr>(seg)));
        }
        return 0x802310A0u;
    }
    if (rom_addr >= 0x000F6090u) {
        const uint32_t seg = n64_mem32(rdram, 0x80151984u);
        if (seg >= 0x80150000u) {
            return static_cast<uint32_t>(normalize_dram_vaddr(static_cast<gpr>(seg)));
        }
        return 0x80228E10u;
    }
    if (dram == 0x80000000u) {
        return 0;
    }
    return dram;
}

void pi_dma_notify_ret_queue(uint8_t* rdram, uint32_t mq, uint32_t mesg_vaddr) {
    if (mq == 0 || !is_kseg0_rdram_pointer(mq) || !is_kseg0_rdram_pointer(mesg_vaddr)) {
        return;
    }
    static std::atomic<uint32_t> completion_count{0};
    const uint32_t n = completion_count.fetch_add(1) + 1;
    if (n <= 64) {
        ultramodern::boot_log(
            "[wr64] pi_dma complete #%u -> retQueue=0x%08X mesg=0x%08X\n",
            n,
            mq,
            mesg_vaddr);
    }
    ultramodern::deliver_mesg_immediate(rdram, mq, static_cast<OSMesg>(static_cast<int32_t>(mesg_vaddr)));
}

void ensure_pi_device_ready(uint8_t* rdram, recomp_context* ctx) {
    // PI manager thread stays deferred during boot; cart DMA is handled by wr64_osPiRawStartDma HLE.
    // codeSEG_800C5A00 here races SysMain init and corrupts Main thread state.
    (void)rdram;
    (void)ctx;
}

bool execute_pi_dma_transfer(
    uint8_t* rdram,
    const char* tag,
    const OSIoMesg& mb,
    uint32_t call_index,
    PiDmaCompletion completion,
    uint32_t mesg_vaddr,
    bool apply_overlays = true) {
    const gpr dram_addr = normalize_dram_vaddr(static_cast<gpr>(static_cast<int32_t>(mb.dramAddr)));
    const uint32_t size = mb.size;
    const uint32_t mq = static_cast<uint32_t>(mb.hdr.retQueue);
    const uint32_t physical = resolve_cart_physical(rdram, mb);
    const uint32_t rom_offset =
        physical >= recomp::rom_base ? physical - recomp::rom_base : physical;

    const uint32_t dram_u32 = static_cast<uint32_t>(dram_addr);

    if (call_index <= 128) {
        ultramodern::boot_log(
            "[wr64] %s #%u rom_offset=0x%08X rom_phys=0x%08X rdram=0x%08X size=0x%08X "
            "retQueue=0x%08X devAddr=0x%08X piHandle=0x%08X\n",
            tag,
            call_index,
            rom_offset,
            physical,
            dram_u32,
            size,
            mq,
            mb.devAddr,
            mb.piHandle);
    }

    if (size == 0) {
        return false;
    }

    constexpr uint64_t kRdramBytes = 8u * 1024u * 1024u;
    if (!is_kseg0_rdram_pointer(dram_u32)) {
        ultramodern::boot_log(
            "[wr64] %s #%u: invalid dram vaddr 0x%08X (size 0x%08X) — skipped\n",
            tag,
            call_index,
            dram_u32,
            size);
        return false;
    }
    const uint64_t rdram_off = wr64_rdram_byte_offset(dram_u32);
    if (rdram_off >= kRdramBytes || size > kRdramBytes - rdram_off) {
        ultramodern::boot_log(
            "[wr64] %s #%u: RDRAM write out of bounds dram=0x%08X size=0x%08X "
            "(rdram_off=0x%llX limit=0x%llX) — skipped\n",
            tag,
            call_index,
            dram_u32,
            size,
            static_cast<unsigned long long>(rdram_off),
            static_cast<unsigned long long>(kRdramBytes));
        return false;
    }

    if (physical < recomp::rom_base) {
        ultramodern::boot_log(
            "[wr64] %s #%u: non-ROM phys 0x%08X (size 0x%08X) — skipped\n",
            tag,
            call_index,
            physical,
            size);
        return false;
    }

    recomp::do_rom_read(rdram, dram_addr, physical, size);
    // ROM is big-endian; recompiled MEM_W on x86 reads host-endian words from RDRAM.
    // Swap each loaded word so lw/sw agree with DMA'd segment data (pointer tables, etc.).
    {
        const uint64_t rdram_off = wr64_rdram_byte_offset(static_cast<uint32_t>(dram_addr));
        uint8_t* const dst = rdram + rdram_off;
        for (size_t i = 0; i + 4 <= size; i += 4) {
            uint8_t* const p = dst + i;
            const uint8_t b0 = p[0];
            p[0] = p[3];
            p[3] = b0;
            const uint8_t b1 = p[1];
            p[1] = p[2];
            p[2] = b1;
        }
    }
    if (apply_overlays && call_index <= 8 && size >= 4) {
        const std::span<const uint8_t> rom_span = recomp::get_rom();
        const size_t rom_off = physical - recomp::rom_base;
        if (rom_off + 4 <= rom_span.size()) {
            const uint32_t rom_word =
                (static_cast<uint32_t>(rom_span[rom_off + 0]) << 24) |
                (static_cast<uint32_t>(rom_span[rom_off + 1]) << 16) |
                (static_cast<uint32_t>(rom_span[rom_off + 2]) << 8) |
                static_cast<uint32_t>(rom_span[rom_off + 3]);
            const uint32_t rdram_word = n64_mem32(rdram, static_cast<uint32_t>(dram_addr));
            ultramodern::boot_log(
                "[wr64] %s #%u: dma verify rdram=0x%08X first_word host=0x%08X rom=0x%08X %s\n",
                tag,
                call_index,
                dram_u32,
                rdram_word,
                rom_word,
                rdram_word == rom_word ? "OK" : "MISMATCH");
        }
    }
    if (apply_overlays) {
        const uint32_t dram_u = static_cast<uint32_t>(dram_addr);
        if (dram_u == kOverlayVram) {
            unload_overlays(static_cast<int32_t>(kOverlayVram), kOverlayBankUnloadSize);
        } else if (dram_u == kProgOverlayVram || dram_u == 0x80316800u || dram_u == 0x802D6800u ||
            dram_u == 0x8029A200u || dram_u == 0x80306800u) {
            unload_overlays(static_cast<int32_t>(dram_u), size);
        }
        load_overlays(rom_offset, static_cast<int32_t>(dram_addr), size);
        if (call_index <= 128) {
            ultramodern::boot_log(
                "[wr64] %s #%u: overlay load rom=[0x%08X,0x%08X) -> rdram=0x%08X\n",
                tag,
                call_index,
                rom_offset,
                rom_offset + size,
                static_cast<uint32_t>(dram_addr));
        }
        if (static_cast<uint32_t>(dram_addr) == 0x801DAFA0u && call_index > 3) {
            ultramodern::boot_log(
                "[wr64] %s #%u: SysMain GameLoad codeseg reload\n", tag, call_index);
        }
        if (static_cast<uint32_t>(dram_addr) == 0x801DAFA0u) {
            wr64_fixup_codeseg_gap_bss(rdram);
        }
        if (rom_offset == 0x000F6090u || static_cast<uint32_t>(dram_addr) == wr64_default_codeseg84()) {
            wr64_seed_segment_globals_if_needed(rdram);
            wr64_seed_dl_ptr_table_if_needed(rdram);
        }
    }
    // boot_dma1 display-list path dereferences pointer table @ 0x802260BC (codeseg offset 0x4B11C).
    constexpr uint32_t kDlPtrTable = 0x802260BCu;
    {
        const uint32_t dram_u = static_cast<uint32_t>(dram_addr);
        if (dram_u <= kDlPtrTable && dram_u + size > kDlPtrTable && call_index <= 16) {
        const uint32_t table0 = n64_mem32(rdram, kDlPtrTable);
        ultramodern::boot_log(
            "[wr64] %s #%u: DL ptr table[0] @0x%08X = 0x%08X (ROM expect 0x80226210)\n",
            tag,
            call_index,
            kDlPtrTable,
            table0);
        }
    }

    switch (completion) {
        case PiDmaCompletion::None:
            break;
        case PiDmaCompletion::PiInterrupt:
            pi_interrupt_notify(rdram);
            break;
        case PiDmaCompletion::RetQueueMesg:
            pi_dma_notify_ret_queue(rdram, mq, mesg_vaddr);
            break;
    }
    return true;
}

bool wr64_pi_mgr_queue(uint32_t queue) {
    constexpr uint32_t kPiAccessMesgQueue = kSeg801E - 0x5368u; // 0x801DAC98
    return queue == kPiStackMesgQueue || queue == kPiAccessMesgQueue ||
           queue == kPiAccessQueue || queue == kPiEventsMesgQueue ||
           queue == kPiAccessClientQueue;
}

bool wr64_mesg_recv_fast_queue(uint32_t queue) {
    // Thread-stack embedded 1-slot PI access queues (e.g. 0x801530C8).
    if (queue >= 0x80152000u && queue < 0x80154000u) {
        return true;
    }
    // VI hardware inject writes queue fields without mesg_queue_mutex.
    if (queue == kViEventQueue || queue == kMainThreadMesgQueue) {
        return true;
    }
    if (is_boot_mesg_queue(queue)) {
        return true;
    }
    return wr64_pi_mgr_queue(queue) || is_overlay_loader_mesg_queue(queue) ||
           queue == kPiCmdQueue;
}

// Game PI access helpers embed a 1-slot queue on the caller's stack (msg buf just above the head).
bool wr64_try_grant_stack_pi_access(uint8_t* rdram, uint32_t queue) {
    if (!is_kseg0_rdram_pointer(queue)) {
        return false;
    }
    // Only thread-stack addresses — never BSS queues like 0x801540xx.
    if (queue < 0x80152000u || queue >= 0x80154000u) {
        return false;
    }
    if (is_overlay_loader_mesg_queue(queue) || wr64_pi_mgr_queue(queue) ||
        queue == kPiCmdQueue || queue == kBootMqVi || queue == kBootMqEvents ||
        queue == kBootMqMainSync || queue == kMainThreadMesgQueue) {
        return false;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
    if (mq->msgCount != 1 || mq->validCount != 0) {
        return false;
    }
    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<int32_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf) || msg_buf < queue || msg_buf > queue + 0x80u) {
        return false;
    }
    static std::atomic<uint32_t> grant_count{0};
    const uint32_t n = grant_count.fetch_add(1) + 1;
    if (n <= 16) {
        ultramodern::boot_log(
            "[wr64] stack_pi_grant #%u queue=0x%08X msg=0x%08X\n",
            n,
            queue,
            msg_buf);
    }
    ultramodern::deliver_mesg_immediate(
        rdram, static_cast<PTR(OSMesgQueue)>(queue), static_cast<OSMesg>(0));
    return true;
}

bool wr64_mesg_recv_fast(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t queue = static_cast<uint32_t>(ctx->r4);
    if (!wr64_mesg_recv_fast_queue(queue) || !is_kseg0_rdram_pointer(queue)) {
        return false;
    }
    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
    if (mq->validCount <= 0) {
        return false;
    }
    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<int32_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        return false;
    }
    auto peek_slot = [&]() -> uint32_t {
        return n64_mem32(rdram, msg_buf + static_cast<uint32_t>(mq->first) * sizeof(uint32_t));
    };
    auto queue_contains = [&](uint32_t target) -> bool {
        for (s32 i = 0; i < mq->validCount; ++i) {
            const s32 idx = (mq->first + i) % mq->msgCount;
            if (n64_mem32(rdram, msg_buf + static_cast<uint32_t>(idx) * sizeof(uint32_t)) == target) {
                return true;
            }
        }
        return false;
    };
    auto rotate_to_message = [&](uint32_t target) -> bool {
        for (s32 i = 0; i < mq->validCount; ++i) {
            const s32 idx = (mq->first + i) % mq->msgCount;
            if (n64_mem32(rdram, msg_buf + static_cast<uint32_t>(idx) * sizeof(uint32_t)) == target) {
                mq->first = idx;
                return true;
            }
        }
        return false;
    };
    uint32_t peek = peek_slot();
    // Stock Main_Thread expects SP/DP (and gfx-task set) before VI retraces. Host VI
    // inject can enqueue 0x19 ahead of deferred RSP completions; never consume 0x19 while
    // a gfx task is in flight or DP is still pending in the queue.
    if (queue == kMainThreadMesgQueue) {
        // Drop stray zero slots (PI grant noise) so they do not wedge the VI loop.
        while (mq->validCount > 0 && peek_slot() == 0u) {
            mq->first = (mq->first + 1) % mq->msgCount;
            mq->validCount--;
        }
        if (mq->validCount <= 0) {
            return false;
        }
        peek = peek_slot();
        if (peek == 0x19u) {
            // Stock order: SP (0x17) then DP (0x18) before VI (0x19).
            if (rotate_to_message(0x17u) || rotate_to_message(0x18u) || rotate_to_message(0x15u)) {
                peek = peek_slot();
            }
        }
        const bool gfx_in_flight = n64_mem32(rdram, kGfxTaskActiveGlobal) != 0;
        const bool dp_pending = queue_contains(0x18u);
        const bool awaiting_dp = n64_mem32(rdram, kGfxTaskDpPendingGlobal) != 0;
        if (peek == 0x15u && gfx_in_flight) {
            if (rotate_to_message(0x17u) || rotate_to_message(0x18u)) {
                peek = peek_slot();
            } else {
                return false;
            }
        }
        if (peek == 0x19u && (gfx_in_flight || dp_pending || awaiting_dp)) {
            ultramodern::drain_external_messages(rdram);
            ultramodern::process_deferred_rsp_completions(rdram);
            if (rotate_to_message(0x17u) || rotate_to_message(0x18u) || rotate_to_message(0x15u)) {
                peek = peek_slot();
            } else if (peek == 0x19u &&
                       (gfx_in_flight || awaiting_dp || queue_contains(0x18u))) {
                static std::atomic<uint32_t> defer_vi_count{0};
                const uint32_t n = defer_vi_count.fetch_add(1) + 1;
                if (n <= 32) {
                    ultramodern::boot_log(
                        "[wr64] main_mq: defer VI 0x19 until SP/DP (task_active=%u dp_pending=%u)\n",
                        n64_mem32(rdram, kGfxTaskActiveGlobal),
                        n64_mem32(rdram, kGfxTaskDpPendingGlobal));
                }
                return false;
            }
        }
    }
    const uint32_t received = peek;
    mq->first = (mq->first + 1) % mq->msgCount;
    mq->validCount--;
    wr64_clear_sysmain_event_pending_on_consume(queue, received);
    if (queue == kMainThreadMesgQueue && received == 0x18u) {
        // DP completion can arrive before RT64 finishes copyNativeToRAM; Gfx-active clears in finish_gfx_task.
        n64_mem32(rdram, kGfxTaskDpPendingGlobal) = 0u;
        n64_mem32(rdram, 0x800D4600u) = 0u;
        // Stock Main posts 0x33 to kBootMqMainSync after DP; ensure SysMain is not wedged if
        // deferred delivery lagged behind gfx completion (dispatch #6+ race).
        if (is_kseg0_rdram_pointer(kBootMqMainSync)) {
            auto* sync_mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, kBootMqMainSync));
            if (sync_mq->validCount == 0 && !wr64::gfx_pipeline_busy(rdram)) {
                wr64_deliver_mesg_no_wake(
                    rdram, kBootMqMainSync, kBootMainSyncBootstrapMesg);
            }
        }
    }
    if (queue == kMainThreadMesgQueue) {
        static std::atomic<uint32_t> main_recv_count{0};
        const uint32_t n = main_recv_count.fetch_add(1) + 1;
        if (n <= 24) {
            ultramodern::boot_log(
                "[wr64] main_mq recv_fast #%u msg=0x%08X remain=%d\n",
                n,
                received,
                static_cast<int>(mq->validCount));
        }
    }
    const uint32_t msg_out = static_cast<uint32_t>(ctx->r5);
    if (msg_out != 0 && is_kseg0_rdram_pointer(msg_out)) {
        n64_mem32(rdram, msg_out) = received;
    }
    ctx->r2 = 0;
    return true;
}

bool wr64_mesg_send_noblock_fast(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<int32_t>(ctx->r6) != OS_MESG_NOBLOCK) {
        return false;
    }
    const uint32_t queue = static_cast<uint32_t>(ctx->r4);
    if (!is_kseg0_rdram_pointer(queue)) {
        return false;
    }
    // PI cmd queue needs mutex wake when the manager thread is blocked on recv.
    if (queue == kPiCmdQueue) {
        return false;
    }

    auto* mq = reinterpret_cast<OSMesgQueue*>(n64_vram(rdram, queue));
    if (mq->msgCount <= 0) {
        return false;
    }
    if (mq->validCount >= mq->msgCount) {
        ctx->r2 = -1;
        return true;
    }

    const uint32_t msg_buf = static_cast<uint32_t>(static_cast<int32_t>(mq->msg));
    if (!is_kseg0_rdram_pointer(msg_buf)) {
        return false;
    }

    const s32 slot = (mq->first + mq->validCount) % mq->msgCount;
    if (mq->blocked_on_recv != NULLPTR &&
        !is_embedded_empty_queue_sentinel(
            static_cast<uint32_t>(static_cast<intptr_t>(mq->blocked_on_recv)))) {
        return false;
    }

    n64_mem32(rdram, msg_buf + static_cast<uint32_t>(slot) * sizeof(uint32_t)) =
        static_cast<uint32_t>(ctx->r5);
    mq->validCount++;
    ctx->r2 = 0;
    return true;
}

void signal_pi_manager_stack(uint8_t* rdram) {
    if (!is_kseg0_rdram_pointer(kPiStackMesgQueue)) {
        return;
    }
    recomp_context ctx{};
    ctx.r4 = kPiStackMesgQueue;
    ctx.r5 = 0;
    ctx.r6 = OS_MESG_NOBLOCK;
    if (!wr64_mesg_send_noblock_fast(rdram, &ctx)) {
        osSendMesg(rdram, kPiStackMesgQueue, reinterpret_cast<OSMesg>(0), OS_MESG_NOBLOCK);
    }
}

uint16_t iomsg_type_field(uint8_t* rdram, uint32_t mesg_vaddr) {
    // Matches lhu 0($mesg) in codeSEG_800CC450 (high halfword of first word).
    return static_cast<uint16_t>(n64_mem32(rdram, mesg_vaddr) >> 16);
}

bool hle_pi_cmd_queue_mesg(uint8_t* rdram, uint32_t mesg_vaddr) {
    if (!is_kseg0_rdram_pointer(mesg_vaddr)) {
        return false;
    }
    const uint16_t type = iomsg_type_field(rdram, mesg_vaddr);
    if (type != 0xB && type != 0xC) {
        return false;
    }

    const OSIoMesg& mb =
        *reinterpret_cast<const OSIoMesg*>(n64_vram(rdram, mesg_vaddr));
    static std::atomic<uint32_t> count{0};
    const uint32_t n = count.fetch_add(1) + 1;
    if (n <= 64) {
        ultramodern::boot_log(
            "[wr64] pi_cmd_hle #%u type=%u dram=0x%08X dev=0x%08X size=0x%08X\n",
            n,
            type,
            static_cast<uint32_t>(mb.dramAddr),
            mb.devAddr,
            mb.size);
    }

    if (mb.size != 0) {
        execute_pi_dma_transfer(
            rdram,
            type == 0xC ? "pi_cmd_wr" : "pi_cmd_rd",
            mb,
            n,
            PiDmaCompletion::PiInterrupt,
            mesg_vaddr,
            true);
        const uint32_t ret_q = static_cast<uint32_t>(static_cast<int32_t>(mb.hdr.retQueue));
        if (is_overlay_loader_mesg_queue(ret_q)) {
            ultramodern::deliver_mesg_immediate_tail(
                rdram,
                static_cast<PTR(OSMesgQueue)>(ret_q),
                static_cast<OSMesg>(static_cast<int32_t>(mesg_vaddr)));
        }
    } else {
        pi_interrupt_notify(rdram);
    }

    return true;
}

extern "C" void wr64_pi_manager_dma_handler(uint8_t* rdram, recomp_context* ctx) {
    // DMA already ran in hle_pi_cmd_queue_mesg; post stack completion for __osDevMgrMain.
    (void)ctx;
    signal_pi_manager_stack(rdram);
    ctx->r2 = 0;
}

extern "C" void wr64_codeSEG_80097EC8(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t rom_addr = static_cast<uint32_t>(ctx->r4);
    const uint32_t dram_hint = static_cast<uint32_t>(ctx->r5);
    const uint32_t dram = resolve_game_dma_dram(rdram, rom_addr, dram_hint);
    const uint32_t size = static_cast<uint32_t>(ctx->r6);

    if (dram < 0x80150000u || dram >= 0x80800000u) {
        static std::atomic<uint32_t> bad_count{0};
        const uint32_t n = bad_count.fetch_add(1) + 1;
        if (n <= 16) {
            ultramodern::boot_log(
                "[wr64] game_dma: skip invalid dram hint=0x%08X resolved=0x%08X rom=0x%08X\n",
                dram_hint,
                dram,
                rom_addr);
        }
        ctx->r2 = 0;
        return;
    }

    {
        recomp_context inval = *ctx;
        inval.r4 = dram;
        inval.r5 = size;
        codeSEG_800CA2C0(rdram, &inval);
    }

    constexpr uint32_t kPiMq = 0x801540B8u;
    constexpr uint32_t kIoMesg = 0x801542A0u;
    OSIoMesg mesg{};
    mesg.hdr.retQueue = static_cast<PTR(OSMesgQueue)>(kPiMq);
    mesg.dramAddr = static_cast<PTR(void)>(static_cast<int32_t>(dram));
    mesg.devAddr = rom_addr;
    mesg.size = size;

    static std::atomic<uint32_t> game_dma_count{0};
    const uint32_t n = game_dma_count.fetch_add(1) + 1;
    ultramodern::boot_log(
        "[wr64] game_dma #%u rom=0x%08X dram=0x%08X size=0x%08X ra=0x%08X\n",
        n,
        rom_addr,
        dram,
        size,
        static_cast<uint32_t>(ctx->r31));
    if (n <= 8 || (n % 16) == 0) {
        wr64_log_boot_progress(rdram, "game_dma");
    }

    execute_pi_dma_transfer(
        rdram, "game_dma", mesg, 1000u + n, PiDmaCompletion::RetQueueMesg, kIoMesg);
    ctx->r2 = 0;
}

extern "C" void wr64_osPiStartDma(uint8_t* rdram, recomp_context* ctx) {
    // osPiStartDma(mesg, pri, direction, devAddr) with stack args:
    //   sp+0x10 = dramAddr, sp+0x14 = size, sp+0x18 = retQueue, sp+0x1c = piHandle
    const uint32_t mesg_vaddr = static_cast<uint32_t>(ctx->r4);
    const uint32_t priority = static_cast<uint32_t>(ctx->r5);
    const uint32_t direction = static_cast<uint32_t>(ctx->r6);
    const uint32_t dev_addr = static_cast<uint32_t>(ctx->r7);
    const uint32_t sp = static_cast<uint32_t>(ctx->r29);

    if (!is_kseg0_rdram_pointer(mesg_vaddr)) {
        ultramodern::boot_log("[wr64] pi_dma: invalid mesg pointer 0x%08X\n", mesg_vaddr);
        ctx->r2 = -1;
        return;
    }

    uint32_t dram_addr = n64_mem32(rdram, sp + 0x10u);
    uint32_t size = n64_mem32(rdram, sp + 0x14u);
    const uint32_t ret_queue = n64_mem32(rdram, sp + 0x18u);
    const uint32_t pi_handle = n64_mem32(rdram, sp + 0x1cu);

    uint32_t cart_dev = dev_addr;
    wr64_try_fixup_overlay_pi_dma(rdram, cart_dev, dram_addr, size);

    auto* mesg = reinterpret_cast<OSIoMesg*>(n64_vram(rdram, mesg_vaddr));
    mesg->hdr.status = 0;
    mesg->hdr.pri = static_cast<uint8_t>(priority);
    mesg->hdr.type = direction == 0 ? static_cast<uint16_t>(11) : static_cast<uint16_t>(12);
    mesg->hdr.retQueue = static_cast<PTR(OSMesgQueue)>(ret_queue);
    mesg->dramAddr = static_cast<PTR(void)>(static_cast<int32_t>(dram_addr));
    mesg->devAddr = cart_dev;
    mesg->size = size;
    mesg->piHandle = pi_handle;

    static std::atomic<uint32_t> dma_count{0};
    const uint32_t n = dma_count.fetch_add(1) + 1;

    if (n <= 128) {
        ultramodern::boot_log(
            "[wr64] osPiStartDma #%u mesg=0x%08X dev=0x%08X dram=0x%08X size=0x%08X queue=0x%08X\n",
            n,
            mesg_vaddr,
            cart_dev,
            dram_addr,
            size,
            ret_queue);
    }

    execute_pi_dma_transfer(rdram, "pi_dma", *mesg, n, PiDmaCompletion::RetQueueMesg, mesg_vaddr);
    ctx->r2 = 0;
}

extern "C" void wr64_osEPiStartDma(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mb_vaddr = static_cast<uint32_t>(ctx->r5);
    if (!is_kseg0_rdram_pointer(mb_vaddr)) {
        ultramodern::boot_log( "[wr64] epi_dma: invalid mesg pointer 0x%08X\n", mb_vaddr);
        ctx->r2 = -1;
        return;
    }

    static std::atomic<uint32_t> dma_count{0};
    const uint32_t n = dma_count.fetch_add(1) + 1;

    const OSIoMesg& mb = *reinterpret_cast<const OSIoMesg*>(n64_vram(rdram, mb_vaddr));
    execute_pi_dma_transfer(rdram, "epi_dma", mb, n, PiDmaCompletion::RetQueueMesg, mb_vaddr);
    ctx->r2 = 0;
}

extern "C" void wr64_osPiRawStartDma(uint8_t* rdram, recomp_context* ctx) {
    // WR64 codeSEG_800CB9C0: (direction, cmd_buf*) — not OSIoMesg*.
    const uint32_t direction = static_cast<uint32_t>(ctx->r4);
    const uint32_t cmd_buf = static_cast<uint32_t>(ctx->r5);

    static std::atomic<uint32_t> raw_count{0};
    const uint32_t n = raw_count.fetch_add(1) + 1;

    if (n <= 128) {
        ultramodern::boot_log(
            "[wr64] pi_raw #%u direction=%u cmd_buf=0x%08X (global=0x%08X)\n",
            n,
            direction,
            cmd_buf,
            kPiCmdBuf);
    }

    static std::atomic<uint32_t> si_raw_activity{0};

    if (direction == 1 && is_kseg0_rdram_pointer(cmd_buf)) {
        const uint8_t entry_count = n64_mem8(rdram, kPiCmdEntryCount);
        uint32_t transfers = 0;
        if (entry_count > 0) {
            uint32_t entry_vaddr = cmd_buf;
            for (uint8_t i = 0; i < entry_count; ++i) {
                const uint8_t* entry = n64_vram(rdram, entry_vaddr);
                if ((entry[6] & 0xC0u) == 0) {
                    const uint32_t size =
                        static_cast<uint32_t>(entry[4]) |
                        (static_cast<uint32_t>(entry[5]) << 8);
                    if (size != 0) {
                        OSIoMesg mb{};
                        mb.dramAddr = n64_mem32(rdram, entry_vaddr);
                        mb.devAddr = n64_mem32(rdram, entry_vaddr + 4);
                        mb.size = size;
                        execute_pi_dma_transfer(
                            rdram, "pi_raw", mb, n, PiDmaCompletion::None, entry_vaddr);
                        ++transfers;
                    }
                }
                entry_vaddr += 8;
            }
        }
        if (n <= 16) {
            const bool si_pif = entry_count == 0 && is_pif_cmd_buffer(cmd_buf);
            ultramodern::boot_log(
                "[wr64] pi_raw #%u direction=1 entries=%u transfers=%u cmd_buf=0x%08X%s\n",
                n,
                entry_count,
                transfers,
                cmd_buf,
                si_pif ? " (si_pif_cont)" : "");
        }
        // entry_count==0 on the PIF buffer is __osSiRawStartDma (osContInit), not PI manager batch DMA.
        if (entry_count == 0 && is_pif_cmd_buffer(cmd_buf)) {
            si_raw_activity.fetch_add(1, std::memory_order_relaxed);
            si_interrupt_notify(rdram);
        } else {
            pi_interrupt_notify(rdram);
            signal_pi_manager_stack(rdram);
        }
    } else if (direction == 0 && is_kseg0_rdram_pointer(cmd_buf)) {
        const uint8_t entry_count = n64_mem8(rdram, kPiCmdEntryCount);
        const bool si_path =
            (entry_count == 0 && is_pif_cmd_buffer(cmd_buf) &&
             si_raw_activity.load(std::memory_order_relaxed) > 0);
        if (!si_path && entry_count == 0 && cmd_buf == kPiCmdBuf) {
            // PI manager cart probe before any SI traffic (deferred manager / early boot).
            OSIoMesg mb{};
            mb.dramAddr = static_cast<gpr>(static_cast<int32_t>(cmd_buf));
            mb.devAddr = kPiRawInitRomOffset;
            mb.size = kPiRawInitReadSize;
            execute_pi_dma_transfer(rdram, "pi_raw_init", mb, n, PiDmaCompletion::None, 0, false);
            if (n <= 8) {
                ultramodern::boot_log(
                    "[wr64] pi_raw #%u: init read 0x%X bytes from ROM+0x%X -> 0x%08X\n",
                    n,
                    kPiRawInitReadSize,
                    kPiRawInitRomOffset,
                    cmd_buf);
            }
        }
        if (n <= 128) {
            ultramodern::boot_log(
                "[wr64] pi_raw #%u: completion step (direction=0 si=%u)\n", n, si_path ? 1u : 0u);
        }
        if (si_path) {
            si_interrupt_notify(rdram);
        } else if (n > 2) {
            pi_interrupt_notify(rdram);
        }
    }

    ctx->r2 = 0;
}

extern "C" void wr64_osPiGetAccess(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    static std::atomic<uint32_t> access_count{0};
    const uint32_t n = access_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log( "[wr64] pi_get_access #%u (non-blocking)\n", n);
    }
    ctx->r2 = 0;
}

extern "C" void wr64_osPiRelAccess(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> release_count{0};
    const uint32_t n = release_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log( "[wr64] pi_rel_access #%u queue=0x%08X\n", n, kPiAccessQueue);
    }
    grant_bss_pi_access(rdram);
    ctx->r2 = 0;
}

extern "C" void wr64_osSpTaskLoad(uint8_t* rdram, recomp_context* ctx) {
    (void)rdram;
    (void)ctx;
    // HLE: display-list task data already lives in RDRAM; skip hardware SP DMA.
}

extern "C" void wr64_osSpTaskStartGo(uint8_t* rdram, recomp_context* ctx) {
    gpr task_ptr = ctx->r4;
    uint32_t task_vaddr = static_cast<uint32_t>(task_ptr);
    if (!is_kseg0_rdram_pointer(task_vaddr)) {
        task_ptr = ctx->r6;
        task_vaddr = static_cast<uint32_t>(task_ptr);
    }
    if (!is_kseg0_rdram_pointer(task_vaddr)) {
        ctx->r2 = -1;
        return;
    }
    wr64_on_sp_task_started(rdram, task_ptr);
    ctx->r2 = 0;
}

extern "C" void wr64_on_enter_800C5C60(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t queue = static_cast<uint32_t>(ctx->r4);
    if (queue != kViEventQueue) {
        return;
    }
    std::lock_guard lock(g_timer_path_mutex);
    fix_mesg_queue_struct(rdram, queue);
    restore_timer_handler_fields(rdram);
}

extern "C" void wr64_on_enter_800CBF50(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    std::lock_guard lock(g_timer_path_mutex);
    restore_timer_handler_fields(rdram);
}

extern "C" void wr64_on_enter_800CB7D0(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    std::lock_guard lock(g_timer_path_mutex);
    restore_timer_handler_fields(rdram);
}

extern "C" void wr64_codeSEG_800CB7D0(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    std::lock_guard lock(g_timer_path_mutex);
    ensure_libultra_virt_count(rdram);
    init_libultra_timer_globals(rdram, false);
    restore_timer_handler_fields(rdram);
    ultramodern::boot_log( "[wr64] __osTimerInit (800CB7D0): native stub\n");
}

extern "C" int wr64_is_kseg0_pointer(gpr addr) {
    return is_kseg0_rdram_pointer(static_cast<uint32_t>(addr)) ? 1 : 0;
}

// libultra BSS lives around 0x800E8xxx; reject 0/0x80000000 which map to low RDRAM offsets.
extern "C" int wr64_is_safe_k0_store_target(gpr addr) {
    const uint32_t vaddr = static_cast<uint32_t>(addr);
    return (vaddr >= 0x800E8000u && vaddr < 0x80800000u) ? 1 : 0;
}

extern "C" void wr64_ensure_virt_count_k0(uint8_t* rdram, gpr* k0) {
    if (k0 == nullptr) {
        return;
    }
    ensure_libultra_virt_count(rdram);
    *k0 = static_cast<gpr>(static_cast<int32_t>(n64_mem32(rdram, kSeg800F - 0x6FF0)));
}

extern "C" void wr64_fixup_timer_handler_s0(uint8_t* rdram, gpr* s0) {
    uint32_t value = static_cast<uint32_t>(*s0);
    if (!is_timer_handler_pointer(value)) {
        const uint32_t handler = timer_handler_for_tv(rdram);
        const uint32_t active = n64_mem32(rdram, kSeg800F - 0x6F4C);
        if (is_kseg0_rdram_pointer(active)) {
            n64_mem32(rdram, active + 0x8) = handler;
        }
        *s0 = handler;
    }
}

extern "C" void wr64_fixup_timer_handler_t1(uint8_t* rdram, gpr* t1) {
    const uint32_t handler = timer_handler_for_tv(rdram);
    const uint32_t addr = static_cast<uint32_t>(*t1);
    constexpr uint32_t kHandlerTableBytes = 0x80u;
    if (addr < handler || addr >= handler + kHandlerTableBytes) {
        *t1 = handler;
    }
}

extern "C" void wr64_fixup_pi_callback(gpr* fn_ptr) {
    if (fn_ptr == nullptr) {
        return;
    }
    const uint32_t fn = static_cast<uint32_t>(*fn_ptr);
    if (fn == 0) {
        *fn_ptr = static_cast<gpr>(static_cast<int32_t>(kPiManagerHandler));
        ultramodern::boot_log(
            "[wr64] pi_manager: LOOKUP_FUNC(0) patched -> 0x%08X\n",
            kPiManagerHandler);
    }
}

extern "C" gpr wr64_safe_kseg0_load(uint8_t* rdram, gpr addr) {
    const uint32_t vaddr = static_cast<uint32_t>(addr);
    if (!is_kseg0_rdram_pointer(vaddr)) {
        return 0;
    }
    return static_cast<gpr>(n64_mem32(rdram, vaddr));
}

extern "C" gpr wr64_safe_thread_pri(uint8_t* rdram, gpr thread) {
    const uint32_t vaddr = static_cast<uint32_t>(thread);
    if (vaddr == 0 || !is_kseg0_rdram_pointer(vaddr)) {
        return 0;
    }
    return static_cast<gpr>(n64_mem32(rdram, vaddr + 4));
}

extern "C" void wr64_codeSEG_800C6DE0(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    ensure_libultra_virt_count(rdram);
    init_libultra_mmio_stubs(rdram);
    copy_osinit_ipl3_tables(rdram);
    ultramodern::boot_log( "[wr64] osInitialize: patched stub (virt+ipl3)\n");
}

extern "C" void wr64_codeSEG_800C6D00(uint8_t* rdram, recomp_context* ctx) {
    ensure_libultra_virt_count(rdram);
    codeSEG_800C6D00(rdram, ctx);
}

extern "C" void wr64_codeSEG_800C5DF0(uint8_t* rdram, recomp_context* ctx) {
    // LLONSIT decomp: 0x800C5DF0 is osViSetMode (Main_IdleThread), not __osViInit.
    osViSetMode_recomp(rdram, ctx);
    ultramodern::boot_log("[wr64] osViSetMode (800C5DF0): done\n");
    ensure_vi_event_queue(rdram);
    g_presents_after_vi_init.store(0, std::memory_order_release);
    g_vi_init_complete.store(true, std::memory_order_release);
    // VI pulse is enabled after Main_Thread finishes osCreateMesgQueue + osViSetEvent.
}

extern "C" void wr64_osCreateViManager(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    ensure_vi_event_queue(rdram);
}

extern "C" void wr64_osViSetEvent_recomp(uint8_t* rdram, recomp_context* ctx) {
    osViSetEvent_recomp(rdram, ctx);
    const uint32_t mq = static_cast<uint32_t>(ctx->r4);
    if (mq == kMainThreadMesgQueue) {
        g_main_thread_mq_ready.store(true, std::memory_order_release);
        enable_boot_vi_delivery();
        ultramodern::boot_log(
            "[wr64] osViSetEvent: main queue 0x%08X msg=0x%08X (VI pulse enabled)\n",
            mq,
            static_cast<uint32_t>(ctx->r5));
    }
}

uint32_t os_physical_to_virtual(uint8_t* rdram, recomp_context* ctx, uint32_t paddr) {
    recomp_context local = *ctx;
    local.r4 = paddr;
    codeSEG_800C5720(rdram, &local);
    return static_cast<uint32_t>(local.r2);
}

// Native ViSwap — patch_function_entry redirects codeSEG_800CBF50 here; calling the
// patched symbol from this wrapper would recurse until stack overflow.
void hle_vi_swap(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t kGlobalRoot = kSeg800F - 0x6F50u;
    constexpr uint32_t kGlobalAux = kSeg800F - 0x6F4Cu;
    constexpr uint32_t kViCtrlReg = 0xA4400010u;
    constexpr uint32_t kViBase = 0xA4400000u;

    uint32_t& global_aux = n64_mem32(rdram, kGlobalAux);
    uint32_t& global_root = n64_mem32(rdram, kGlobalRoot);
    const uint32_t aux = global_aux;
    if (!is_kseg0_rdram_pointer(aux)) {
        return;
    }

    const uint32_t vi_struct = n64_mem32(rdram, aux + 0x8);
    if (!is_kseg0_rdram_pointer(vi_struct)) {
        return;
    }

    const uint32_t field = n64_mem32(rdram, kViCtrlReg) & 1u;
    const uint32_t slot_offset = field * 0x14u;
    const uint16_t aux_flags = *reinterpret_cast<uint16_t*>(n64_vram(rdram, aux));

    if (aux_flags & 0x2) {
        const uint32_t combined = n64_mem32(rdram, aux + 0x20);
        const uint32_t vi_y = n64_mem32(rdram, vi_struct + 0x20);
        n64_mem32(rdram, aux + 0x20) = (combined & ~0xFFFFF000u) | (vi_y & 0xFFFFF000u);
    } else {
        n64_mem32(rdram, aux + 0x20) = n64_mem32(rdram, vi_struct + 0x20);
    }

    n64_mem32(rdram, aux + 0x2C) = n64_mem32(rdram, vi_struct + 0x2C + slot_offset);

    uint32_t vi_origin = n64_mem32(rdram, aux + 0x2C);
    if (aux_flags & 0x40) {
        vi_origin = os_physical_to_virtual(rdram, ctx, n64_mem32(rdram, aux + 0x4));
        if (aux_flags & 0x80) {
            const uint16_t extra = *reinterpret_cast<uint16_t*>(n64_vram(rdram, aux + 0x28));
            n64_mem32(rdram, aux + 0x2C) = (static_cast<uint32_t>(extra) << 16) & 0x3FF0000u;
        }
    }

    uint32_t width_param = n64_mem32(rdram, vi_struct + 0x1C);
    if (aux_flags & 0x20) {
        width_param = 0;
    }
    n64_mem32(rdram, aux + 0x1C) = n64_mem32(rdram, vi_struct + 0x1C);

    n64_mem32(rdram, kViBase + 0x4) = vi_origin;
    n64_mem32(rdram, kViBase + 0x8) = n64_mem32(rdram, vi_struct + 0x8);
    n64_mem32(rdram, kViBase + 0x14) = n64_mem32(rdram, vi_struct + 0xC);
    n64_mem32(rdram, kViBase + 0x18) = n64_mem32(rdram, vi_struct + 0x10);
    n64_mem32(rdram, kViBase + 0x1C) = n64_mem32(rdram, vi_struct + 0x14);
    n64_mem32(rdram, kViBase + 0x20) = n64_mem32(rdram, vi_struct + 0x18);
    n64_mem32(rdram, kViBase + 0x24) = width_param;
    n64_mem32(rdram, kViBase + 0x28) = n64_mem32(rdram, vi_struct + slot_offset + 0x30);
    n64_mem32(rdram, kViBase + 0x2C) = n64_mem32(rdram, vi_struct + slot_offset + 0x34);
    n64_mem32(rdram, kViBase + 0xC) = n64_mem32(rdram, vi_struct + slot_offset + 0x38);

    const uint32_t prev_root = global_root;
    global_aux = prev_root;
    global_root = aux;

    uint32_t copy_src = global_root;
    uint32_t copy_dst = global_aux;
    const uint32_t copy_end = copy_src + 0x30u;
    while (copy_src != copy_end) {
        n64_mem32(rdram, copy_dst) = n64_mem32(rdram, copy_src);
        n64_mem32(rdram, copy_dst + 0x4) = n64_mem32(rdram, copy_src + 0x4);
        n64_mem32(rdram, copy_dst + 0x8) = n64_mem32(rdram, copy_src + 0x8);
        copy_src += 0xCu;
        copy_dst += 0xCu;
    }
    wr64_sync_host_vi_regs_from_rdram(rdram);
}

// Recompiled COP1 path hits NAN_CHECK in the Taylor inner loop; native math matches decomp.
extern "C" void wr64_guMtxIdentF(float mf[4][4]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            mf[r][c] = (r == c) ? 1.0f : 0.0f;
        }
    }
}

extern "C" void wr64_guOrthoF_impl(float mf[4][4], float l, float r, float b, float t, float n, float f, float scale) {
    wr64_guMtxIdentF(mf);
    mf[0][0] = 2.0f / (r - l);
    mf[1][1] = 2.0f / (t - b);
    mf[2][2] = -2.0f / (f - n);
    mf[3][0] = -(r + l) / (r - l);
    mf[3][1] = -(t + b) / (t - b);
    mf[3][2] = -(f + n) / (f - n);
    mf[3][3] = 1.0f;
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            mf[i][j] *= scale;
        }
    }
}

float wr64_rdram_stack_float(uint8_t* rdram, uint32_t sp, uint32_t offset) {
    uint32_t bits = n64_mem32(rdram, sp + offset);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

float wr64_reg_float(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

void wr64_guMtxF2L(uint8_t* rdram, uint32_t m_vaddr, const float mf[4][4]) {
    if (!is_kseg0_rdram_pointer(m_vaddr)) {
        return;
    }
    int32_t* m1 = reinterpret_cast<int32_t*>(n64_vram(rdram, m_vaddr));
    int32_t* m2 = m1 + 8;
    for (int row = 0; row < 4; ++row) {
        for (int col = 0; col < 2; ++col) {
            const int32_t tmp1 = static_cast<int32_t>(mf[row][col * 2] * 65536.0f);
            const int32_t tmp2 = static_cast<int32_t>(mf[row][col * 2 + 1] * 65536.0f);
            *m1++ = (tmp1 & 0xffff0000) | ((tmp2 >> 16) & 0xffff);
            *m2++ = ((tmp1 << 16) & 0xffff0000) | (tmp2 & 0xffff);
        }
    }
}

void wr64_read_guOrtho_args(
    uint8_t* rdram,
    recomp_context* ctx,
    float& l,
    float& r,
    float& b,
    float& t,
    float& n,
    float& f,
    float& scale) {
    const uint32_t sp = static_cast<uint32_t>(ctx->r29);
    l = wr64_reg_float(static_cast<uint32_t>(ctx->r5));
    r = wr64_reg_float(static_cast<uint32_t>(ctx->r6));
    b = wr64_reg_float(static_cast<uint32_t>(ctx->r7));
    t = wr64_rdram_stack_float(rdram, sp, 0x10u);
    n = wr64_rdram_stack_float(rdram, sp, 0x14u);
    f = wr64_rdram_stack_float(rdram, sp, 0x18u);
    scale = wr64_rdram_stack_float(rdram, sp, 0x1Cu);
}

extern "C" void wr64_guOrthoF(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t mf_vaddr = static_cast<uint32_t>(ctx->r4);
    float l = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    float t = 0.0f;
    float n = 0.0f;
    float f = 0.0f;
    float scale = 0.0f;
    wr64_read_guOrtho_args(rdram, ctx, l, r, b, t, n, f, scale);

    float mf[4][4];
    wr64_guOrthoF_impl(mf, l, r, b, t, n, f, scale);
    if (is_kseg0_rdram_pointer(mf_vaddr)) {
        std::memcpy(n64_vram(rdram, mf_vaddr), mf, sizeof(mf));
    }
}

extern "C" void wr64_guOrtho(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t m_vaddr = static_cast<uint32_t>(ctx->r4);
    float l = 0.0f;
    float r = 0.0f;
    float b = 0.0f;
    float t = 0.0f;
    float n = 0.0f;
    float f = 0.0f;
    float scale = 0.0f;
    wr64_read_guOrtho_args(rdram, ctx, l, r, b, t, n, f, scale);

    float mf[4][4];
    wr64_guOrthoF_impl(mf, l, r, b, t, n, f, scale);
    wr64_guMtxF2L(rdram, m_vaddr, mf);
}

extern "C" void wr64_codeSEG_80047C38(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    constexpr uint32_t kSinTable = 0x80154350u;
    constexpr int kTableCount = 0x1000;
    constexpr double kTwoPi = 6.28318530700000011;

    float* sin_table = reinterpret_cast<float*>(n64_vram(rdram, kSinTable));
    for (int i = 0; i < kTableCount; i++) {
        double x = (static_cast<double>(i) * kTwoPi) / static_cast<double>(kTableCount);
        double numerator = -x * x * x;
        const double minus_square_x = -x * x;
        double denominator = 3.0 * 2.0 * 1.0;

        for (int j = 2; j < 14; j++) {
            x += numerator / denominator;
            numerator *= minus_square_x;
            denominator *= static_cast<double>(j) * static_cast<double>((j << 2) + 2);
        }

        sin_table[i] = static_cast<float>(x);
    }

    static std::atomic<bool> logged{false};
    wr64_seed_framebuffers_if_needed(rdram);
    if (!logged.exchange(true)) {
        ultramodern::boot_log("[wr64] SysUtils_TaylorSeries HLE: filled gSinTable[0x1000]\n");
    }
}

extern "C" void wr64_codeSEG_800CBF50(uint8_t* rdram, recomp_context* ctx) {
    static std::atomic<uint32_t> swap_count{0};
    const uint32_t n = swap_count.fetch_add(1) + 1;
    if (n <= 32) {
        ultramodern::boot_log( "[wr64] VI swap (800CBF50) #%u\n", n);
    }
    {
        std::lock_guard lock(g_timer_path_mutex);
        ensure_libultra_virt_count(rdram);
        restore_timer_handler_fields(rdram);
    }
    hle_vi_swap(rdram, ctx);
}

extern "C" void wr64_on_sp_task_started(uint8_t* rdram, gpr task_ptr) {
    const uint32_t task_vaddr = static_cast<uint32_t>(task_ptr);
    if (!is_kseg0_rdram_pointer(task_vaddr)) {
        return;
    }

    OSTask* task = reinterpret_cast<OSTask*>(n64_vram(rdram, task_vaddr));
    static std::atomic<uint32_t> count{0};
    const uint32_t n = count.fetch_add(1) + 1;

    if (task->t.type == M_GFXTASK) {
        if (!g_allow_vi_pulse.exchange(true, std::memory_order_acq_rel)) {
            ultramodern::set_vi_retrace_mesg_enabled(true);
            ultramodern::boot_log("[wr64] vi_pulse + stock VI retrace: enabled on first Gfx task\n");
        }
        wr64_ensure_host_vi_visible_impl(rdram);
        if (n <= 128) {
            ultramodern::boot_log(
                "[RSP] Gfx Task #%u: flags=0x%X task=0x%08X data_ptr=0x%08X data_size=0x%X "
                "ucode=0x%08X ucode_data=0x%08X\n",
                n,
                task->t.flags,
                task_vaddr,
                static_cast<uint32_t>(task->t.data_ptr),
                task->t.data_size,
                static_cast<uint32_t>(task->t.ucode),
                static_cast<uint32_t>(task->t.ucode_data));
        }
        if (n == 1) {
            g_wr64_rsp_gfx_task_ready.store(true, std::memory_order_release);
            wr64_try_audio_load_init(rdram);
            wr64_invoke_post_gfx_init(rdram);
        }
        wr64_apply_boot_display_mode_if_needed(rdram);
        if (n <= 8) {
            wr64_log_boot_progress(rdram, "gfx-task");
        }

        // Snapshot the display list and hand it to RT64 on the gfx thread. SP/DP
        // completion interrupts are deferred until the next scheduler drain.
        ultramodern::submit_rsp_task(rdram, task_ptr);
        wr64_defer_sp_not_busy();

        static std::atomic<bool> notified_gfx{false};
        if (!notified_gfx.exchange(true)) {
            recompui::notify_game_gfx_submitted();
        }
        return;
    }

    if (n <= 128) {
        ultramodern::boot_log(
            "[RSP] Intercepted Task #%u: type=%u flags=0x%X task=0x%08X\n",
            n,
            task->t.type,
            task->t.flags,
            task_vaddr);
    }

    ultramodern::submit_rsp_task(rdram, task_ptr);
    wr64_defer_sp_not_busy();
}

void on_game_init(uint8_t* rdram, recomp_context* ctx) {
    ::wr64_last_rdram = rdram;
    if (stock_boot_enabled()) {
        // Match dino: no HLE patch table, but WR64 still needs IPL3 boot stack before entrypoint.
        ctx->r29 = WR64_BOOT_STACK_TOP;
        ultramodern::boot_log("[boot] stock_boot: minimal init (boot stack only, no HLE patches)\n");
        return;
    }
    ultramodern::set_vi_retrace_mesg_enabled(false);
    // IPL3 / linker setup: bootThreadStack + BOOT_STACKSIZE (leak-ref kn_memory.h).
    ctx->r29 = WR64_BOOT_STACK_TOP;
    register_wr64_function_patches();
    init_boot_thread_gates(rdram);
    init_libultra_timer_globals(rdram, true);
}

// Runs immediately before recomp_entrypoint (after heap init). osInitialize reads
// libultra BSS that may still contain 0x80000000 sentinels from the ROM DMA.
void prepare_boot_entrypoint(uint8_t* rdram) {
    g_vi_init_complete.store(false, std::memory_order_release);
    g_allow_vi_pulse.store(false, std::memory_order_release);
    g_main_thread_mq_ready.store(false, std::memory_order_release);
    g_entrypoint_complete.store(false, std::memory_order_release);
    g_presents_after_vi_init.store(0, std::memory_order_release);
    ultramodern::set_vi_retrace_mesg_enabled(false);
    init_libultra_timer_globals(rdram, false);
    wr64_sync_game_state_dispatch(rdram);
    wr64_seed_game_state_jump_table(rdram);
    wr64_seed_gs_aux_jump_table(rdram);
    wr64_seed_asset_load_jump_tables(rdram);
    wr64_seed_goverlay_table(rdram);
    ultramodern::boot_log("[wr64] boot entry: overlay table seeded @0x%08X\n", kOverlayTableVram);
    // symbol_addrs.txt (LLONSIT/Wave-Race-64 decomp)
    constexpr uint32_t kGameState = 0x800DAB24u;
    constexpr uint32_t kOverlayMode = 0x801CE638u;
    constexpr uint32_t kGameModes = 0x801CE620u;
    ultramodern::boot_log(
        "[wr64] boot entry: virt active=0x%08X freelist=0x%08X "
        "gGameState=0x%08X D_801CE638=0x%08X gGameModes=0x%08X\n",
        n64_mem32(rdram, kSeg800F - 0x6FF0u),
        n64_mem32(rdram, kSeg800F - 0x6FF8u),
        n64_mem32(rdram, kGameState),
        n64_mem32(rdram, kOverlayMode),
        n64_mem32(rdram, kGameModes));
}

void on_present(uint8_t* rdram) {
    if (stock_boot_enabled()) {
        return;
    }
    pulse_vi_and_pi(rdram);
}

void on_vi_tick(uint8_t* rdram) {
    if (stock_boot_enabled()) {
        return;
    }
    // VI timer thread must not wait on Present before pulsing — that creates a circular
    // wait with OSThreads blocked in osRecvMesg -> run_next_thread_and_wait -> wait_for_host_frame_presented.
    pulse_vi_and_pi(rdram);
}

void on_thread_create(uint8_t* rdram, recomp_context* ctx) {
    (void)ctx;
    init_libultra_timer_globals(rdram, false);
}

std::vector<uint8_t> extract_rom_slice(std::span<const uint8_t> rom, uint32_t rom_offset, uint32_t size) {
    if (rom_offset + size > rom.size()) {
        return {};
    }
    return std::vector<uint8_t>(rom.begin() + rom_offset, rom.begin() + rom_offset + size);
}

void notify_entrypoint_returned() {
    g_entrypoint_complete.store(true, std::memory_order_release);
    ultramodern::boot_log("[wr64] entrypoint returned — boot continues on OSThreads (stock VI until first Gfx)\n");
}

} // namespace wr64

extern "C" void wr64_notify_nonempty_swapchain_present() {
    wr64::notify_nonempty_swapchain_present();
}

extern "C" bool wr64_consume_skip_next_update_screen_present(void) {
    return wr64::consume_skip_next_update_screen_present();
}

extern "C" void recomp_syscall_handler(uint8_t* rdram, recomp_context* ctx, int32_t instruction_vram) {
    (void)rdram;
    (void)ctx;
    ultramodern::boot_log( "[wr64] syscall at 0x%08X (stub)\n", static_cast<uint32_t>(instruction_vram));
}
