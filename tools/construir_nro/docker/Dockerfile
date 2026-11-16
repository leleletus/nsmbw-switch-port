# construir_nro: imagen oficial de devkitPro para Switch (devkitA64 + libnx + portlibs) con lo que la herramienta
# necesita además: python3, git, cmake, ninja y un compilador de C++ para el PC (el recompilador corre en el PC).
FROM devkitpro/devkita64:latest
RUN apt-get update && apt-get install -y --no-install-recommends python3 git cmake ninja-build g++ xz-utils \
      ca-certificates && rm -rf /var/lib/apt/lists/* \
 && (dkp-pacman -S --noconfirm --needed switch-sdl2 switch-mesa switch-glm deko3d || true)
ENV DEVKITPRO=/opt/devkitpro
WORKDIR /trabajo
