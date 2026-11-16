#include "runtime/config.h"
#include "runtime/res_scale.h"
#include <toml++/toml.hpp>
#include <algorithm>
#include <string>
#include <iostream>
#include <filesystem>
#include <cstdlib>
#ifdef __SWITCH__
#include <switch.h>
#endif

namespace nwii {
namespace runtime {

res::Spec res::g_cfg[2] = {{res::Spec::Auto, 0.0f}, {}};  // r16res (res_scale.h)

// [input.playerN] entry: a string or an array of strings.
static std::vector<std::string> toml_names(const toml::node &n) {
    std::vector<std::string> out;
    if (auto s = n.value<std::string>()) {
        out.push_back(*s);
    } else if (auto arr = n.as_array()) {
        for (const auto &e : *arr)
            if (auto es = e.value<std::string>()) out.push_back(*es);
    }
    return out;
}

static void parse_input_player(const toml::table &t, InputPlayerConfig &p) {
    p.configured = true;
    if (auto v = t["device"].value<std::string>()) p.device = *v;
    if (auto v = t["gamepad"].value<std::string>()) p.gamepad = *v;
    if (auto v = t["tilt"].value<std::string>()) p.tilt = *v;
    if (auto v = t["tilt_invert"].value<bool>()) p.tilt_invert = *v;
    if (auto v = t["rumble"].value<bool>()) p.rumble = *v;
    if (auto keys = t["keys"].as_table())
        for (const auto &[k, v] : *keys) p.keys[std::string(k.str())] = toml_names(v);
    if (auto btns = t["buttons"].as_table())
        for (const auto &[k, v] : *btns) p.buttons[std::string(k.str())] = toml_names(v);
}

// IPL.LNG for "auto": the host's language, English unless it is one the US
// disc ships (Spanish, French).
static int host_wii_language() {
#ifdef __SWITCH__
    int lang = 1;
    if (R_SUCCEEDED(setInitialize())) {
        u64 code = 0;
        SetLanguage sl;
        if (R_SUCCEEDED(setGetSystemLanguage(&code)) &&
            R_SUCCEEDED(setMakeLanguage(code, &sl))) {
            if (sl == SetLanguage_ES || sl == SetLanguage_ES419) lang = 4;
            else if (sl == SetLanguage_FR || sl == SetLanguage_FRCA) lang = 3;
        }
        setExit();
    }
    return lang;
#else
    for (const char *var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char *v = std::getenv(var);
        if (!v || !*v) continue;
        std::string s(v);
        if (s.rfind("es", 0) == 0) return 4;
        if (s.rfind("fr", 0) == 0) return 3;
        return 1;
    }
    return 1;
#endif
}

static int parse_wii_language(const std::string &name) {
    static const struct { const char *name; int code; } table[] = {
        {"ja", 0}, {"en", 1}, {"de", 2}, {"fr", 3},
        {"es", 4}, {"it", 5}, {"nl", 6},
    };
    for (const auto &e : table)
        if (name == e.name) return e.code;
    return host_wii_language();  // "auto" or anything unknown
}

void Config::load(const std::string& filepath) {
#ifdef __SWITCH__
    // save/ next to config.toml (the NRO's folder; older installs: nand/);
    // filesystem::absolute does not understand "sdmc:/" paths.
    {
        size_t slash = filepath.rfind('/');
        std::string dir = slash == std::string::npos ? "sdmc:/nsmbw" : filepath.substr(0, slash);
        nand_dir = dir + "/save";
        if (!std::filesystem::exists(nand_dir) && std::filesystem::exists(dir + "/nand"))
            nand_dir = dir + "/nand";
    }
    // The Switch screen (and its docked output, scaled by the system) is
    // 1280x720; the game renders 16:9 (SYSCONF IPL.AR = 1), so the XFB fills
    // it with the right aspect.
    window_width = 1280;
    window_height = 720;
#else
    nand_dir = (std::filesystem::absolute(filepath).parent_path() / "nand").string();
#endif
    system_language = host_wii_language();

    if (!std::filesystem::exists(filepath)) {
        std::cout << "[Config] Config file " << filepath << " not found, using defaults." << std::endl;
        return;
    }

    try {
        toml::table tbl = toml::parse_file(filepath);

        auto plat = tbl["system"]["platform"].value<std::string>();
        if (plat) {
            if (*plat == "GameCube" || *plat == "gamecube") {
                platform = Platform::GameCube;
                std::cout << "[Config] Platform set to GameCube." << std::endl;
            } else if (*plat == "WiiU" || *plat == "wiiu" || *plat == "Wii U") {
                platform = Platform::WiiU;
                std::cout << "[Config] Platform set to Wii U." << std::endl;
            } else {
                platform = Platform::Wii;
                std::cout << "[Config] Platform set to Wii." << std::endl;
            }
        }

        auto vsync = tbl["graphics"]["vsync"].value<bool>();
        if (vsync) enable_vsync = *vsync;

        auto back = tbl["graphics"]["backend"].value<std::string>();
        if (back) {
            if (*back == "Metal" || *back == "metal") {
                backend = Backend::Metal;
                std::cout << "[Config] Backend set to Metal." << std::endl;
            } else if (*back == "deko3d" || *back == "Deko3d" || *back == "dk") {
                // r15dk: deko3d es GL por debajo (dkgl); config.h no se toca: se pasa por el entorno
                // (debug.toml NWII_BACKEND tiene prioridad)
                backend = Backend::OpenGL;
                setenv("NWII_BACKEND", "deko3d", 0);
                std::cout << "[Config] Backend set to deko3d (Switch; OpenGL elsewhere)." << std::endl;
            } else {
                backend = Backend::OpenGL;
                std::cout << "[Config] Backend set to OpenGL." << std::endl;
            }
        }

        auto nand = tbl["system"]["nand_dir"].value<std::string>();
        if (nand) nand_dir = *nand;

        system_language = parse_wii_language(
            tbl["system"]["language"].value_or(std::string("auto")));
        std::cout << "[Config] System language (IPL.LNG) = " << system_language
                  << std::endl;

        auto w = tbl["graphics"]["window_width"].value<int>();
        if (w) window_width = *w;

        auto h = tbl["graphics"]["window_height"].value<int>();
        if (h) window_height = *h;

        // [graphics] internal_resolution = "auto" | 1..4 (and, Switch only,
        // internal_resolution_handheld): "auto" or anything unknown = 0.
        auto parse_scale = [&](const char *key, int def) {
            auto node = tbl["graphics"][key];
            if (auto n = node.value<int64_t>())
                return (int)std::min<int64_t>(4, std::max<int64_t>(1, *n));
            if (auto str = node.value<std::string>())
                return (*str == "1" || *str == "2" || *str == "3" || *str == "4")
                           ? (*str)[0] - '0' : 0;
            return def;
        };
        internal_resolution = parse_scale("internal_resolution", 0);
        internal_resolution_handheld = parse_scale("internal_resolution_handheld", -1);
        // r16res: the same keys as a fractional spec (res_scale.h): "auto",
        // 1..4, "1.3636", "720p"... (the int fields above stay for the
        // generated code's view of Config; the renderer uses these).
        for (int k = 0; k < 2; ++k) {
            const char *key = k ? "internal_resolution_handheld" : "internal_resolution";
            auto node = tbl["graphics"][key];
            res::Spec sp;
            bool ok = true;
            if (auto n = node.value<double>()) ok = res::Parse(std::to_string(*n).c_str(), sp);
            else if (auto str = node.value<std::string>()) ok = res::Parse(str->c_str(), sp);
            if (!ok) {
                std::cout << "[Config] " << key << ": not understood, using auto" << std::endl;
                sp = {res::Spec::Auto, 0.0f};
            }
            if (!k && sp.kind == res::Spec::Unset) sp = {res::Spec::Auto, 0.0f};
            res::g_cfg[k] = sp;
        }
        std::cout << "[Config] Internal resolution: " << res::Describe(res::g_cfg[0])
                  << (res::g_cfg[1].kind != res::Spec::Unset
                          ? " (handheld " + res::Describe(res::g_cfg[1]) + ")" : std::string())
                  << std::endl;

        auto gid = tbl["game"]["id"].value<std::string>();
        if (gid) {
            game_id = *gid;
            std::cout << "[Config] Game ID override: " << game_id << std::endl;
        }

        auto mode = tbl["input"]["mode"].value<int>();
        if (mode) {
            input_mode = *mode;
            static const char* mode_names[] = {
                "", "wiimote-bluetooth", "gamepad-classic",
                "gamepad-pointer-assist", "gamepad-full-tilt",
                "smartphone", "keyboard-mouse", "touch"
            };
            if (input_mode >= 1 && input_mode <= 7)
                std::cout << "[Config] Input mode " << input_mode << " ("
                          << mode_names[input_mode] << ")" << std::endl;
            else
                std::cout << "[Config] Unknown input mode " << input_mode
                          << ", falling back to 6 (keyboard-mouse)" << std::endl;
        }

        auto gyro = tbl["input"]["gyro_sensitivity"].value<double>();
        if (gyro) gyro_sensitivity = (float)*gyro;

        auto pspeed = tbl["input"]["pointer_speed"].value<double>();
        if (pspeed) pointer_speed = (float)*pspeed;

        auto pport = tbl["input"]["phone_port"].value<int>();
        if (pport) phone_port = *pport;

        // Virtual Wii Remotes (multiplayer): [input] players, joycon_mode,
        // nx_controller_applet and one [input.playerN] table per player.
        if (auto np = tbl["input"]["players"].value<int>())
            input_players = *np < 1 ? 1 : (*np > 4 ? 4 : *np);
        if (auto jm = tbl["input"]["joycon_mode"].value<std::string>())
            joycon_mode = *jm;
#ifndef __SWITCH__
        // SDL reads hints from the environment too; this runs before
        // SDL_Init. "single": every Joy-Con is its own sideways controller.
        // "auto"/"pair" keep SDL's default (an L+R pair on SDL's HIDAPI
        // driver is one controller), so player 1 behaves as before. Joy-Cons
        // seen through hid-nintendo/evdev without joycond are separate pads
        // either way.
        if (joycon_mode == "single")
            setenv("SDL_JOYSTICK_HIDAPI_COMBINE_JOY_CONS", "0", 0);
        // Controllers keep working while the window is not focused (two
        // players, a second monitor...); [input] background_input = false
        // restores SDL's default (presses ignored without focus).
        if (tbl["input"]["background_input"].value_or(true))
            setenv("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1", 0);
#endif
        if (auto ap = tbl["input"]["nx_controller_applet"].value<bool>())
            nx_controller_applet = *ap;
        for (int i = 0; i < 4; ++i) {
            const std::string key = "player" + std::to_string(i + 1);
            if (auto pt = tbl["input"][key].as_table())
                parse_input_player(*pt, input_player[i]);
        }
        std::cout << "[Config] Wii Remotes: up to " << input_players
                  << " players, joycon_mode=" << joycon_mode;
        for (int i = 0; i < input_players; ++i)
            std::cout << ", P" << (i + 1) << "=" << input_player[i].device;
        std::cout << std::endl;

        auto eid = tbl["hle"]["ext_interrupt_dispatch"].value<int64_t>();
        if (eid && *eid != 0) {
            game_profile.ext_interrupt_dispatch = (uint32_t)*eid;
            std::cout << "[Config] ext_interrupt_dispatch = 0x"
                      << std::hex << game_profile.ext_interrupt_dispatch
                      << std::dec << std::endl;
        }

        auto slo = tbl["hle"]["sched_lock_offset"].value<int64_t>();
        if (slo && *slo != 0) {
            game_profile.sched_lock_offset = (uint32_t)*slo;
            std::cout << "[Config] sched_lock_offset = 0x"
                      << std::hex << game_profile.sched_lock_offset
                      << std::dec << std::endl;
        }

    } catch (const toml::parse_error& err) {

        std::cerr << "[Config] Failed to parse config.toml:\n" << err << std::endl;
    }
}

void Config::load_game_profile() {
    
    std::vector<std::string> search_paths = {
        "assets/games/" + game_id + ".toml",
        game_dir + "/../assets/games/" + game_id + ".toml",
    };

    std::string found_path;
    for (const auto& p : search_paths) {
        if (std::filesystem::exists(p)) {
            found_path = p;
            break;
        }
    }

    if (found_path.empty()) {
        std::cout << "[Config] No game profile for " << game_id << " (using defaults)." << std::endl;
        return;
    }

    try {
        toml::table tbl = toml::parse_file(found_path);

        auto cst = tbl["quirks"]["callback_stack_top"].value<int64_t>();
        if (cst) {
            game_profile.callback_stack_top = (uint32_t)*cst;
            std::cout << "[Config] Game profile: callback_stack_top = 0x"
                      << std::hex << game_profile.callback_stack_top << std::dec << std::endl;
        }

        if (auto mods = tbl["quirks"]["disable_module"].as_array()) {
            mods->for_each([this](const toml::value<std::string>& v) {
                game_profile.disable_modules.push_back(*v);
                std::cout << "[Config] Game profile: disable_module = " << *v << std::endl;
            });
        }

        auto rpl = tbl["game"]["rpl_path"].value<std::string>();
        if (rpl) game_profile.rpl_path = *rpl;

        auto eid = tbl["hle"]["ext_interrupt_dispatch"].value<int64_t>();
        if (eid) {
            game_profile.ext_interrupt_dispatch = (uint32_t)*eid;
            std::cout << "[Config] Game profile: ext_interrupt_dispatch = 0x"
                      << std::hex << game_profile.ext_interrupt_dispatch
                      << std::dec << std::endl;
        }

        std::cout << "[Config] Loaded game profile: " << found_path << std::endl;

    } catch (const toml::parse_error& err) {
        std::cerr << "[Config] Failed to parse game profile " << found_path
                  << ":\n" << err << std::endl;
    }
}

} 
} 
