#pragma once

/* WR64 input / Controller Pak / rumble hooks (runtime stubs until full hardware parity). */

#include <cstdint>

namespace wr64::input {
    void init();
    void poll();
    void rumble_start();
    void rumble_stop();
    bool controller_pak_present(int port);
}
