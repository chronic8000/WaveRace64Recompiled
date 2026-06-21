#ifndef __ZELDA_GAME_H__
#define __ZELDA_GAME_H__

#include <cstdint>
#include <span>
#include <vector>

namespace zelda64 {
    void quicksave_save();
    void quicksave_load();
    std::vector<uint8_t> decompress_mm(std::span<const uint8_t> compressed_rom);
    std::vector<uint8_t> decompress_wr64(std::span<const uint8_t> rom);
};

#endif
