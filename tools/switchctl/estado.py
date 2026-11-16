#!/usr/bin/env python3
"""estado.py <shot.jpg>: what the console shows, from a sys-botbase screenshot (1280x720).

Prints one word:
  juego   the NSMBW port is running (its fps overlay: dark box top-left with the blue CPU bar / orange bar)
  hbmenu  homebrew menu (teal band at the bottom-left, dark grey top)
  home    HOME menu (flat grey 45,45,45)
  oscuro  anything dark without the overlay: system error dialog ("Se ha cerrado el programa..."),
          error applet, a console app such as 90DNS, a black screen
  otro    anything else
"""
import sys

from PIL import Image


def near(c, ref, tol):
    return all(abs(a - b) <= tol for a, b in zip(c, ref))


def classify(path):
    im = Image.open(path).convert("RGB")
    px = im.getpixel
    # 15-nov: con la barra de Status Monitor arriba (y < 36) los puntos altos quedan tapados: vale tambien que la
    # franja del recuadro por debajo de la barra (x 20-190, y 40-66) sea oscura en su mayor parte
    pts = [(x, y) for x in range(20, 191, 10) for y in (40, 46, 52, 58, 64)]
    box_dark = (all(max(px(p)) < 40 for p in [(150, 20), (180, 30), (120, 12)])
                or sum(max(px(p)) < 60 for p in pts) >= 0.7 * len(pts))
    blue = any(px((x, 104))[2] > 190 and px((x, 104))[0] < 130 for x in range(12, 18))
    orange = any(px((x, 112))[0] > 190 and 110 < px((x, 112))[1] < 190 and px((x, 112))[2] < 110 for x in range(12, 18))
    if box_dark and (blue or orange):
        return "juego"
    if px((12, 560))[1] > 150 and px((12, 560))[2] > 140 and px((12, 560))[0] < 130 and (near(px((640, 30)), (44, 45, 49), 12) or near(px((640, 60)), (44, 45, 49), 12)):
        return "hbmenu"
    if (near(px((640, 30)), (45, 45, 45), 8) or near(px((640, 60)), (45, 45, 45), 8)) and near(px((12, 560)), (45, 45, 45), 8):
        return "home"
    lum = [sum(px((x, y))) / 3 for x in range(40, 1240, 80) for y in range(40, 660, 80)]
    if sum(lum) / len(lum) < 60:
        return "oscuro"
    return "otro"


if __name__ == "__main__":
    print(classify(sys.argv[1]))
