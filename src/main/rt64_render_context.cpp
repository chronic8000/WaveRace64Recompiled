#include <atomic>
#include <cstdio>
#include <memory>
#include <cstring>
#include <variant>
#include <algorithm>
#include <mutex>

#include "xxHash/xxh3.h"

#define HLSL_CPU
#include "hle/rt64_application.h"
#include "hle/rt64_framebuffer.h"
#include "hle/rt64_framebuffer_pair.h"
#include "hle/rt64_present_queue.h"
#include "hle/rt64_workload.h"
#include "hle/rt64_workload_queue.h"
#include "hle/rt64_rdp.h"
#include "render/rt64_render_target_manager.h"
#include "render/rt64_render_worker.h"
#include "rhi/rt64_render_hooks.h"
#include "gbi/rt64_gbi_f3dwave.h"
#include "gbi/rt64_gbi_rdp.h"
#include "overloaded.h"


#include "ultramodern/ultramodern.hpp"
#include "ultramodern/config.hpp"
#include "ultramodern/ultra64.h"

#include "zelda_render.h"
#include "zelda_support.h"
#include "wr64_boot_config.hpp"
#include "wr64_memory.h"
#include "recomp_ui.h"
#include "concurrentqueue.h"

#include <array>

static RT64::UserConfiguration::Antialiasing device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
static bool sample_positions_supported = false;
static bool high_precision_fb_enabled = false;

static uint8_t DMEM[0x1000];
static uint8_t IMEM[0x1000];

namespace wr64 {
void on_present(uint8_t* rdram);
}

extern uint8_t* wr64_last_rdram;

namespace {

uint32_t wr64_rdram_u32(uint8_t* rdram, uint32_t vaddr) {
    if (rdram == nullptr || vaddr < 0x80000000u) {
        return 0u;
    }
    return *reinterpret_cast<uint32_t*>(wr64_rdram_byte_ptr(rdram, vaddr));
}

void wr64_log_framebuffer_tracking(
    uint8_t* rdram,
    uint32_t send_dl_n,
    const char* phase,
    const RT64::RDP* rdp) {
    if (send_dl_n > 64u || rdram == nullptr) {
        return;
    }

    constexpr uint32_t kGFrameBuffers = 0x801542C0u;
    constexpr uint32_t kGFramebuffersIdx = 0x80151948u;
    constexpr uint32_t kDisplayMode = 0x800DAB1Cu;
    constexpr uint32_t kDisplayModeFbIdx = 0x800D45D8u;
    constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;

    const uint32_t mode = wr64_rdram_u32(rdram, kDisplayMode);
    const uint32_t mode_fb_idx = wr64_rdram_u32(rdram, kDisplayModeFbIdx) & 1u;
    const uint32_t mode_fb0 = wr64_rdram_u32(rdram, kDisplayModeFbArray + 0u);
    const uint32_t mode_fb1 = wr64_rdram_u32(rdram, kDisplayModeFbArray + 4u);
    const uint32_t draw_fb = wr64_rdram_u32(rdram, kDisplayModeFbArray + mode_fb_idx * 4u);
    const uint32_t gfb_idx = wr64_rdram_u32(rdram, kGFramebuffersIdx) & 3u;
    const uint32_t gfb0 = wr64_rdram_u32(rdram, kGFrameBuffers + 0u);
    const uint32_t gfb1 = wr64_rdram_u32(rdram, kGFrameBuffers + 4u);
    const uint32_t gfb2 = wr64_rdram_u32(rdram, kGFrameBuffers + 8u);
    const uint32_t gfb = wr64_rdram_u32(rdram, kGFrameBuffers + gfb_idx * 4u);
    const uint32_t cur_fb = static_cast<uint32_t>(osViGetCurrentFramebuffer());
    const uint32_t next_fb = static_cast<uint32_t>(osViGetNextFramebuffer());

    const uint32_t rdp_color = (rdp != nullptr) ? rdp->colorImage.address : 0u;
    const uint32_t rdp_color_kseg0 = 0x80000000u | (rdp_color & 0x00FFFFFFu);
    const uint32_t draw_phys = draw_fb & 0x00FFFFFFu;
    const uint32_t other_phys = (mode_fb_idx == 0u ? mode_fb1 : mode_fb0) & 0x00FFFFFFu;

    fprintf(stderr,
        "[RT64] fb_track #%u %-8s mode=%u rdp.color=0x%08X (kseg0=0x%08X) "
        "D_800D45DC={0x%08X,0x%08X} idx=%u draw=0x%08X "
        "gFB={0x%08X,0x%08X,0x%08X}[%u]=0x%08X "
        "osVi{cur=0x%08X,next=0x%08X} "
        "rdp==draw=%d rdp==other=%d vi==draw=%d vi==gfb=%d\n",
        send_dl_n,
        phase,
        mode,
        rdp_color,
        rdp_color_kseg0,
        mode_fb0,
        mode_fb1,
        mode_fb_idx,
        draw_fb,
        gfb0,
        gfb1,
        gfb2,
        gfb_idx,
        gfb,
        cur_fb,
        next_fb,
        (rdp_color == draw_phys) ? 1 : 0,
        (rdp_color == other_phys) ? 1 : 0,
        ((cur_fb & 0x00FFFFFFu) == draw_phys) ? 1 : 0,
        ((cur_fb & 0x00FFFFFFu) == (gfb & 0x00FFFFFFu)) ? 1 : 0);
    fflush(stderr);
}

void wr64_log_present_targets(
    uint8_t* rdram,
    uint32_t send_dl_n,
    const RT64::VI& vi,
    const RT64::Workload& workload) {
    if (send_dl_n > 64) {
        return;
    }

    constexpr uint32_t kGFrameBuffers = 0x801542C0u;
    constexpr uint32_t kGFramebuffersIdx = 0x80151948u;
    constexpr uint32_t kDisplayMode = 0x800DAB1Cu;
    constexpr uint32_t kDisplayModeFbIdx = 0x800D45D8u;
    constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;

    const uint32_t vi_origin = vi.origin;
    const uint32_t vi_fb_base = vi.fbAddress();
    const hlslpp::uint2 vi_fb_size = vi.fbSize();
    const uint32_t vi_fb_w = vi_fb_size[0];
    const uint32_t vi_fb_h = vi_fb_size[1];
    const uint32_t vi_origin_kseg0 = 0x80000000u | vi_origin;
    const uint32_t vi_fb_base_kseg0 = 0x80000000u | vi_fb_base;

    const uint32_t cur_fb = static_cast<uint32_t>(osViGetCurrentFramebuffer());
    const uint32_t next_fb = static_cast<uint32_t>(osViGetNextFramebuffer());

    const uint32_t fb_idx = wr64_rdram_u32(rdram, kGFramebuffersIdx);
    const uint32_t gfb0 = wr64_rdram_u32(rdram, kGFrameBuffers);
    const uint32_t gfb1 = wr64_rdram_u32(rdram, kGFrameBuffers + 4u);
    const uint32_t gfb2 = wr64_rdram_u32(rdram, kGFrameBuffers + 8u);
    const uint32_t mode = wr64_rdram_u32(rdram, kDisplayMode);
    const uint32_t mode_fb_idx = wr64_rdram_u32(rdram, kDisplayModeFbIdx);
    const uint32_t mode_fb0 = wr64_rdram_u32(rdram, kDisplayModeFbArray + 0u);
    const uint32_t mode_fb1 = wr64_rdram_u32(rdram, kDisplayModeFbArray + 4u);
    const uint32_t mode_fb = wr64_rdram_u32(rdram, kDisplayModeFbArray + (mode_fb_idx & 1u) * 4u);
    const uint32_t mode_fb0_phys = mode_fb0 & 0x00FFFFFFu;
    const uint32_t mode_fb1_phys = mode_fb1 & 0x00FFFFFFu;

    fprintf(stderr,
        "[RT64] present #%u VI origin=0x%08X fbBase=0x%08X vi.width=%u fbSize=%ux%u serrate=%u "
        "(kseg0 origin=0x%08X base=0x%08X) osVi cur=0x%08X next=0x%08X gFB[%u]=0x%08X mode=%u modeFB=0x%08X\n",
        send_dl_n,
        vi_origin,
        vi_fb_base,
        vi.width,
        vi_fb_w,
        vi_fb_h,
        vi.status.serrate ? 1u : 0u,
        vi_origin_kseg0,
        vi_fb_base_kseg0,
        cur_fb,
        next_fb,
        fb_idx & 3u,
        wr64_rdram_u32(rdram, kGFrameBuffers + (fb_idx & 3u) * 4u),
        mode,
        mode_fb);
    if (mode == 3u) {
        fprintf(stderr,
            "[RT64] present #%u mode3 D_800D45DC={0x%08X,0x%08X} idx=%u drawFB=0x%08X "
            "vi_matches_fb0=%d vi_matches_fb1=%d\n",
            send_dl_n,
            mode_fb0,
            mode_fb1,
            mode_fb_idx & 1u,
            mode_fb,
            (vi_fb_base == mode_fb0_phys) ? 1 : 0,
            (vi_fb_base == mode_fb1_phys) ? 1 : 0);
    }
  if (vi_origin >= vi_fb_base && vi_fb_base != 0u) {
        fprintf(stderr,
            "[RT64] present #%u VI field offset=0x%08X (origin - fbBase)\n",
            send_dl_n,
            vi_origin - vi_fb_base);
    }

    for (uint32_t f = 0; f < workload.fbPairCount && f < 4u; ++f) {
        const auto& pair = workload.fbPairs[f];
        const uint32_t rdp_color = pair.colorImage.address;
        fprintf(stderr,
            "[RT64] present #%u fbPair[%u] rdpColor=0x%08X rdpColor_kseg0=0x%08X "
            "width=%u siz=%u match_vi_base=%d match_gfb=%d\n",
            send_dl_n,
            f,
            rdp_color,
            0x80000000u | (rdp_color & 0x00FFFFFFu),
            pair.colorImage.width,
            pair.colorImage.siz,
            (rdp_color == vi_fb_base) ? 1 : 0,
            (rdp_color == (gfb0 & 0x00FFFFFFu) || rdp_color == (gfb1 & 0x00FFFFFFu) ||
             rdp_color == (gfb2 & 0x00FFFFFFu))
                ? 1
                : 0);
    }
    fflush(stderr);
}

static void wr64_log_storage_bands(const std::vector<uint8_t>& storage, uint32_t width, uint32_t height) {
    if (width == 0u || height == 0u || storage.size() < size_t(width) * height * 2u) {
        return;
    }

    static std::atomic<uint32_t> band_log_count{0};
    const uint32_t n = band_log_count.fetch_add(1) + 1;
    if (n > 16u) {
        return;
    }

    const auto* px = reinterpret_cast<const uint16_t*>(storage.data());
    auto count_nonzero = [&](uint32_t y0, uint32_t y1, uint32_t x0, uint32_t x1) {
        uint32_t nonzero = 0u;
        const uint32_t y_end = std::min(y1, height);
        const uint32_t x_end = std::min(x1, width);
        for (uint32_t y = y0; y < y_end; ++y) {
            for (uint32_t x = x0; x < x_end; ++x) {
                if (px[y * width + x] != 0u) {
                    ++nonzero;
                }
            }
        }
        return nonzero;
    };

    const uint32_t top_y1 = height * 20u / 100u;
    const uint32_t mid_y0 = top_y1;
    const uint32_t mid_y1 = height * 40u / 100u;
    const uint32_t bot_y0 = mid_y1;
    const uint32_t left_x1 = width / 4u;
    const uint32_t right_x0 = left_x1;

    fprintf(stderr,
        "[RT64] storage_bands #%u %ux%u top20%%=%u mid20%%=%u bot60%%=%u "
        "bot_left25%%=%u bot_right75%%=%u\n",
        n,
        width,
        height,
        count_nonzero(0u, top_y1, 0u, width),
        count_nonzero(mid_y0, mid_y1, 0u, width),
        count_nonzero(bot_y0, height, 0u, width),
        count_nonzero(bot_y0, height, 0u, left_x1),
        count_nonzero(bot_y0, height, right_x0, width));
    fflush(stderr);
}

static void wr64_log_present_storage_sample(
    uint32_t send_dl_n,
    uint32_t fb_phys,
    const std::vector<uint8_t>& storage,
    const uint8_t* rdram) {
    static std::atomic<uint32_t> log_count{0};
    const uint32_t n = log_count.fetch_add(1) + 1;
    if (n > 32u) {
        return;
    }

    auto emit_pixels = [&](const char* source, const uint16_t* pixels) {
        fprintf(stderr,
            "[RT64] present_storage #%u send_dl=%u fb=0x%08X src=%s px16:",
            n,
            send_dl_n,
            fb_phys,
            source);
        for (int i = 0; i < 16; ++i) {
            fprintf(stderr, " %04X", pixels[i]);
        }
        fprintf(stderr, "\n");
        fflush(stderr);
    };

    if (!storage.empty() && storage.size() >= 32u) {
        const auto* storage_px = reinterpret_cast<const uint16_t*>(storage.data());
        emit_pixels("storage", storage_px);
        wr64_log_storage_bands(storage, 640u, 240u);
    }

    if (rdram != nullptr && fb_phys != 0u) {
        const auto* rdram_px = reinterpret_cast<const uint16_t*>(rdram + fb_phys);
        emit_pixels("rdram", rdram_px);
    }
}

uint32_t wr64_phys_from_segmented(const std::array<uint32_t, 16>& segments, uint32_t seg_addr) {
    const uint32_t seg = (seg_addr >> 24) & 0x0Fu;
    return (segments[seg] + (seg_addr & 0x00FFFFFFu)) & 0x00FFFFFFu;
}

bool wr64_is_draw_opcode(uint8_t op) {
    return op == 0x04u || op == 0x05u || op == 0xB1u || (op >= 0x30u && op <= 0x3Fu);
}

struct Wr64DlOpcodeCounts {
    uint32_t texrect = 0;
    uint32_t texrect_flip = 0;
    uint32_t set_tile = 0;
    uint32_t load_tile = 0;
    uint32_t load_block = 0;
    uint32_t set_tilesize = 0;
    uint32_t load_tlut = 0;
    uint32_t fillrect = 0;
    uint32_t setcimg = 0;
    uint32_t setzimg = 0;
    uint32_t tri = 0;
    uint32_t branch = 0;
    uint32_t fullsync = 0;
    uint32_t other = 0;
};

void wr64_count_dl_opcode(uint8_t op, Wr64DlOpcodeCounts& counts) {
    switch (op) {
    case 0xE4u:
        ++counts.texrect;
        break;
    case 0xE5u:
        ++counts.texrect_flip;
        break;
    case 0xF5u:
        ++counts.set_tile;
        break;
    case 0xF4u:
        ++counts.load_tile;
        break;
    case 0xF3u:
        ++counts.load_block;
        break;
    case 0xF2u:
        ++counts.set_tilesize;
        break;
    case 0xF0u:
        ++counts.load_tlut;
        break;
    case 0xF6u:
        ++counts.fillrect;
        break;
    case 0xFFu:
        ++counts.setcimg;
        break;
    case 0xFEu:
        ++counts.setzimg;
        break;
    case 0x04u:
    case 0x05u:
    case 0xB1u:
        ++counts.tri;
        break;
    case 0x06u:
        ++counts.branch;
        break;
    case 0xE9u:
        ++counts.fullsync;
        break;
    default:
        if (wr64_is_draw_opcode(op)) {
            ++counts.tri;
        } else {
            ++counts.other;
        }
        break;
    }
}

void wr64_scan_dl_range(
    uint8_t* rdram,
    uint32_t send_dl_n,
    uint32_t ptr,
    uint32_t max_cmds,
    uint32_t depth,
    const std::array<uint32_t, 16>& segments,
    uint32_t& tri_out,
    uint32_t& branch_out,
    Wr64DlOpcodeCounts& opcode_counts) {
    if (depth > 4u || max_cmds == 0u || ptr >= 0x00800000u) {
        return;
    }
    if (depth <= 2u) {
        const uint32_t w0 = *reinterpret_cast<uint32_t*>(rdram + ptr);
        const uint32_t w1 = *reinterpret_cast<uint32_t*>(rdram + ptr + 4u);
        fprintf(stderr,
            "[RT64] DL scan #%u branch depth=%u resolved=0x%08X head=0x%08X%08X\n",
            send_dl_n,
            depth,
            ptr,
            w0,
            w1);
    }
    for (uint32_t i = 0; i < max_cmds; ++i) {
        const uint32_t off = ptr + i * 8u;
        if (off + 8u > 0x800000u) {
            break;
        }
        const uint32_t w0 = *reinterpret_cast<uint32_t*>(rdram + off);
        const uint32_t w1 = *reinterpret_cast<uint32_t*>(rdram + off + 4u);
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        wr64_count_dl_opcode(op, opcode_counts);
        if (wr64_is_draw_opcode(op)) {
            ++tri_out;
        }
        if (op == 0xBCu && (w0 & 0xFFu) == 0x06u) {
            const uint32_t seg = ((w0 >> 8) & 0xFFu) >> 2;
            if (seg < 16u) {
                fprintf(stderr,
                    "[RT64] DL scan #%u seg%u=0x%08X (depth=%u)\n",
                    send_dl_n,
                    seg,
                    w1,
                    depth);
            }
        }
        if (op == 0x06u) {
            ++branch_out;
            const uint32_t resolved = wr64_phys_from_segmented(segments, w1);
            fprintf(stderr,
                "[RT64] DL scan #%u branch depth=%u target=0x%08X resolved=0x%08X "
                "head=0x%08X%08X\n",
                send_dl_n,
                depth,
                w1,
                resolved,
                w0,
                w1);
            if (depth < 4u) {
                std::array<uint32_t, 16> child_segments = segments;
                wr64_scan_dl_range(
                    rdram,
                    send_dl_n,
                    resolved,
                    256u,
                    depth + 1u,
                    child_segments,
                    tri_out,
                    branch_out,
                    opcode_counts);
            }
        }
        if (op == 0xB8u) {
            break;
        }
    }
}

void wr64_log_dl_scan(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size, uint32_t send_dl_n) {
    if (send_dl_n > 24u || rdram == nullptr || data_size < 8u) {
        return;
    }

    Wr64DlOpcodeCounts root_counts;
    Wr64DlOpcodeCounts total_counts;
    uint32_t branch_tri = 0;
    std::array<uint32_t, 16> segments{};

    for (uint32_t off = 0; off + 8u <= data_size; off += 8u) {
        const uint32_t w0 = *reinterpret_cast<uint32_t*>(rdram + data_ptr + off);
        const uint32_t w1 = *reinterpret_cast<uint32_t*>(rdram + data_ptr + off + 4u);
        const uint8_t op = static_cast<uint8_t>(w0 >> 24);
        wr64_count_dl_opcode(op, root_counts);
        if (op == 0xBCu && (w0 & 0xFFu) == 0x06u) {
            const uint32_t seg = ((w0 >> 8) & 0xFFu) >> 2;
            if (seg < 16u) {
                segments[seg] = w1;
            }
        }
        switch (op) {
        case 0xFFu: {
            const uint8_t fmt = static_cast<uint8_t>((w0 >> 21) & 7u);
            const uint8_t siz = static_cast<uint8_t>((w0 >> 19) & 3u);
            const uint16_t width = static_cast<uint16_t>((w0 & 0xFFFu) + 1u);
            fprintf(stderr,
                "[RT64] DL scan #%u SetColorImage off=0x%X addr=0x%08X fmt=%u siz=%u width=%u\n",
                send_dl_n,
                off,
                w1,
                fmt,
                siz,
                width);
            break;
        }
        case 0xFEu:
            fprintf(stderr,
                "[RT64] DL scan #%u SetDepthImage off=0x%X addr=0x%08X\n",
                send_dl_n,
                off,
                w1);
            break;
        case 0x06u:
            fprintf(stderr,
                "[RT64] DL scan #%u BranchDL off=0x%X target=0x%08X\n",
                send_dl_n,
                off,
                w1);
            break;
        default:
            break;
        }
    }

    total_counts = root_counts;
    fprintf(stderr, "[RT64] DL scan #%u segments seg8=0x%08X seg1=0x%08X\n",
        send_dl_n, segments[8], segments[1]);
    for (uint32_t off = 0; off + 8u <= data_size; off += 8u) {
        const uint32_t w0 = *reinterpret_cast<uint32_t*>(rdram + data_ptr + off);
        const uint32_t w1 = *reinterpret_cast<uint32_t*>(rdram + data_ptr + off + 4u);
        if (static_cast<uint8_t>(w0 >> 24) == 0x06u) {
            Wr64DlOpcodeCounts branch_counts{};
            uint32_t local_tri = 0;
            uint32_t local_branch = 0;
            wr64_scan_dl_range(
                rdram,
                send_dl_n,
                wr64_phys_from_segmented(segments, w1),
                256u,
                1u,
                segments,
                local_tri,
                local_branch,
                branch_counts);
            branch_tri += local_tri;
            total_counts.texrect += branch_counts.texrect;
            total_counts.texrect_flip += branch_counts.texrect_flip;
            total_counts.set_tile += branch_counts.set_tile;
            total_counts.load_tile += branch_counts.load_tile;
            total_counts.load_block += branch_counts.load_block;
            total_counts.set_tilesize += branch_counts.set_tilesize;
            total_counts.load_tlut += branch_counts.load_tlut;
            total_counts.fillrect += branch_counts.fillrect;
            total_counts.setcimg += branch_counts.setcimg;
            total_counts.setzimg += branch_counts.setzimg;
            total_counts.tri += branch_counts.tri;
            total_counts.branch += branch_counts.branch;
            total_counts.fullsync += branch_counts.fullsync;
            total_counts.other += branch_counts.other;
        }
    }

    fprintf(stderr,
        "[RT64] DL scan #%u summary cmds=%u setcimg=%u setzimg=%u root_tri=%u "
        "branch_tri=%u fill=%u branch=%u fullsync=%u\n",
        send_dl_n,
        data_size / 8u,
        root_counts.setcimg,
        root_counts.setzimg,
        root_counts.tri,
        branch_tri,
        root_counts.fillrect,
        root_counts.branch,
        root_counts.fullsync);
    fprintf(stderr,
        "[RT64] DL scan #%u totals texrect=%u texrect_flip=%u set_tile=%u load_tile=%u "
        "load_block=%u set_tilesize=%u load_tlut=%u fillrect=%u setcimg=%u tri=%u other=%u\n",
        send_dl_n,
        total_counts.texrect,
        total_counts.texrect_flip,
        total_counts.set_tile,
        total_counts.load_tile,
        total_counts.load_block,
        total_counts.set_tilesize,
        total_counts.load_tlut,
        total_counts.fillrect,
        total_counts.setcimg,
        total_counts.tri,
        total_counts.other);
    fflush(stderr);
}

RT64::VI wr64_fixup_mode3_present_vi(
    uint8_t* rdram,
    RT64::VI vi,
    uint32_t rdp_color_phys,
    uint32_t rdp_color_width) {
    if (rdram == nullptr || rdp_color_phys == 0u || rdp_color_width == 0u) {
        return vi;
    }

    constexpr uint32_t kDisplayMode = 0x800DAB1Cu;
    constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;
    if (wr64_rdram_u32(rdram, kDisplayMode) != 3u) {
        return vi;
    }

    uint32_t present_color = rdp_color_phys & 0x00FFFFFFu;
    const uint32_t fb0_phys = wr64_rdram_u32(rdram, kDisplayModeFbArray + 0u) & 0x00FFFFFFu;
    const uint32_t fb1_phys = wr64_rdram_u32(rdram, kDisplayModeFbArray + 4u) & 0x00FFFFFFu;
    if (present_color != fb0_phys && present_color != fb1_phys) {
        return vi;
    }
    present_color = wr64::mode3_present_color_phys(rdram, present_color);

    // RT64 GPU framebuffers use the RDP colorImage width (640 in WR64 mode 3).
    const uint32_t present_width = rdp_color_width != 0u ? rdp_color_width : 320u;
    vi.width = present_width;
    vi.origin = wr64::mode3_host_vi_origin_for_color_fb(rdram, present_color, present_width);
    // decodeVI() can yield garbage regions/scales; force sane defaults so fbSize() is 640x240.
    vi.status.type = VI_STATUS_TYPE_16_BIT;
    vi.status.serrate = 0;
    vi.hRegion.word = 0x006C02ECu;
    vi.vRegion.word = 0x2501FFu;
    vi.xTransform.xScale = 0x200u;
    vi.xTransform.xOffset = 0x06Cu;
    vi.yTransform.yScale = 0x400u;
    vi.yTransform.yOffset = 0u;
    return vi;
}

static bool wr64_rdram_fb_has_pixels(const uint8_t* rdram, uint32_t phys, uint32_t sample_words) {
    if (rdram == nullptr || phys == 0u) {
        return false;
    }
    const auto* pixels = reinterpret_cast<const uint16_t*>(rdram + phys);
    const uint32_t limit = std::min<uint32_t>(sample_words, 64u);
    for (uint32_t i = 0; i < limit; ++i) {
        if (pixels[i] != 0u) {
            return true;
        }
    }
    return false;
}

static RT64::RenderTarget* wr64_lookup_gpu_color_target(
    RT64::State& state,
    uint32_t phys,
    uint8_t siz,
    uint32_t preferred_width,
    bool mode3) {
    phys &= 0x00FFFFFFu;
    if (phys == 0u) {
        return nullptr;
    }

    uint32_t try_widths[3];
    uint32_t try_count = 0u;
    auto push_width = [&](uint32_t w) {
        if (w == 0u) {
            return;
        }
        for (uint32_t i = 0u; i < try_count; ++i) {
            if (try_widths[i] == w) {
                return;
            }
        }
        if (try_count < 3u) {
            try_widths[try_count++] = w;
        }
    };

    push_width(preferred_width);
    push_width(640u);
    if (!mode3) {
        push_width(320u);
    }

    for (uint32_t i = 0u; i < try_count; ++i) {
        const RT64::RenderTargetKey key(
            phys, try_widths[i], siz, RT64::Framebuffer::Type::Color);
        RT64::RenderTarget& target = state.renderTargetManager.get(key, true);
        if (!target.isEmpty()) {
            return &target;
        }
    }

    return nullptr;
}

static bool wr64_gpu_color_target_nonempty(
    RT64::Application* app,
    uint32_t phys,
    uint8_t siz,
    uint32_t preferred_width,
    bool mode3) {
    if (app == nullptr || app->state == nullptr || app->sharedQueueResources == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(app->sharedQueueResources->workloadMutex);
    return wr64_lookup_gpu_color_target(*app->state, phys, siz, preferred_width, mode3) != nullptr;
}

static bool wr64_try_force_gpu_framebuffer_to_rdram(
    RT64::Application* app,
    uint32_t phys,
    uint8_t fmt,
    uint8_t siz,
    uint32_t width,
    uint32_t height,
    uint32_t row_start,
    uint32_t row_end,
    uint32_t log_n,
    bool force_overwrite = false,
    bool mode3 = false) {
    phys &= 0x00FFFFFFu;
    if (app == nullptr || app->state == nullptr || app->core.RDRAM == nullptr || phys == 0u || width == 0u ||
        height == 0u)
    {
        return false;
    }
    if (!force_overwrite && wr64_rdram_fb_has_pixels(app->core.RDRAM, phys, 64u)) {
        return false;
    }

    if (app->shaderLibrary == nullptr || app->sharedQueueResources == nullptr) {
        return false;
    }

    // Readback runs on the send_dl thread — never touch workloadGraphicsWorker (owned by the
    // RT64 render thread). Copy from the shared render target via framebufferGraphicsWorker.
    RT64::RenderWorker* worker = app->framebufferGraphicsWorker.get();
    if (worker == nullptr) {
        return false;
    }

    if (row_end <= row_start) {
        row_start = 0u;
        row_end = height;
    }
    row_end = std::min(row_end, height);

    std::lock_guard<std::mutex> lock(app->sharedQueueResources->workloadMutex);
    RT64::State& state = *app->state;

    RT64::RenderTarget* color_target =
        wr64_lookup_gpu_color_target(state, phys, siz, width, mode3);
    if (color_target == nullptr || color_target->isEmpty()) {
        if (log_n <= 16u) {
            fprintf(stderr,
                "[RT64] wr64_gpu_readback #%u phys=0x%08X preferred_w=%u mode3=%d: no GPU color target\n",
                log_n,
                phys,
                width,
                mode3 ? 1 : 0);
            fflush(stderr);
        }
        return false;
    }

    const uint32_t effective_width = color_target->width;
    if (log_n <= 16u && effective_width != width) {
        fprintf(stderr,
            "[RT64] wr64_gpu_readback #%u phys=0x%08X using gpu_w=%u (preferred=%u)\n",
            log_n,
            phys,
            effective_width,
            width);
        fflush(stderr);
    }

    RT64::Framebuffer& color_fb =
        state.framebufferManager.get(phys, siz, effective_width, height);

    const uint32_t native_writes_before = color_fb.nativeTarget.writeBufferHistoryCount;
    {
        RT64::RenderWorkerExecution execution(worker);
        color_target->resolveTarget(worker, app->shaderLibrary.get());
        color_fb.copyRenderTargetToNative(
            worker,
            color_target,
            effective_width,
            row_start,
            row_end,
            fmt,
            0u,
            app->shaderLibrary.get());
    }

    if (color_fb.nativeTarget.writeBufferHistoryCount <= native_writes_before) {
        if (log_n <= 16u) {
            fprintf(stderr,
                "[RT64] wr64_gpu_readback #%u phys=0x%08X width=%u rows=%u-%u: native writeback empty\n",
                log_n,
                phys,
                effective_width,
                row_start,
                row_end);
            fflush(stderr);
        }
        return false;
    }

    color_fb.copyNativeToRAM(&app->core.RDRAM[phys], effective_width, row_start, row_end);
    if (wr64_rdram_fb_has_pixels(app->core.RDRAM, phys, 64u)) {
        if (log_n <= 16u) {
            fprintf(stderr,
                "[RT64] wr64_gpu_readback #%u phys=0x%08X width=%u rows=%u-%u rdram_nonzero=1\n",
                log_n,
                phys,
                effective_width,
                row_start,
                row_end);
            fflush(stderr);
        }
        return true;
    }

    if (log_n <= 16u) {
        fprintf(stderr,
            "[RT64] wr64_gpu_readback #%u phys=0x%08X width=%u rows=%u-%u rdram_nonzero=0\n",
            log_n,
            phys,
            effective_width,
            row_start,
            row_end);
        fflush(stderr);
    }
    return false;
}

static void wr64_sync_workload_framebuffer_to_rdram(
    RT64::Application* app,
    const RT64::Workload& workload,
    uint32_t send_dl_n) {
    if (app == nullptr || app->core.RDRAM == nullptr || workload.fbPairCount == 0u) {
        return;
    }
    const RT64::FramebufferPair& pair = workload.fbPairs[workload.fbPairCount - 1u];
    const uint32_t phys = getFramebufferPairColorAddress(workload, pair) & 0x00FFFFFFu;
    if (phys == 0u) {
        return;
    }

    constexpr uint32_t kDisplayMode = 0x800DAB1Cu;
    const bool mode3 = wr64_rdram_u32(app->core.RDRAM, kDisplayMode) == 3u;
    const uint32_t mode3_height = 240u;
    const bool had_pixels = wr64_rdram_fb_has_pixels(app->core.RDRAM, phys, 64u);
    if (mode3 || (!had_pixels && !workload.preferRdpColorForCopy)) {
        uint32_t row_start = 0u;
        uint32_t row_end = mode3 ? mode3_height : pair.colorImage.width;
        uint32_t fb_height = mode3 ? mode3_height : pair.drawColorRect.bottom(true);
        if (!mode3 && !pair.drawColorRect.isEmpty()) {
            row_start = pair.drawColorRect.top(false);
            row_end = pair.drawColorRect.bottom(true);
        }
        if (!mode3 && pair.drawColorRect.isEmpty()) {
            fb_height = 240u;
        }
        wr64_try_force_gpu_framebuffer_to_rdram(
            app,
            phys,
            pair.colorImage.fmt,
            pair.colorImage.siz,
            pair.colorImage.width,
            fb_height,
            row_start,
            row_end,
            send_dl_n,
            mode3,
            mode3);
    }

    // Mode 3 double-buffer: ensure both D_800D45DC framebuffers are synced when the GPU
    // target has content but copyNativeToRAM missed one of the swap buffers.
    constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;
    if (mode3) {
        const uint32_t fb0 = wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbArray + 0u) & 0x00FFFFFFu;
        const uint32_t fb1 = wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbArray + 4u) & 0x00FFFFFFu;
        constexpr uint32_t kMode3Width = 640u;
        constexpr uint32_t kMode3Height = 240u;
        for (const uint32_t mode_fb : {fb0, fb1}) {
            if (mode_fb == 0u || mode_fb == phys) {
                continue;
            }
            if (!wr64_rdram_fb_has_pixels(app->core.RDRAM, mode_fb, 64u) &&
                wr64_gpu_color_target_nonempty(app, mode_fb, pair.colorImage.siz, kMode3Width, true)) {
                wr64_try_force_gpu_framebuffer_to_rdram(
                    app,
                    mode_fb,
                    pair.colorImage.fmt,
                    pair.colorImage.siz,
                    kMode3Width,
                    kMode3Height,
                    0u,
                    kMode3Height,
                    send_dl_n,
                    true,
                    true);
            }
        }
    }

    static std::atomic<uint32_t> sync_log{0};
    const uint32_t n = sync_log.fetch_add(1) + 1;
    if (n <= 16u) {
        fprintf(stderr,
            "[RT64] wr64_sync_fb #%u phys=0x%08X width=%u rdram_nonzero=%d (post-workload read)\n",
            n,
            phys,
            pair.colorImage.width,
            wr64_rdram_fb_has_pixels(app->core.RDRAM, phys, 64u) ? 1 : 0);
        fflush(stderr);
    }
}

static void wr64_fill_present_storage_from_rdp(
    RT64::Application* app,
    RT64::Present& present,
    const RT64::VI& vi,
    uint32_t rdp_color_phys,
    uint32_t rdp_width,
    bool exact_color_phys = false) {
    if (app == nullptr || app->core.RDRAM == nullptr || !vi.visible()) {
        present.storage.clear();
        present.storageSourcePhys = 0u;
        return;
    }
    const uint32_t rdp_phys = rdp_color_phys & 0x00FFFFFFu;
    uint32_t fb_addr = rdp_phys;
    if (!exact_color_phys && app->core.RDRAM != nullptr &&
        wr64_rdram_u32(app->core.RDRAM, 0x800DAB1Cu) == 3u) {
        fb_addr = wr64::mode3_present_color_phys(app->core.RDRAM, fb_addr);
    }
    if (fb_addr == 0u) {
        fb_addr = vi.fbAddress();
    }
    const hlslpp::uint2 fb_size = vi.fbSize();
    const uint8_t siz = vi.fbSiz();
    uint32_t fb_w = fb_size[0];
    uint32_t fb_h = fb_size[1];
    if (app->core.RDRAM != nullptr && wr64_rdram_u32(app->core.RDRAM, 0x800DAB1Cu) == 3u && rdp_width != 0u) {
        fb_w = rdp_width;
        fb_h = 240u;
    }
    if (siz < G_IM_SIZ_16b || fb_addr == 0u || fb_w == 0u || fb_h == 0u) {
        present.storage.clear();
        present.storageSourcePhys = 0u;
        return;
    }
    const uint32_t bytes = fb_w * fb_h << (siz - 1);
    present.storage.resize(bytes);
    std::memcpy(present.storage.data(), &app->core.RDRAM[fb_addr], bytes);
    present.storageSourcePhys = fb_addr;
    wr64_log_present_storage_sample(0u, fb_addr, present.storage, app->core.RDRAM);
}

static void wr64_adjust_video_interface_cb(
    const RT64::VI& vi,
    uint32_t texture_width,
    uint32_t texture_height,
    interop::VideoInterfaceCB& push_constants) {
    // Mode-3 title: sample the full 640x240 texture (FullScreenVS uv range is [0, 2]).
    const hlslpp::uint2 video_size = vi.fbSize();
    if (video_size[0] == 640u && video_size[1] == 240u && texture_width > 0u && texture_height > 0u) {
        push_constants.textureResolution = { float(texture_width), float(texture_height) };
        push_constants.videoResolution = { float(texture_width), float(texture_height) };
        push_constants.textureOffset = { 0.0f, 0.0f };
        push_constants.gamma = vi.gamma();
    }
}

RT64::VI wr64_prepare_present_vi(
    RT64::Application::Core& core,
    uint8_t* rdram,
    const RT64::RDP* rdp,
    const RT64::Workload& workload) {
    wr64::ensure_host_vi_visible(rdram);
    RT64::VI vi = core.decodeVI();

    if (rdp == nullptr || workload.fbPairCount == 0u) {
        return vi;
    }

    const RT64::FramebufferPair& last_pair = workload.fbPairs[workload.fbPairCount - 1u];
    const uint32_t rdp_color = getFramebufferPairColorAddress(workload, last_pair);
    const uint32_t rdp_width = rdp->colorImage.width;
    const uint32_t present_color = rdp_color & 0x00FFFFFFu;
    vi = wr64_fixup_mode3_present_vi(rdram, vi, present_color, rdp_width);
    if (workload.fbPairCount > 0u) {
        static std::atomic<uint32_t> sample_tag{100u};
        const uint32_t tag = sample_tag.fetch_add(1);
        if (tag <= 120u) {
            wr64::log_framebuffer_sample(rdram, 0x80000000u | present_color, tag);
        }
    }
    return vi;
}

void wr64_log_workload_fb_pairs(
    uint32_t send_dl_n,
    const RT64::Workload& workload,
    const RT64::RDP& rdp) {
    if (send_dl_n > 64u) {
        return;
    }

    fprintf(stderr,
        "[RT64] send_dl #%u workload fbPairCount=%u fbPairSubmitted=%u gameCallCount=%u\n",
        send_dl_n,
        workload.fbPairCount,
        workload.fbPairSubmitted,
        workload.gameCallCount);
    fprintf(stderr,
        "[RT64] send_dl #%u final RDP color=0x%08X fmt=%u siz=%u width=%u depth=0x%08X\n",
        send_dl_n,
        rdp.colorImage.address,
        rdp.colorImage.fmt,
        rdp.colorImage.siz,
        rdp.colorImage.width,
        rdp.depthImage.address);

    if (wr64_last_rdram != nullptr) {
        constexpr uint32_t kDisplayMode = 0x800DAB1Cu;
        constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;
        constexpr uint32_t kDisplayModeFbIdx = 0x800D45D8u;
        const uint32_t mode = wr64_rdram_u32(wr64_last_rdram, kDisplayMode);
        if (mode == 3u) {
            const uint32_t idx = wr64_rdram_u32(wr64_last_rdram, kDisplayModeFbIdx) & 1u;
            const uint32_t draw_fb = wr64_rdram_u32(wr64_last_rdram, kDisplayModeFbArray + idx * 4u);
            const uint32_t draw_phys = draw_fb & 0x00FFFFFFu;
            const uint32_t other_phys = wr64_rdram_u32(
                wr64_last_rdram, kDisplayModeFbArray + ((idx ^ 1u) * 4u)) & 0x00FFFFFFu;
            fprintf(stderr,
                "[RT64] send_dl #%u mode3 RDP color vs drawFB=0x%08X match_draw=%d match_other=%d\n",
                send_dl_n,
                draw_fb,
                (rdp.colorImage.address == draw_phys) ? 1 : 0,
                (rdp.colorImage.address == other_phys) ? 1 : 0);
        }
    }

    for (uint32_t i = 0; i < workload.fbPairCount; ++i) {
        const RT64::FramebufferPair& pair = workload.fbPairs[i];
        const uint32_t copy_color = getFramebufferPairColorAddress(workload, pair);
        fprintf(stderr,
            "[RT64] send_dl #%u fbPair[%u] color=0x%08X copyColor=0x%08X depth=0x%08X fmt=%u siz=%u width=%u "
            "projCount=%u gameCalls=%u drawColorEmpty=%d drawRect=(%d,%d)-(%d,%d) fillRectOnly=%d flush=%d preferRdpCopy=%d\n",
            send_dl_n,
            i,
            pair.colorImage.address,
            copy_color,
            pair.depthImage.address,
            pair.colorImage.fmt,
            pair.colorImage.siz,
            pair.colorImage.width,
            pair.projectionCount,
            pair.gameCallCount,
            pair.drawColorRect.isEmpty() ? 1 : 0,
            pair.drawColorRect.ulx,
            pair.drawColorRect.uly,
            pair.drawColorRect.lrx,
            pair.drawColorRect.lry,
            pair.fillRectOnly ? 1 : 0,
            static_cast<int>(pair.flushReason),
            workload.preferRdpColorForCopy ? 1 : 0);
        for (uint32_t p = 0; p < pair.projectionCount; ++p) {
            const RT64::Projection& proj = pair.projections[p];
            const char* type_name = "unknown";
            switch (proj.type) {
            case RT64::Projection::Type::Perspective:
                type_name = "persp";
                break;
            case RT64::Projection::Type::Rectangle:
                type_name = "rect";
                break;
            default:
                break;
            }
            fprintf(stderr,
                "[RT64] send_dl #%u fbPair[%u] proj[%u] type=%s gameCalls=%u\n",
                send_dl_n,
                i,
                p,
                type_name,
                proj.gameCallCount);
        }
    }
    fflush(stderr);
}

void wr64_log_gpu_color_targets(
    uint32_t send_dl_n,
    RT64::Application* app,
    const RT64::Workload& workload) {
    if (send_dl_n > 24u || app == nullptr || app->state == nullptr) {
        return;
    }

    auto log_target = [&](uint32_t phys, uint32_t width, uint32_t siz, const char* label) {
        if (phys == 0u) {
            return;
        }
        const RT64::RenderTargetKey key(
            phys, width, siz, RT64::Framebuffer::Type::Color);
        RT64::RenderTarget& target = app->state->renderTargetManager.get(key, true);
        fprintf(stderr,
            "[RT64] send_dl #%u GPU %s phys=0x%08X key_w=%u empty=%d tex=%ux%u\n",
            send_dl_n,
            label,
            phys,
            width,
            target.isEmpty() ? 1 : 0,
            target.width,
            target.height);
    };

    if (workload.fbPairCount > 0u) {
        const RT64::FramebufferPair& pair = workload.fbPairs[workload.fbPairCount - 1u];
        const uint32_t phys = getFramebufferPairColorAddress(workload, pair) & 0x00FFFFFFu;
        log_target(phys, pair.colorImage.width, pair.colorImage.siz, "active");
    }

    if (app->core.RDRAM != nullptr && wr64_rdram_u32(app->core.RDRAM, 0x800DAB1Cu) == 3u) {
        constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;
        const uint32_t fb0 = wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbArray + 0u) & 0x00FFFFFFu;
        const uint32_t fb1 = wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbArray + 4u) & 0x00FFFFFFu;
        constexpr uint32_t kMode3Width = 640u;
        constexpr uint32_t kMode3Siz = 2u; // G_IM_SIZ_16b
        log_target(fb0, kMode3Width, kMode3Siz, "mode3_fb0");
        log_target(fb1, kMode3Width, kMode3Siz, "mode3_fb1");
    }
    fflush(stderr);
}

} // namespace

struct TexturePackEnableAction {
    std::string mod_id;
};

struct TexturePackDisableAction {
    std::string mod_id;
};

struct TexturePackSecondaryEnableAction {
    std::string mod_id;
};

struct TexturePackSecondaryDisableAction {
    std::string mod_id;
};

struct TexturePackUpdateAction {
};

using TexturePackAction = std::variant<TexturePackEnableAction, TexturePackDisableAction, TexturePackSecondaryEnableAction, TexturePackSecondaryDisableAction, TexturePackUpdateAction>;

static moodycamel::ConcurrentQueue<TexturePackAction> texture_pack_action_queue;

unsigned int MI_INTR_REG = 0;

unsigned int DPC_START_REG = 0;
unsigned int DPC_END_REG = 0;
unsigned int DPC_CURRENT_REG = 0;
unsigned int DPC_STATUS_REG = 0;
unsigned int DPC_CLOCK_REG = 0;
unsigned int DPC_BUFBUSY_REG = 0;
unsigned int DPC_PIPEBUSY_REG = 0;
unsigned int DPC_TMEM_REG = 0;

void dummy_check_interrupts() {}

RT64::UserConfiguration::Antialiasing compute_max_supported_aa(RT64::RenderSampleCounts bits) {
    if (bits & RT64::RenderSampleCount::Bits::COUNT_2) {
        if (bits & RT64::RenderSampleCount::Bits::COUNT_4) {
            if (bits & RT64::RenderSampleCount::Bits::COUNT_8) {
                return RT64::UserConfiguration::Antialiasing::MSAA8X;
            }
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        }
        return RT64::UserConfiguration::Antialiasing::MSAA2X;
    };
    return RT64::UserConfiguration::Antialiasing::None;
}

RT64::UserConfiguration::AspectRatio to_rt64(ultramodern::renderer::AspectRatio option) {
    switch (option) {
        case ultramodern::renderer::AspectRatio::Original:
            return RT64::UserConfiguration::AspectRatio::Original;
        case ultramodern::renderer::AspectRatio::Expand:
            return RT64::UserConfiguration::AspectRatio::Expand;
        case ultramodern::renderer::AspectRatio::Manual:
            return RT64::UserConfiguration::AspectRatio::Manual;
        case ultramodern::renderer::AspectRatio::OptionCount:
            return RT64::UserConfiguration::AspectRatio::OptionCount;
    }
}

RT64::UserConfiguration::Antialiasing to_rt64(ultramodern::renderer::Antialiasing option) {
    switch (option) {
        case ultramodern::renderer::Antialiasing::None:
            return RT64::UserConfiguration::Antialiasing::None;
        case ultramodern::renderer::Antialiasing::MSAA2X:
            return RT64::UserConfiguration::Antialiasing::MSAA2X;
        case ultramodern::renderer::Antialiasing::MSAA4X:
            return RT64::UserConfiguration::Antialiasing::MSAA4X;
        case ultramodern::renderer::Antialiasing::MSAA8X:
            return RT64::UserConfiguration::Antialiasing::MSAA8X;
        case ultramodern::renderer::Antialiasing::OptionCount:
            return RT64::UserConfiguration::Antialiasing::OptionCount;
    }
}

RT64::UserConfiguration::RefreshRate to_rt64(ultramodern::renderer::RefreshRate option) {
    switch (option) {
        case ultramodern::renderer::RefreshRate::Original:
            return RT64::UserConfiguration::RefreshRate::Original;
        case ultramodern::renderer::RefreshRate::Display:
            return RT64::UserConfiguration::RefreshRate::Display;
        case ultramodern::renderer::RefreshRate::Manual:
            return RT64::UserConfiguration::RefreshRate::Manual;
        case ultramodern::renderer::RefreshRate::OptionCount:
            return RT64::UserConfiguration::RefreshRate::OptionCount;
    }
}

RT64::UserConfiguration::InternalColorFormat to_rt64(ultramodern::renderer::HighPrecisionFramebuffer option) {
    switch (option) {
        case ultramodern::renderer::HighPrecisionFramebuffer::Off:
            return RT64::UserConfiguration::InternalColorFormat::Standard;
        case ultramodern::renderer::HighPrecisionFramebuffer::On:
            return RT64::UserConfiguration::InternalColorFormat::High;
        case ultramodern::renderer::HighPrecisionFramebuffer::Auto:
            return RT64::UserConfiguration::InternalColorFormat::Automatic;
        case ultramodern::renderer::HighPrecisionFramebuffer::OptionCount:
            return RT64::UserConfiguration::InternalColorFormat::OptionCount;
    }
}

void set_application_user_config(RT64::Application* application, const ultramodern::renderer::GraphicsConfig& config) {
    switch (config.res_option) {
        default:
        case ultramodern::renderer::Resolution::Auto:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;
            application->userConfig.downsampleMultiplier = 1;
            break;
        case ultramodern::renderer::Resolution::Original:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
        case ultramodern::renderer::Resolution::Original2x:
            application->userConfig.resolution = RT64::UserConfiguration::Resolution::Manual;
            application->userConfig.resolutionMultiplier = 2.0 * std::max(config.ds_option, 1);
            application->userConfig.downsampleMultiplier = std::max(config.ds_option, 1);
            break;
    }

    switch (config.hr_option) {
        default:
        case ultramodern::renderer::HUDRatioMode::Original:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
            break;
        case ultramodern::renderer::HUDRatioMode::Clamp16x9:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
            application->userConfig.extAspectTarget = 16.0/9.0;
            break;
        case ultramodern::renderer::HUDRatioMode::Full:
            application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Expand;
            break;
    }

    application->userConfig.aspectRatio = to_rt64(config.ar_option);
    application->userConfig.antialiasing = to_rt64(config.msaa_option);
    application->userConfig.refreshRate = to_rt64(config.rr_option);
    application->userConfig.refreshRateTarget = config.rr_manual_value;
    application->userConfig.internalColorFormat = to_rt64(config.hpfb_option);
    application->userConfig.displayBuffering = RT64::UserConfiguration::DisplayBuffering::Triple;

    // WR64 mode-3 title frames are RDP/RDRAM fills (see send_dl root_tri=0). Upscaling creates
    // empty 1600x876 GPU targets that the VI samples as black while RDRAM already has pixels.
    application->userConfig.resolution = RT64::UserConfiguration::Resolution::Original;
    application->userConfig.resolutionMultiplier = 1.0f;
    application->userConfig.downsampleMultiplier = 1;
    application->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Original;
    application->userConfig.extAspectRatio = RT64::UserConfiguration::AspectRatio::Original;
    // Title present blits to the B8G8R8A8 swap chain; HDR R16 internal targets caused D3D12 PSO
    // format mismatch and coloured noise when copying with the resolve pipeline.
    application->userConfig.internalColorFormat = RT64::UserConfiguration::InternalColorFormat::Standard;
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult rt64_result) {
    switch (rt64_result) {
        case RT64::Application::SetupResult::Success:
            return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound:
            return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI:
            return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound:
            return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound:
            return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }

    fprintf(stderr, "Unhandled `RT64::Application::SetupResult` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

ultramodern::renderer::GraphicsApi map_graphics_api(RT64::UserConfiguration::GraphicsAPI api) {
    switch (api) {
        case RT64::UserConfiguration::GraphicsAPI::D3D12:
            return ultramodern::renderer::GraphicsApi::D3D12;
        case RT64::UserConfiguration::GraphicsAPI::Vulkan:
            return ultramodern::renderer::GraphicsApi::Vulkan;
        case RT64::UserConfiguration::GraphicsAPI::Metal:
            return ultramodern::renderer::GraphicsApi::Metal;
        case RT64::UserConfiguration::GraphicsAPI::Automatic:
            return ultramodern::renderer::GraphicsApi::Auto;
        case RT64::UserConfiguration::GraphicsAPI::OptionCount:
            break;
    }

    fprintf(stderr, "Unhandled `RT64::UserConfiguration::GraphicsAPI` ?\n");
    assert(false);
    std::exit(EXIT_FAILURE);
}

zelda64::renderer::RT64Context::RT64Context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool debug) {
    static unsigned char dummy_rom_header[0x40];
    recompui::set_render_hooks();
    RT64::SetRenderHookTrace([](const char* message) {
        if (zelda64::startup_verbose_enabled()) {
            fprintf(stderr, "[startup] %s\n", message);
            fflush(stderr);
        }
    });
    RT64::SetRenderHookPresent([]() {
        ultramodern::signal_host_frame_presented();
        if (ultramodern::is_game_started()) {
            recompui::notify_game_gfx_submitted();
        }
        if (!wr64::stock_boot_enabled() && ultramodern::is_game_started() && ::wr64_last_rdram != nullptr) {
            wr64::on_present(::wr64_last_rdram);
        }
    });
    RT64::SetRenderHookAdjustVideoCB(wr64_adjust_video_interface_cb);
    // Set up the RT64 application core fields.
    RT64::Application::Core appCore{};
#if defined(_WIN32)
    appCore.window = window_handle.window;
#elif defined(__linux__) || defined(__ANDROID__)
    appCore.window = window_handle;
#elif defined(__APPLE__)
    appCore.window.window = window_handle.window;
    appCore.window.view = window_handle.view;
#endif

    appCore.checkInterrupts = dummy_check_interrupts;

    appCore.HEADER = dummy_rom_header;
    appCore.RDRAM = rdram;
    appCore.DMEM = DMEM;
    appCore.IMEM = IMEM;

    appCore.MI_INTR_REG = &MI_INTR_REG;

    appCore.DPC_START_REG = &DPC_START_REG;
    appCore.DPC_END_REG = &DPC_END_REG;
    appCore.DPC_CURRENT_REG = &DPC_CURRENT_REG;
    appCore.DPC_STATUS_REG = &DPC_STATUS_REG;
    appCore.DPC_CLOCK_REG = &DPC_CLOCK_REG;
    appCore.DPC_BUFBUSY_REG = &DPC_BUFBUSY_REG;
    appCore.DPC_PIPEBUSY_REG = &DPC_PIPEBUSY_REG;
    appCore.DPC_TMEM_REG = &DPC_TMEM_REG;

    ultramodern::renderer::ViRegs* vi_regs = ultramodern::renderer::get_vi_regs();

    appCore.VI_STATUS_REG = &vi_regs->VI_STATUS_REG;
    appCore.VI_ORIGIN_REG = &vi_regs->VI_ORIGIN_REG;
    appCore.VI_WIDTH_REG = &vi_regs->VI_WIDTH_REG;
    appCore.VI_INTR_REG = &vi_regs->VI_INTR_REG;
    appCore.VI_V_CURRENT_LINE_REG = &vi_regs->VI_V_CURRENT_LINE_REG;
    appCore.VI_TIMING_REG = &vi_regs->VI_TIMING_REG;
    appCore.VI_V_SYNC_REG = &vi_regs->VI_V_SYNC_REG;
    appCore.VI_H_SYNC_REG = &vi_regs->VI_H_SYNC_REG;
    appCore.VI_LEAP_REG = &vi_regs->VI_LEAP_REG;
    appCore.VI_H_START_REG = &vi_regs->VI_H_START_REG;
    appCore.VI_V_START_REG = &vi_regs->VI_V_START_REG;
    appCore.VI_V_BURST_REG = &vi_regs->VI_V_BURST_REG;
    appCore.VI_X_SCALE_REG = &vi_regs->VI_X_SCALE_REG;
    appCore.VI_Y_SCALE_REG = &vi_regs->VI_Y_SCALE_REG;

    // Set up the RT64 application configuration fields.
    RT64::ApplicationConfiguration appConfig;
    appConfig.useConfigurationFile = false;

    // Create the RT64 application.
    app = std::make_unique<RT64::Application>(appCore, appConfig);

    // Set initial user config settings based on the current settings.
    auto& cur_config = ultramodern::renderer::get_graphics_config();
    set_application_user_config(app.get(), cur_config);
    app->userConfig.developerMode = debug;
    // Force gbi depth branches to prevent LODs from kicking in.
    app->enhancementConfig.f3dex.forceBranch = true;
    // Scale LODs based on the output resolution.
    app->enhancementConfig.textureLOD.scale = true;
    // Pick an API if the user has set an override.
    switch (cur_config.api_option) {
        case ultramodern::renderer::GraphicsApi::D3D12:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::D3D12;
            break;
        case ultramodern::renderer::GraphicsApi::Vulkan:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Vulkan;
            break;
        case ultramodern::renderer::GraphicsApi::Metal:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
            break;
        case ultramodern::renderer::GraphicsApi::Auto:
            app->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Automatic;
            break;
        case ultramodern::renderer::GraphicsApi::OptionCount:
            break;
    }

    // Set up the RT64 application.
    uint32_t thread_id = 0;
#ifdef _WIN32
    thread_id = window_handle.thread_id;
#endif
    setup_result = map_setup_result(app->setup(thread_id));
    // Get the API that RT64 chose.
    chosen_api = map_graphics_api(app->chosenGraphicsAPI);
    if (setup_result != ultramodern::renderer::SetupResult::Success) {
        fprintf(stderr, "RT64 setup failed (result=%d, api=%d)\n",
            static_cast<int>(setup_result), static_cast<int>(chosen_api));
        fflush(stderr);
        app = nullptr;
        return;
    }

    // Set the application's fullscreen state.
    app->setFullScreen(cur_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);

    // Check if the selected device actually supports MSAA sample positions and MSAA for for the formats that will be used
    // and downgrade the configuration accordingly.
    if (app->device->getCapabilities().sampleLocations) {
        RT64::RenderSampleCounts color_sample_counts = app->device->getSampleCountsSupported(RT64::RenderFormat::R8G8B8A8_UNORM);
        RT64::RenderSampleCounts depth_sample_counts = app->device->getSampleCountsSupported(RT64::RenderFormat::D32_FLOAT);
        RT64::RenderSampleCounts common_sample_counts = color_sample_counts & depth_sample_counts;
        device_max_msaa = compute_max_supported_aa(common_sample_counts);
        sample_positions_supported = true;
    }
    else {
        device_max_msaa = RT64::UserConfiguration::Antialiasing::None;
        sample_positions_supported = false;
    }

    high_precision_fb_enabled = app->shaderLibrary->usesHDR;
}

zelda64::renderer::RT64Context::~RT64Context() = default;

static void wr64_patch_mode3_dl_bootstrap_color(uint8_t* rdram, uint32_t data_ptr, uint32_t data_size) {
    if (rdram == nullptr || wr64_rdram_u32(rdram, 0x800DAB1Cu) != 3u) {
        return;
    }
    const uint32_t idx = wr64_rdram_u32(rdram, 0x800D45D8u) & 1u;
    const uint32_t fb_phys = wr64_rdram_u32(rdram, 0x800D45DCu + idx * 4u) & 0x00FFFFFFu;
    if (fb_phys == 0u) {
        return;
    }
    for (uint32_t off = 0; off + 8u <= data_size; off += 8u) {
        uint32_t w0 = *reinterpret_cast<uint32_t*>(rdram + data_ptr + off);
        if (static_cast<uint8_t>(w0 >> 24) == 0xFFu) {
            // Mode-3 title uses 640-wide RGBA5551 framebuffers; bootstrap DLs often
            // emit width=320 which leaves a stale 320-wide FB entry for 0x002D4000.
            w0 = (w0 & 0xFFFFF000u) | 0x27Fu;
            *reinterpret_cast<uint32_t*>(rdram + data_ptr + off) = w0;
            *reinterpret_cast<uint32_t*>(rdram + data_ptr + off + 4u) = fb_phys;
            break;
        }
    }
}

void zelda64::renderer::RT64Context::send_dl(const OSTask* task) {
    static std::atomic<uint32_t> dl_count{0};
    const uint32_t n = dl_count.fetch_add(1) + 1;
    if (n == 1 && ultramodern::is_game_started()) {
        recompui::notify_game_gfx_submitted();
    }
    const uint32_t data_ptr = static_cast<uint32_t>(task->t.data_ptr) & 0x3FFFFFFu;
    const uint32_t data_end = data_ptr + task->t.data_size;
    if (n <= 64 || zelda64::startup_verbose_enabled()) {
        fprintf(stderr,
            "[RT64] send_dl #%u data_ptr=0x%08X data_end=0x%08X data_size=0x%X ucode=0x%08X ucode_data=0x%08X\n",
            n,
            data_ptr,
            data_end,
            task->t.data_size,
            static_cast<uint32_t>(task->t.ucode) & 0x3FFFFFFu,
            static_cast<uint32_t>(task->t.ucode_data) & 0x3FFFFFFu);
        fflush(stderr);
    }

    check_texture_pack_actions();
    wr64::set_gfx_workload_busy(true);
    wr64::set_skip_next_update_screen_present(true);
    struct GfxWorkloadScope {
        uint8_t* rdram;
        ~GfxWorkloadScope() {
            wr64::finish_gfx_task(rdram);
        }
    } gfx_scope{app->core.RDRAM};

    app->state->rsp->reset();
    const uint32_t ucode_text = static_cast<uint32_t>(task->t.ucode) & 0x3FFFFFFu;
    const uint32_t ucode_data = static_cast<uint32_t>(task->t.ucode_data) & 0x3FFFFFFu;

    auto setup_wr64_f3dwave = [&]() {
        RT64::GBI& gbi =
            app->interpreter->gbiManager.gbiCache[static_cast<uint32_t>(RT64::GBIUCode::F3DWAVE)];
        if (gbi.ucode == RT64::GBIUCode::Unknown) {
            gbi.ucode = RT64::GBIUCode::F3DWAVE;
            RT64::GBI_RDP::setup(&gbi, true);
            RT64::GBI_F3DWAVE::setup(&gbi);
        }
        app->interpreter->hleGBI = &gbi;
        app->state->rsp->setGBI(&gbi);
        if (gbi.resetFromTask != nullptr) {
            gbi.resetFromTask(app->state.get());
        }
    };

    // WR64 US Rev A: custom gspFast3D @ 0x800D2380 / 0x800EE310 (F3DWAVE family).
    // RT64's hash DB entry is still marked "Needs confirmation", so skip lookup for this pair.
    if (ucode_text == 0x000D2380u && ucode_data == 0x000EE310u) {
        setup_wr64_f3dwave();
    } else {
        app->interpreter->loadUCodeGBI(ucode_text, ucode_data, true);
        if (app->interpreter->hleGBI == nullptr &&
            ucode_text == 0x000D2380u && ucode_data == 0x000EE310u) {
            setup_wr64_f3dwave();
        }
    }
    wr64_log_framebuffer_tracking(app->core.RDRAM, n, "pre", nullptr);
    wr64_patch_mode3_dl_bootstrap_color(app->core.RDRAM, data_ptr, task->t.data_size);
    wr64_log_dl_scan(app->core.RDRAM, data_ptr, task->t.data_size, n);
    app->processDisplayLists(app->core.RDRAM, data_ptr, data_end, true);
    if (n <= 16 || zelda64::startup_verbose_enabled()) {
        fprintf(stderr, "[RT64] send_dl #%u processDisplayLists returned\n", n);
        fflush(stderr);
    }
    // waitForWorkloadId alone returns after frame-0 starts; waitForIdle waits for the render
    // thread to finish the full workload before we read back GPU results on this thread.
    app->workloadQueue->waitForIdle();
    if (n <= 16 || zelda64::startup_verbose_enabled()) {
        fprintf(stderr, "[RT64] send_dl #%u waitForIdle returned\n", n);
        fflush(stderr);
    }
    const RT64::Workload& workload =
        app->workloadQueue->workloads[app->workloadQueue->previousWriteCursor()];
    wr64_sync_workload_framebuffer_to_rdram(app.get(), workload, n);
    wr64_log_framebuffer_tracking(app->core.RDRAM, n, "post", app->state->rdp.get());
    wr64_log_workload_fb_pairs(n, workload, *app->state->rdp);
    wr64_log_gpu_color_targets(n, app.get(), workload);
    if (workload.fbPairCount > 0u) {
        const RT64::FramebufferPair& last_pair = workload.fbPairs[workload.fbPairCount - 1u];
        const uint32_t draw_phys = getFramebufferPairColorAddress(workload, last_pair);
        wr64::set_last_mode3_draw_phys(draw_phys);
        wr64::set_last_mode3_draw_width(app->state->rdp->colorImage.width);
    }
    const RT64::VI present_vi =
        wr64_prepare_present_vi(app->core, app->core.RDRAM, app->state->rdp.get(), workload);
    wr64_log_present_targets(app->core.RDRAM, n, present_vi, workload);
    if (workload.fbPairCount > 0u) {
        // Gfx workloads: present from the completed workload before updateScreen() copies
        // present.storage from the game's VI (often the previous swap buffer in mode 3).
        RT64::Present& present = app->presentQueue->presents[app->presentQueue->writeCursor];
        present.fbOperations.clear();
        present.storage.clear();
        present.storageSourcePhys = 0u;
        const uint32_t rdp_phys =
            getFramebufferPairColorAddress(workload, workload.fbPairs[workload.fbPairCount - 1u]) &
            0x00FFFFFFu;
        RT64::VI resolved_vi = present_vi;
        if (app->core.RDRAM != nullptr && wr64_rdram_u32(app->core.RDRAM, 0x800DAB1Cu) == 3u) {
            resolved_vi = wr64_fixup_mode3_present_vi(
                app->core.RDRAM,
                present_vi,
                rdp_phys,
                app->state->rdp->colorImage.width);
            if (n <= 16u || zelda64::startup_verbose_enabled()) {
                constexpr uint32_t kDisplayModeFbArray = 0x800D45DCu;
                constexpr uint32_t kDisplayModeFbIdx = 0x800D45D8u;
                const uint32_t idx = wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbIdx) & 1u;
                const uint32_t fb0 =
                    wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbArray + 0u) & 0x00FFFFFFu;
                const uint32_t fb1 =
                    wr64_rdram_u32(app->core.RDRAM, kDisplayModeFbArray + 4u) & 0x00FFFFFFu;
                fprintf(stderr,
                    "[RT64] send_dl #%u mode3 present: rdp=0x%08X vi_origin=0x%08X fbBase=0x%08X "
                    "D_800D45DC={0x%08X,0x%08X} idx=%u rdram_nonzero=%d\n",
                    n,
                    rdp_phys,
                    resolved_vi.origin,
                    resolved_vi.fbAddress(),
                    fb0,
                    fb1,
                    idx,
                    wr64_rdram_fb_has_pixels(app->core.RDRAM, rdp_phys, 64u) ? 1 : 0);
                fflush(stderr);
            }
        }
        uint32_t present_color_phys = rdp_phys;
        if (app->core.RDRAM != nullptr && wr64_rdram_u32(app->core.RDRAM, 0x800DAB1Cu) == 3u) {
            const uint32_t present_phys = present_color_phys & 0x00FFFFFFu;
            const RT64::FramebufferPair& last_pair =
                workload.fbPairs[workload.fbPairCount - 1u];
            // Always snapshot GPU → RDRAM before memcpy storage (readback may have failed in sync).
            wr64_try_force_gpu_framebuffer_to_rdram(
                app.get(),
                present_phys,
                last_pair.colorImage.fmt,
                last_pair.colorImage.siz,
                last_pair.colorImage.width != 0u ? last_pair.colorImage.width : 640u,
                240u,
                0u,
                240u,
                n,
                true,
                true);
        }
        {
            present.screenVI = resolved_vi;
            wr64_fill_present_storage_from_rdp(
                app.get(),
                present,
                resolved_vi,
                present_color_phys,
                app->state->rdp->colorImage.width,
                true);
            if (!present.storage.empty()) {
                app->state->lastScreenHash =
                    XXH3_64bits(present.storage.data(), present.storage.size());
            }
            app->state->advancePresent(present, false);
            app->presentQueue->advanceToNextPresent();
            wr64::set_skip_next_update_screen_present(true);
            app->state->lastScreenVI = present.screenVI;
            if (n <= 16 || zelda64::startup_verbose_enabled()) {
                fprintf(stderr,
                    "[RT64] send_dl #%u forced present: fbPairs=%u VI origin=0x%08X fbBase=0x%08X visible=%d workloadId=%llu\n",
                    n,
                    workload.fbPairCount,
                    present.screenVI.origin,
                    present.screenVI.fbAddress(),
                    present.screenVI.visible() ? 1 : 0,
                    static_cast<unsigned long long>(present.workloadId));
                fflush(stderr);
            }
        }
    } else {
        const bool bootstrap_only = (workload.gameCallCount == 0u);
        const uint32_t fb_phys = present_vi.fbAddress();
        const bool fb_has_pixels =
            fb_phys != 0u && wr64_rdram_fb_has_pixels(app->core.RDRAM, fb_phys, 64u);
        if (!bootstrap_only || fb_has_pixels) {
            update_screen();
        } else if (n <= 16u || zelda64::startup_verbose_enabled()) {
            fprintf(stderr,
                "[RT64] send_dl #%u skipped bootstrap present: fbPairs=0 gameCalls=0 empty FB 0x%08X\n",
                n,
                fb_phys);
            fflush(stderr);
        }
        if (n <= 16 || zelda64::startup_verbose_enabled()) {
            fprintf(stderr,
                "[RT64] send_dl #%u done: fbPairs=%u gameCalls=%u VI origin=0x%08X fbBase=0x%08X "
                "visible=%d presentId=%llu bootstrap_skip=%d\n",
                n,
                workload.fbPairCount,
                workload.gameCallCount,
                present_vi.origin,
                present_vi.fbAddress(),
                present_vi.visible() ? 1 : 0,
                static_cast<unsigned long long>(app->state->presentId),
                (bootstrap_only && !fb_has_pixels) ? 1 : 0);
            fflush(stderr);
        }
    }
}

void zelda64::renderer::RT64Context::update_screen() {
    if (app == nullptr) {
        return;
    }
    app->appWindow->sdlCheckFilterInstallation();
    app->screenApiProfiler.logAndRestart();
    if (app->core.RDRAM != nullptr) {
        wr64::ensure_host_vi_visible(app->core.RDRAM);
    }
    RT64::VI vi = app->core.decodeVI();
    if (app->core.RDRAM != nullptr) {
        const uint32_t last_draw = wr64::last_mode3_draw_phys();
        const uint32_t last_width = wr64::last_mode3_draw_width();
        if (last_draw != 0u) {
            uint32_t present_phys = last_draw;
            if (wr64_rdram_u32(app->core.RDRAM, 0x800DAB1Cu) == 3u) {
                present_phys = wr64::mode3_present_color_phys(app->core.RDRAM, present_phys);
            }
            vi = wr64_fixup_mode3_present_vi(
                app->core.RDRAM,
                vi,
                present_phys,
                last_width);
        }
    }
    app->state->updateScreen(vi, false);
}

void zelda64::renderer::RT64Context::shutdown() {
    if (app != nullptr) {
        app->end();
    }
}

bool zelda64::renderer::RT64Context::update_config(const ultramodern::renderer::GraphicsConfig& old_config, const ultramodern::renderer::GraphicsConfig& new_config) {
    if (old_config == new_config) {
        return false;
    }

    if (new_config.wm_option != old_config.wm_option) {
        app->setFullScreen(new_config.wm_option == ultramodern::renderer::WindowMode::Fullscreen);
    }

    set_application_user_config(app.get(), new_config);

    app->updateUserConfig(true);

    if (new_config.msaa_option != old_config.msaa_option) {
        app->updateMultisampling();
    }
    return true;
}

void zelda64::renderer::RT64Context::enable_instant_present() {
    // WR64: PresentEarly makes updateScreen() a no-op while viHistory rarely matches
    // the first F3DWAVE color buffers, so the swapchain stays cleared black.
}

uint32_t zelda64::renderer::RT64Context::get_display_framerate() const {
    return app->presentQueue->ext.sharedResources->swapChainRate;
}

float zelda64::renderer::RT64Context::get_resolution_scale() const {
    constexpr int ReferenceHeight = 240;
    switch (app->userConfig.resolution) {
        case RT64::UserConfiguration::Resolution::WindowIntegerScale:
            if (app->sharedQueueResources->swapChainHeight > 0) {
                return std::max(float((app->sharedQueueResources->swapChainHeight + ReferenceHeight - 1) / ReferenceHeight), 1.0f);
            }
            else {
                return 1.0f;
            }
        case RT64::UserConfiguration::Resolution::Manual:
            return float(app->userConfig.resolutionMultiplier);
        case RT64::UserConfiguration::Resolution::Original:
        default:
            return 1.0f;
    }
}

void zelda64::renderer::RT64Context::check_texture_pack_actions() {
    bool packs_changed = false;
    TexturePackAction cur_action;
    while (texture_pack_action_queue.try_dequeue(cur_action)) {
        std::visit(overloaded{
            [&](TexturePackDisableAction &to_disable) {
                enabled_texture_packs.erase(to_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackEnableAction &to_enable) {
                enabled_texture_packs.insert(to_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryDisableAction &to_override_disable) {
                secondary_disabled_texture_packs.insert(to_override_disable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackSecondaryEnableAction &to_override_enable) {
                secondary_disabled_texture_packs.erase(to_override_enable.mod_id);
                packs_changed = true;
            },
            [&](TexturePackUpdateAction &) {
                packs_changed = true;
            }
        }, cur_action);
    }

    // If any packs were disabled, unload all packs and load all the active ones.
    if (packs_changed) {
        // Sort the enabled texture packs in reverse order so that earlier ones override later ones.
        std::vector<std::string> sorted_texture_packs{};
        sorted_texture_packs.reserve(enabled_texture_packs.size());
        for (const std::string& mod : enabled_texture_packs) {
            if (!secondary_disabled_texture_packs.contains(mod)) {
                sorted_texture_packs.emplace_back(mod);
            }
        }

        std::sort(sorted_texture_packs.begin(), sorted_texture_packs.end(),
            [](const std::string& lhs, const std::string& rhs) {
                return recomp::mods::get_mod_order_index(lhs) > recomp::mods::get_mod_order_index(rhs);
            }
        );

        // Build the path list from the sorted mod list.
        std::vector<RT64::ReplacementDirectory> replacement_directories;
        replacement_directories.reserve(enabled_texture_packs.size());
        for (const std::string &mod_id : sorted_texture_packs) {
            replacement_directories.emplace_back(RT64::ReplacementDirectory(recomp::mods::get_mod_filename(mod_id)));
        }

        if (!replacement_directories.empty()) {
            app->textureCache->loadReplacementDirectories(replacement_directories);
        }
        else {
            app->textureCache->clearReplacementDirectories();
        }
    }
}

RT64::UserConfiguration::Antialiasing zelda64::renderer::RT64MaxMSAA() {
    return device_max_msaa;
}

std::unique_ptr<ultramodern::renderer::RendererContext> zelda64::renderer::create_render_context(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<zelda64::renderer::RT64Context>(rdram, window_handle, developer_mode);
}

bool zelda64::renderer::RT64SamplePositionsSupported() {
    return sample_positions_supported;
}

bool zelda64::renderer::RT64HighPrecisionFBEnabled() {
    return high_precision_fb_enabled;
}

void zelda64::renderer::trigger_texture_pack_update() {
    texture_pack_action_queue.enqueue(TexturePackUpdateAction{});
}

void zelda64::renderer::enable_texture_pack(const recomp::mods::ModContext& context, const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackEnableAction{mod.manifest.mod_id});

    // Check for the texture pack enabled config option.
    const recomp::mods::ConfigSchema& config_schema = context.get_mod_config_schema(mod.manifest.mod_id);
    auto find_it = config_schema.options_by_id.find(zelda64::renderer::special_option_texture_pack_enabled);
    if (find_it != config_schema.options_by_id.end()) {
        const recomp::mods::ConfigOption& config_option = config_schema.options[find_it->second];

        if (is_texture_pack_enable_config_option(config_option, false)) {
            recomp::mods::ConfigValueVariant value_variant = context.get_mod_config_value(mod.manifest.mod_id, config_option.id);
            uint32_t value;
            if (uint32_t* value_ptr = std::get_if<uint32_t>(&value_variant)) {
                value = *value_ptr;
            }
            else {
                value = 0;
            }

            if (value) {
                zelda64::renderer::secondary_enable_texture_pack(mod.manifest.mod_id);
            }
            else {
                zelda64::renderer::secondary_disable_texture_pack(mod.manifest.mod_id);
            }
        }
    }
}

void zelda64::renderer::disable_texture_pack(const recomp::mods::ModHandle& mod) {
    texture_pack_action_queue.enqueue(TexturePackDisableAction{mod.manifest.mod_id});
}

void zelda64::renderer::secondary_enable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryEnableAction{mod_id});
}

void zelda64::renderer::secondary_disable_texture_pack(const std::string& mod_id) {
    texture_pack_action_queue.enqueue(TexturePackSecondaryDisableAction{mod_id});
}


// HD texture enable option. Must be an enum with two options.
// The first option is treated as disabled and the second option is treated as enabled.
bool zelda64::renderer::is_texture_pack_enable_config_option(const recomp::mods::ConfigOption& option, bool show_errors) {
    if (option.id == zelda64::renderer::special_option_texture_pack_enabled) {
        if (option.type != recomp::mods::ConfigOptionType::Enum) {
            if (show_errors) {
                recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + zelda64::renderer::special_option_texture_pack_enabled + "\"), but the config option is not an enum.").c_str());
            }
            return false;
        }

        const recomp::mods::ConfigOptionEnum &option_enum = std::get<recomp::mods::ConfigOptionEnum>(option.variant);
        if (option_enum.options.size() != 2) {
            if (show_errors) {
                recompui::message_box(("Mod has the special config option id for enabling an HD texture pack (\"" + zelda64::renderer::special_option_texture_pack_enabled + "\"), but the config option doesn't have exactly 2 values.").c_str());
            }
            return false;
        }

        return true;
    }
    return false;
}
