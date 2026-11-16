#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <map>

namespace nwii {
namespace runtime {

enum class Platform {
    Wii,
    GameCube,
    WiiU
};

enum class Backend {
    OpenGL,
    Metal
};

// One virtual Wii Remote's host devices, [input.playerN] (N = 1..4).
// Action names: left right up down (screen directions, the remote is held
// sideways), two one a b plus minus home shake tilt_left tilt_right.
struct InputPlayerConfig {
    // auto (player 1: keyboard + first controller; 2-4: next controller),
    // keyboard, gamepad, keyboard+gamepad, none.
    std::string device = "auto";
    // Pick a specific controller: part of its name ("Xbox", "Joy-Con (R)"),
    // "#n" for the n-th controller found (0-based), or an SDL GUID. Empty =
    // the next free one.
    std::string gamepad;
    // Overrides of the default mapping, action -> list of names: SDL key
    // names for keys ("X", "Left", "Return") and SDL controller names for
    // buttons ("a", "b", "x", "y", "leftshoulder", "righttrigger", "start",
    // "back", "guide", "dpleft"...). Unlisted actions keep their default.
    std::map<std::string, std::vector<std::string>> keys;
    std::map<std::string, std::vector<std::string>> buttons;
    // Tilt (steering-wheel roll): auto (right stick X, else the controller's
    // accelerometer), stick, motion, none.
    std::string tilt = "auto";
    bool tilt_invert = false;
    bool rumble = true;
    bool configured = false;  // the [input.playerN] table exists
};

struct GameProfile {

    uint32_t callback_stack_top = 0;

    std::vector<std::string> disable_modules;

    
    std::string rpl_path;

    

    uint32_t ext_interrupt_dispatch = 0;

    

    uint32_t sched_lock_offset = 0;
};

class Config {
public:
    static Config& get() {
        static Config instance;
        return instance;
    }

    void load(const std::string& filepath);

    
    void load_game_profile();

    Platform platform = Platform::Wii;
    Backend backend = Backend::OpenGL;
    bool enable_vsync = true;
    int window_width = 640;
    int window_height = 480;
    std::string game_dir;

    
    
    std::string game_id = "RSZK";

    // Host directory that backs the emulated NAND (/title, /shared2, /tmp...).
    // [system] nand_dir; defaults to "nand" next to the config file
    // (save/ in the NRO folder on Switch).
    std::string nand_dir;

    // Wii system language (SYSCONF IPL.LNG): 0 ja, 1 en, 2 de, 3 fr, 4 es,
    // 5 it, 6 nl. [system] language = "auto" follows the host language.
    int system_language = 1;

    

    

    
    int input_mode = 6;

    float gyro_sensitivity = 1.0f;

    float pointer_speed = 1.0f;

    int phone_port = 4313;

    // Virtual Wii Remotes ([input] players = 1..4, [input.player1..4]).
    int input_players = 4;
    InputPlayerConfig input_player[4];
    // Joy-Cons: "single" = each Joy-Con is its own player held sideways,
    // "pair" = a left+right pair is one controller, "auto" = PC: as "pair"
    // (SDL default); Switch: player 1 pair, players 2-4 single. PC: SDL hint
    // SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS; Switch: Npad assignment mode.
    std::string joycon_mode = "auto";
    // Switch: open the controller applet by itself when the game waits for
    // another remote ("Add a remote" screens). The L3+R3 hotkey always works.
    bool nx_controller_applet = true;

    GameProfile game_profile;

    // Internal resolution ([graphics] internal_resolution): EFB pixels per GX
    // pixel. 0 = "auto" (PC: smallest scale whose 528*N EFB covers the window
    // height, capped at 4; Switch: 1 in handheld, 2 docked), 1-4 fixed. 1 is
    // the console's native 640x528 EFB. Kept last in the class: the generated
    // code reads Config (platform) and must keep the old member offsets.
    int internal_resolution = 0;
    // Switch only ([graphics] internal_resolution_handheld): the scale while
    // undocked, -1 = same as internal_resolution.
    int internal_resolution_handheld = -1;

private:
    Config() = default;
};

} 
} 
