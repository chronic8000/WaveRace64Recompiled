#pragma once

#include <cstdint>

namespace wr64 {

// Call once at startup (reads portable.txt / WR64_STOCK_BOOT env).
void load_boot_config();

// When true: no wr64_assets HLE patches, stock VI mesg delivery, no present-driven vi_pulse.
bool stock_boot_enabled();

// Keep RT64 host VI regs visible for present (WR64 hle_vi_swap vs ultramodern split).
void ensure_host_vi_visible(uint8_t* rdram);

// Boot diagnostics: sample first pixels of an RDRAM framebuffer.
void log_framebuffer_sample(uint8_t* rdram, uint32_t fb, uint32_t tag);

// Mode-3 double buffer: point host VI at the RDP color buffer that was just drawn.
uint32_t mode3_host_vi_origin_for_color_fb(uint8_t* rdram, uint32_t color_phys, uint32_t width_pixels = 0);
void commit_mode3_draw_framebuffer(uint8_t* rdram, uint32_t rdp_color_phys);

// After Gfx/RDP completes: point host VI swap + origin at the drawn mode-3 framebuffer.
void sync_host_vi_to_mode3_draw(uint8_t* rdram, uint32_t rdp_color_phys, uint32_t rdp_color_width);

// Mode-3 present: prefer the double-buffer entry that has visible RDRAM pixels.
uint32_t mode3_present_color_phys(uint8_t* rdram, uint32_t rdp_color_phys);

// Last mode-3 RDP color buffer (for updateScreen between gfx workloads).
void set_last_mode3_draw_phys(uint32_t rdp_color_phys);
uint32_t last_mode3_draw_phys();
void set_last_mode3_draw_width(uint32_t rdp_color_width);
uint32_t last_mode3_draw_width();

// Gfx/SysMain sync: block SysMain until RT64 send_dl (including copyNativeToRAM) completes.
void set_gfx_workload_busy(bool busy);
bool gfx_pipeline_busy(uint8_t* rdram);
void wait_hw_gfx_idle(uint8_t* rdram);
void finish_gfx_task(uint8_t* rdram);

// Called from RT64 when a swapchain present includes visible framebuffer pixels.
void notify_nonempty_swapchain_present();
bool nonempty_swapchain_presented();
void reset_nonempty_swapchain_presented();

// send_dl forced present: skip the following updateScreen() duplicate present.
void set_skip_next_update_screen_present(bool skip);
bool consume_skip_next_update_screen_present();

} // namespace wr64
