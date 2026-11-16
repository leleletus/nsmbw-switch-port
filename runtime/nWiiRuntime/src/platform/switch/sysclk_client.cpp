// sysclk_client.cpp: minimal client for the sys-clk sysmodule (m6r; m6s:
// override dropped on focus loss and asked again on focus regain; m6t: exit
// locked while the session is open, so HOME -> close goes through the exit path).
//
// Protocol taken from the public sources (read on 16-nov-2026), not copied:
//  - retronx-team/sys-clk, master c86ce66 (2024-10-14):
//    common/include/sysclk/ipc.h, common/src/client/ipc.c,
//    common/include/sysclk/board.h, sysmodule/src/ipc_service.cpp.
//    Service "sys:clk"; cmd 0 = GetApiVersion (out u32), cmd 8 = SetOverride
//    (in {u32 module; u32 hz}, module 0 = CPU, hz 0 = remove the override).
//    SYSCLK_IPC_API_VERSION is 4 today; 0, 1, 2 and 3 (commits 44456d8,
//    4c5cee6, f36c52d, 9bc6542) have the same command numbers, the same
//    SetOverride arguments and CPU = 0.
//  - Horizon-OC/Horizon-OC (sys-clk fork), HEAD d7c9ad8 (2026-10-23):
//    Source/hoc-clk/common/include/hocclk/ipc.h, .../board.h,
//    .../src/client/ipc.c, sysmodule/src/ipc/ipc_service.cpp. Service
//    "hoc:clk", HOCCLK_IPC_API_VERSION 2, same cmd 0/8 and arguments.
//  - Atmosphere-NX/Atmosphere libstratosphere sm_ams.os.horizon.c: the
//    Atmosphère sm extension 65100 AtmosphereHasService (tipc, in
//    SmServiceName, out u8). smGetService() of a service nobody registered
//    waits forever, so we ask sm first and stay away if it is not there.
// sys-clk is under the "BEER-WARE LICENSE" (Revision 42) of p-sam,
// natinusala and m4xw ("you can do whatever you want with this stuff");
// Horizon OC is a fork of it. This file only re-implements the wire format.
//
// Any failure (no Atmosphère extension, service missing, unknown API
// version, refused call) = do nothing, quietly: one "[NX] sys-clk:" line on
// stdout, which only reaches a file/nxlink when logging is on.
#ifdef __SWITCH__
#include "sysclk_client.h"
#include "switch_platform.h"

#include <switch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nwii::runtime::nx {
namespace {

struct Backend {
  const char *name;
  u32 api_min, api_max;  // versions whose SetOverride we have read
};
constexpr Backend kBackends[] = {
    {"sys:clk", 0, 4},  // retronx-team/sys-clk
    {"hoc:clk", 2, 2},  // Horizon OC
};
constexpr u32 kCmdGetApiVersion = 0;
constexpr u32 kCmdSetOverride = 8;
constexpr u32 kModuleCpu = 0;
constexpr int kAllowedMhz[] = {1020, 1122, 1224, 1326, 1428, 1581, 1683, 1785};

struct SetOverrideArgs {
  u32 module;
  u32 hz;
};
static_assert(sizeof(SetOverrideArgs) == 8, "sys-clk SetOverride args");

Service g_srv;
bool g_active = false;  // an override of ours is in place

bool g_open = false;  // session to sys-clk open (and applet hook installed)
u32 g_hz = 0;
AppletHookCookie g_hook;
bool g_exit_locked = false;  // m6t: appletLockExit() while our override may be in place
bool g_exiting = false;
Thread g_exit_guard;

// m6t: once the exit was requested the game must leave quickly (the console
// shows "closing..."): whatever happens, unlock the exit after 3 s so the
// system can terminate us, and end the process ourselves 3 s later.
void exit_guard_main(void *) {
  svcSleepThread(3'000'000'000LL);
  appletUnlockExit();
  svcSleepThread(3'000'000'000LL);
  svcExitProcess();
}

void unlock_exit() {
  if (!g_exit_locked) return;
  g_exit_locked = false;
  appletUnlockExit();
}

void set_override(bool on, const char *why) {
  const SetOverrideArgs args = {kModuleCpu, on ? g_hz : 0u};
  const Result rc = serviceDispatchIn(&g_srv, kCmdSetOverride, args);
  if (R_SUCCEEDED(rc)) g_active = on;
  std::printf("[NX] sys-clk: %s (%s)%s\n", on ? "pedido" : "quitado", why,
              R_SUCCEEDED(rc) ? "" : " fallo");
  c38_sink_flush();  // the process may be killed while suspended: get the line to the SD now
}

void on_applet(AppletHookType type, void *) {
  if (!g_open) return;
  if (type == AppletHookType_OnFocusState || type == AppletHookType_OnResume) {
    const bool in_focus = appletGetFocusState() == AppletFocusState_InFocus;
    if (in_focus && !g_active && !g_exiting) set_override(true, "foco recuperado");
    else if (!in_focus && g_active) set_override(false, "foco perdido");
  } else if (type == AppletHookType_OnExitRequest && !g_exiting) {
    // m6t: HOME -> close, delivered because the exit is locked. appletMainLoop()
    // returns false right after this hook, so the main loop quits and goes
    // through shutdown() (sysclk_stop unlocks the exit); the guard thread
    // covers a slow or stuck exit path.
    g_exiting = true;
    if (g_active) set_override(false, "salida pedida");
    if (g_exit_locked &&
        (R_FAILED(threadCreate(&g_exit_guard, exit_guard_main, nullptr, nullptr, 16 * 1024, 0x2C, -2)) ||
         R_FAILED(threadStart(&g_exit_guard))))
      unlock_exit();  // no guard: let the system close us right away
  }
}

bool has_service(const char *name, bool *out) {
  u8 tmp = 0;
  const SmServiceName n = smEncodeName(name);
  Result rc = tipcDispatchInOut(smGetServiceSessionTipc(), 65100, n, tmp);
  if (R_FAILED(rc)) return false;
  *out = (tmp & 1) != 0;
  return true;
}

}  // namespace

void sysclk_start() {
  if (const char *off = std::getenv("NWII_NX_SYSCLK_OFF"); off && *off && *off != '0') {
    std::printf("[NX] sys-clk: apagado (NWII_NX_SYSCLK_OFF)\n");
    return;
  }
  int mhz = 1224;
  if (const char *e = std::getenv("NWII_NX_CPU_MHZ"); e && *e) mhz = std::atoi(e);
  if (mhz == 0) {
    std::printf("[NX] sys-clk: cpu_mhz = 0, no se pide nada\n");
    return;
  }
  bool ok_mhz = false;
  for (int m : kAllowedMhz) ok_mhz |= m == mhz;
  if (!ok_mhz) {
    std::printf("[NX] sys-clk: cpu_mhz = %d no es de la lista, no se pide nada\n", mhz);
    return;
  }

  if (R_FAILED(smInitialize())) {
    std::printf("[NX] sys-clk: sin sm, no se pide nada\n");
    return;
  }
  const Backend *used = nullptr;
  u32 api = 0;
  bool asked = false;
  for (const Backend &b : kBackends) {
    bool there = false;
    if (!has_service(b.name, &there)) {
      std::printf("[NX] sys-clk: sm no responde a HasService, no se pide nada\n");
      break;
    }
    if (!there) continue;
    asked = true;
    if (R_FAILED(smGetService(&g_srv, b.name))) {
      std::printf("[NX] sys-clk: %s no abre, no se pide nada\n", b.name);
      break;
    }
    if (R_FAILED(serviceDispatchOut(&g_srv, kCmdGetApiVersion, api)) || api < b.api_min ||
        api > b.api_max) {
      std::printf("[NX] sys-clk: %s API %u desconocida, no se pide nada\n", b.name, (unsigned)api);
      serviceClose(&g_srv);
      break;
    }
    used = &b;
    break;
  }
  smExit();
  if (!used) {
    if (!asked) std::printf("[NX] sys-clk: no instalado\n");
    return;
  }

  const SetOverrideArgs args = {kModuleCpu, (u32)mhz * 1000000u};
  const Result rc = serviceDispatchIn(&g_srv, kCmdSetOverride, args);
  if (R_FAILED(rc)) {
    std::printf("[NX] sys-clk: %s API %u rechaza el override (0x%x)\n", used->name, (unsigned)api,
                (unsigned)rc);
    serviceClose(&g_srv);
    return;
  }
  g_active = true;
  g_open = true;
  g_hz = (u32)mhz * 1000000u;
  // m6s: HOME -> "close software" kills the process without shutdown()/atexit
  // (seen on hardware with m6r), so give the clock back as soon as the game
  // loses focus (HOME, sleep, an applet on top) and ask again when it is back.
  // libnx calls the hook from appletMainLoop() on the main thread.
  appletHook(&g_hook, on_applet, nullptr);
  // m6s on hardware: the focus hook never ran before HOME -> close killed the
  // process. m6t: lock the exit so that close arrives as ExitRequested and the
  // game leaves through shutdown() with the override removed.
  g_exit_locked = R_SUCCEEDED(appletLockExit());
  static bool registered = false;
  if (!registered) {
    registered = true;
    std::atexit(sysclk_stop);
  }
  std::printf("[NX] sys-clk: CPU %d MHz pedidos (%s API %u); GPU sin tocar\n", mhz, used->name,
              (unsigned)api);
}

void sysclk_stop() {
  if (!g_open) return;
  g_open = false;
  appletUnhook(&g_hook);
  if (g_active) set_override(false, "salida");
  serviceClose(&g_srv);
  c38_sink_flush();
  unlock_exit();
}

}  // namespace nwii::runtime::nx
#endif  // __SWITCH__
