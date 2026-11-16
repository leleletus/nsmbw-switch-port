#!/usr/bin/env python3
"""nav.py hbmenu|ftpd|donde : safe navigation on the user's Switch, by RECOGNIZING tiles instead of blind presses.

The Switch main menu reorders its tiles by recent use (hbmenu forwarder, FTPD forwarder, games...), so "A on the
first tile" can launch anything (9-nov: it launched Flappy Monster, which self-updated). This tool:
  hbmenu : from any screen (game, error dialog, console app, main menu) closes what runs, finds the hbmenu tile on the
           main menu by template matching (tools/switchctl/plantillas/home_hbmenu.png), moves to it and opens it.
  ftpd   : hbmenu, then checks that the 6th hbmenu item IS "ftpd classic" (fixed position inside hbmenu) and opens it.
  donde  : prints the classification of the current screen and, on the main menu, the tiles it recognizes.
Never presses A on something it did not recognize: exits with code 1 instead.
"""
import os
import socket
import subprocess
import sys
import time

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
IP = os.environ.get("SWITCH_IP", "<IP_SWITCH>")
TMP = os.environ.get("NAV_TMP", "/tmp/claude-1000/nav_shot.jpg")
sys.path.insert(0, HERE)
import estado  # noqa: E402
from switchctl import Bot  # noqa: E402

HOME_SLOTS = [(105 + 270 * k, 194, 105 + 270 * k + 256, 450) for k in range(4)]
HB_SLOTS = [(25 + 170 * k, 472, 25 + 170 * k + 148, 650) for k in range(7)]


def tpl(name):
    return Image.open(os.path.join(HERE, "plantillas", name)).convert("L").resize((32, 32))


def mad(a, b):
    pa, pb = a.getdata(), b.getdata()
    return sum(abs(x - y) for x, y in zip(pa, pb)) / len(pa)


def shot():
    os.makedirs(os.path.dirname(TMP), exist_ok=True)
    b = Bot(IP)
    try:
        data = b.ask("pixelPeek")
    finally:
        b.close()
    open(TMP, "wb").write(bytes.fromhex(data.decode()))
    return TMP


def press(*buttons, wait=0.0):
    b = Bot(IP)
    try:
        for x in buttons:
            b.send(f"click {x}")
            time.sleep(0.3)
    finally:
        b.close()
    time.sleep(wait)


def classify():
    return estado.classify(shot())


def selected_home():
    """Index of the main-menu slot with the cyan selection border (left edge, mid height), or None."""
    im = Image.open(TMP).convert("RGB")
    for k, (x0, y0, x1, y1) in enumerate(HOME_SLOTS):
        r, g, b = im.getpixel(((x0 + x1) // 2, y0 - 6))   # top border of the highlighted tile
        if g > 170 and b > 170 and r < 140:
            return k
    return None


def find_home(name, thr=30.0):
    im = Image.open(TMP).convert("L")
    t = tpl(name)
    best = min(((mad(im.crop(s).resize((32, 32)), t), k) for k, s in enumerate(HOME_SLOTS)))
    return best[1] if best[0] < thr else None, best[0]


def to_hbmenu():
    for _ in range(10):
        s = classify()
        if s == "hbmenu":
            return True
        if s == "juego":
            press("HOME", wait=2)
            press("X", wait=1.5)
            press("A", wait=4)
        elif s == "home":
            # the main menu wraps around and reorders by use. Find the hbmenu tile among the visible ones, step to it,
            # and press A only once the SELECTED tile (cyan border) is recognized as hbmenu.
            done = False
            # 12-nov: after the user changes "grip/order" the sys-botbase virtual controller is silently dropped and
            # presses do nothing; detaching makes the next click re-attach it (harmless on the main menu).
            b = Bot(IP)
            try:
                b.send("detachController")
            finally:
                b.close()
            time.sleep(1.5)
            for _ in range(30):
                shot()
                im = Image.open(TMP).convert("L")
                t = tpl("home_hbmenu.png")
                diffs = [mad(im.crop(sl).resize((32, 32)), t) for sl in HOME_SLOTS]
                sel = selected_home()
                if sel is not None and diffs[sel] < 30:
                    press("A", wait=6)
                    done = True
                    break
                hits = [k for k, d in enumerate(diffs) if d < 30]
                if hits and sel is not None and hits[0] > sel:
                    press(*["DRIGHT"] * (hits[0] - sel), wait=1.2)
                else:
                    press("DRIGHT", wait=1.2)
            if not done:
                print("[nav] could not reach the hbmenu tile on the main menu; not pressing anything")
                return False
        elif s == "oscuro":
            # an error dialog (A closes it) or a console app (B exits); never A twice in a row on unknown screens
            press("PLUS", wait=4)   # 12-nov: ftpd classic left open exits with + (B does nothing there)
            if classify() == "oscuro":
                press("B", wait=3)
            if classify() == "oscuro":
                press("HOME", wait=3)
        else:
            press("HOME", wait=3)
    return classify() == "hbmenu"


def to_ftpd():
    if not to_hbmenu():
        return False
    # hbmenu restarts with the first item selected; ftpd classic is the 6th item (fixed position)
    shot()
    im = Image.open(TMP).convert("L")
    d = mad(im.crop(HB_SLOTS[5]).resize((32, 32)), tpl("hb_ftpd_classic.png"))
    if d > 30:
        print(f"[nav] 6th hbmenu item is not ftpd classic (diff {d:.1f}); not pressing anything")
        return False
    press(*["DRIGHT"] * 5, wait=1)
    press("A", wait=6)
    for _ in range(5):
        try:
            socket.create_connection((IP, 5000), timeout=3).close()
            return True
        except OSError:
            time.sleep(2)
    return False


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "donde"
    if cmd == "donde":
        s = classify()
        print(s)
        if s == "home":
            for n in ("home_hbmenu.png", "home_ftpd.png"):
                print(n, find_home(n))
        sys.exit(0)
    ok = to_hbmenu() if cmd == "hbmenu" else to_ftpd() if cmd == "ftpd" else False
    print(f"[nav] {cmd}: {'ok' if ok else 'FALLO'}")
    sys.exit(0 if ok else 1)
