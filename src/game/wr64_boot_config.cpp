#include "wr64_boot_config.hpp"

#include "zelda_support.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace wr64 {

namespace {

bool g_stock_boot = false;

bool line_enables_stock_boot(const std::string& line) {
    if (line.find("stock_boot") == std::string::npos) {
        return false;
    }
    if (line.find("stock_boot=0") != std::string::npos ||
        line.find("stock_boot=false") != std::string::npos ||
        line.find("stock_boot=False") != std::string::npos) {
        return false;
    }
    return true;
}

} // namespace

void load_boot_config() {
    if (const char* env = std::getenv("WR64_STOCK_BOOT")) {
        if (env[0] != '\0' && env[0] != '0') {
            g_stock_boot = true;
            return;
        }
    }

    const std::filesystem::path portable = zelda64::get_program_path() / "portable.txt";
    std::ifstream file(portable);
    if (!file) {
        return;
    }

    std::string line;
    while (std::getline(file, line)) {
        if (line_enables_stock_boot(line)) {
            g_stock_boot = true;
            return;
        }
    }
}

bool stock_boot_enabled() {
    return g_stock_boot;
}

} // namespace wr64
