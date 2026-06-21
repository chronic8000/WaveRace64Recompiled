#include "recomp_ui.h"
#include "zelda_config.h"
#include "zelda_support.h"
#include "wr64_boot_config.hpp"
#include "librecomp/game.hpp"
#include "ultramodern/ultramodern.hpp"
#include "RmlUi/Core.h"
#include "nfd.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <vector>

namespace {

constexpr const char* kLauncherBackgroundSrc = "wr64_launcher_bg.png";

bool should_auto_start_game() {
    if (const char* env = std::getenv("WR64_AUTO_START")) {
        if (env[0] != '\0' && env[0] != '0') {
            return true;
        }
    }
    const std::filesystem::path portable = zelda64::get_program_path() / "portable.txt";
    std::ifstream file(portable);
    if (!file) {
        return false;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.find("auto_start") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void preload_launcher_background() {
    const std::filesystem::path path = zelda64::get_asset_path(kLauncherBackgroundSrc);
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        fprintf(stderr, "[launcher] background not found: %s\n", path.string().c_str());
        fflush(stderr);
        return;
    }

    const std::streamsize size = file.tellg();
    if (size <= 0) {
        fprintf(stderr, "[launcher] background is empty: %s\n", path.string().c_str());
        fflush(stderr);
        return;
    }

    file.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<size_t>(size));
    if (!file.read(bytes.data(), size)) {
        fprintf(stderr, "[launcher] failed to read background: %s\n", path.string().c_str());
        fflush(stderr);
        return;
    }

    recompui::queue_image_from_bytes_file(kLauncherBackgroundSrc, bytes);
    fprintf(stderr, "[launcher] background queued %lld bytes from %s\n",
        static_cast<long long>(bytes.size()), path.string().c_str());
    fflush(stderr);
}

} // namespace

static std::string version_string;

Rml::DataModelHandle model_handle;
bool mm_rom_valid = false;

extern std::vector<recomp::GameEntry> supported_games;

void select_rom() {
    nfdnchar_t* native_path = nullptr;
    zelda64::open_file_dialog([](bool success, const std::filesystem::path& path) {
        if (success) {
            recomp::RomValidationError rom_error = recomp::select_rom(path, supported_games[0].game_id);
            switch (rom_error) {
                case recomp::RomValidationError::Good:
                    mm_rom_valid = true;
                    model_handle.DirtyVariable("mm_rom_valid");
                    break;
                case recomp::RomValidationError::FailedToOpen:
                    recompui::message_box("Failed to open ROM file.");
                    break;
                case recomp::RomValidationError::NotARom:
                    recompui::message_box("This is not a valid ROM file.");
                    break;
                case recomp::RomValidationError::IncorrectRom:
                    recompui::message_box("This ROM is not the correct game.");
                    break;
                case recomp::RomValidationError::NotYet:
                    recompui::message_box("This game isn't supported yet.");
                    break;
                case recomp::RomValidationError::IncorrectVersion:
                    recompui::message_box(
                            "This ROM is the correct game, but the wrong version.\nThis project requires the NTSC-U N64 version of the game.");
                    break;
                case recomp::RomValidationError::OtherError:
                    recompui::message_box("An unknown error has occurred.");
                    break;
            }
        }
    });
}

recompui::ContextId launcher_context;

static std::atomic<bool> g_auto_start_queued{false};

recompui::ContextId recompui::get_launcher_context_id() {
	return launcher_context;
}

void recompui::queue_auto_start_game() {
    g_auto_start_queued.store(true, std::memory_order_release);
}

void recompui::try_run_queued_auto_start(uint32_t host_frame_index) {
    if (!g_auto_start_queued.load(std::memory_order_acquire)) {
        return;
    }
    const uint32_t min_frame = 30u;
    if (host_frame_index < min_frame) {
        return;
    }
    if (!g_auto_start_queued.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    fprintf(stderr, "[launcher] auto_start: starting game (frame %u)\n", host_frame_index);
    fflush(stderr);
    recompui::reset_game_loading_overlay();
    recomp::start_game(supported_games[0].game_id);
    recompui::hide_all_contexts();
    recompui::open_notification("Loading", "Starting Wave Race 64...");
}

class LauncherMenu : public recompui::MenuController {
public:
    LauncherMenu() {
        mm_rom_valid = recomp::is_rom_valid(supported_games[0].game_id);
    }
    ~LauncherMenu() override {

    }
    void load_document() override {
        preload_launcher_background();
        launcher_context = recompui::create_context(zelda64::get_asset_path("launcher.rml"));
        if (mm_rom_valid && should_auto_start_game()) {
            const uint32_t min_frame = 30u;
            fprintf(stderr, "[launcher] auto_start: queued until frame %u\n", min_frame);
            fflush(stderr);
            recompui::queue_auto_start_game();
        }
    }
    void register_events(recompui::UiEventListenerInstancer& listener) override {
        recompui::register_event(listener, "select_rom",
            [](const std::string& param, Rml::Event& event) {
                select_rom();
            }
        );
        recompui::register_event(listener, "rom_selected",
            [](const std::string& param, Rml::Event& event) {
                mm_rom_valid = true;
                model_handle.DirtyVariable("mm_rom_valid");
            }
        );
        recompui::register_event(listener, "start_game",
            [](const std::string& param, Rml::Event& event) {
                recompui::reset_game_loading_overlay();
                recomp::start_game(supported_games[0].game_id);
                recompui::hide_all_contexts();
                recompui::open_notification("Loading", "Starting Wave Race 64...");
            }
        );
        recompui::register_event(listener, "open_controls",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::Controls);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_settings",
            [](const std::string& param, Rml::Event& event) {
                recompui::set_config_tab(recompui::ConfigTab::General);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "open_mods",
            [](const std::string &param, Rml::Event &event) {
                recompui::set_config_tab(recompui::ConfigTab::Mods);
                recompui::hide_all_contexts();
                recompui::show_context(recompui::get_config_context_id(), "");
            }
        );
        recompui::register_event(listener, "exit_game",
            [](const std::string& param, Rml::Event& event) {
                ultramodern::quit("launcher exit_game button");
            }
        );
    }
    void make_bindings(Rml::Context* context) override {
        Rml::DataModelConstructor constructor = context->CreateDataModel("launcher_model");

        constructor.Bind("mm_rom_valid", &mm_rom_valid);

        version_string = recomp::get_project_version().to_string();
        constructor.Bind("version_number", &version_string);

        model_handle = constructor.GetModelHandle();
    }
};

std::unique_ptr<recompui::MenuController> recompui::create_launcher_menu() {
    return std::make_unique<LauncherMenu>();
}
