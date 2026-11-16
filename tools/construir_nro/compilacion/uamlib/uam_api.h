#pragma once
// uamlib (r15dk): GLSL -> DKSH (los mismos bytes que escribe la herramienta uam con estos parches).
// Un solo hilo a la vez (c11/threads.h de uam es un stub): llamar siempre desde el mismo hilo de compilación,
// con pila grande (Mesa y nv50_ir recursan mucho: 1-8 MB).
#include <cstdint>
#include <string>
#include <vector>
namespace uamlib {
enum Stage { Vertex = 0, TessCtrl, TessEval, Geometry, Fragment, Compute };
// devuelve true y el .dksh entero en 'out'; si falla, false (y lo que uam escribió en stderr)
bool Compile(Stage stage, const char* glsl, std::vector<uint8_t>& out);
}
