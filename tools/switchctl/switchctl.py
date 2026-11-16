#!/usr/bin/env python3
"""switchctl: control the user's Switch from the PC for automated tests (NSMBW port).

Uses sys-botbase (Atmosphère sysmodule 430000000000000B, TCP port 6000 on the console's Wi-Fi IP).
Commands are text lines ending in \\r\\n, e.g. "click A", "press HOME", "pixelPeek" (JPEG screenshot as hex).

    switchctl.py [--ip <IP_SWITCH>] ping                     # check the connection
    switchctl.py click A [B X Y L R ZL ZR PLUS MINUS HOME CAPTURE LSTICK RSTICK DUP DDOWN DLEFT DRIGHT]
    switchctl.py seq "A,W500,B"                                  # clickSeq: buttons and waits (W<ms>)
    switchctl.py hold HOME 1500                                  # press, wait ms, release
    switchctl.py stick LEFT 0 32767 800                          # stick x y for ms, then centre
    switchctl.py shot out.jpg                                    # screenshot (pixelPeek)
    switchctl.py screen on|off                                   # backlight (keeps the console awake while off)
    switchctl.py raw "<command>"                                 # any sys-botbase command

nxlink (send an NRO to hbmenu's netloader, after pressing Y in hbmenu, and stream its stdout):
    /opt/devkitpro/tools/bin/nxlink -a <IP_SWITCH> -s <file.nro>
"""
import argparse
import socket
import sys
import time

DEFAULT_IP = "<IP_SWITCH>"
PORT = 6000


class Bot:
    def __init__(self, ip, timeout=10.0):
        self.s = socket.create_connection((ip, PORT), timeout=timeout)
        self.s.settimeout(timeout)

    def send(self, cmd):
        self.s.sendall((cmd + "\r\n").encode())

    def recv_line(self):
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = self.s.recv(1 << 20)
            if not chunk:
                break
            buf += chunk
        return buf.strip()

    def ask(self, cmd):
        self.send(cmd)
        return self.recv_line()

    def close(self):
        self.s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ip", default=DEFAULT_IP)
    ap.add_argument("cmd")
    ap.add_argument("args", nargs="*")
    a = ap.parse_args()
    bot = Bot(a.ip)
    try:
        if a.cmd == "ping":
            print("sys-botbase", bot.ask("getVersion").decode(errors="replace"))
        elif a.cmd == "click":
            for b in a.args:
                bot.send(f"click {b}")
                time.sleep(0.25)
        elif a.cmd == "seq":
            bot.send(f"clickSeq {a.args[0]}")
        elif a.cmd == "hold":
            bot.send(f"press {a.args[0]}")
            time.sleep(int(a.args[1]) / 1000 if len(a.args) > 1 else 1.0)
            bot.send(f"release {a.args[0]}")
        elif a.cmd == "stick":
            side, x, y = a.args[0], a.args[1], a.args[2]
            bot.send(f"setStick {side} {x} {y}")
            time.sleep(int(a.args[3]) / 1000 if len(a.args) > 3 else 0.5)
            bot.send(f"setStick {side} 0 0")
        elif a.cmd == "shot":
            data = bot.ask("pixelPeek")
            out = a.args[0] if a.args else "shot.jpg"
            open(out, "wb").write(bytes.fromhex(data.decode()))
            print(out)
        elif a.cmd == "screen":
            bot.send("screenOn" if a.args and a.args[0] == "on" else "screenOff")
        elif a.cmd == "raw":
            cmd = " ".join(a.args)
            bot.send(cmd)
            if cmd.split()[0] in ("getVersion", "pixelPeek", "getTitleID", "isProgramRunning", "charge", "info"):
                print(bot.recv_line().decode(errors="replace"))
        else:
            sys.exit(f"unknown command {a.cmd}")
    finally:
        bot.close()


if __name__ == "__main__":
    main()
