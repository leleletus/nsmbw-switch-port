#pragma once
// r15dk (deko3d): GLSL del renderer (estilo GL 3.3, uniformes sueltos) -> GLSL que acepta uam.
// uam no enlaza programas ni acepta uniformes fuera de bloques: aquí
//   - "#version 330 core" -> "#version 460"; se quita "#extension GL_ARB_gpu_shader5" (núcleo en 4.0+);
//   - cada "uniform T nombre[N];" que no es sampler pasa a un bloque std140 (binding 0 de la etapa) y se
//     anota su desplazamiento std140 (Reflection::uniforms) para emular glUniform* sobre una copia en CPU;
//   - cada sampler recibe "layout(binding = k)" (k en orden de declaración, por etapa);
//   - los varyings sin layout reciben "layout(location = L)" de una tabla fija por nombre (igual en VS y FS,
//     porque uam no enlaza), y la salida "out vec4 FragColor" la location 0.
// No depende de deko3d: se usa en la NRO (compilación con uamlib en la consola) y en el PC (precompilación).
#include <cstdint>
#include <string>
#include <vector>

namespace dkglsl {

enum Stage { Vertex = 0, Fragment = 4 };  // = uamlib::Stage

enum BaseType : uint8_t { kFloat, kInt, kUint, kBool };

struct Uniform {
    std::string name;
    BaseType base = kFloat;
    uint8_t comps = 1;     // componentes por columna (1-4)
    uint8_t cols = 1;      // columnas (matrices)
    uint16_t count = 1;    // elementos del array (1 si no es array)
    uint32_t offset = 0;   // desplazamiento std140 en el bloque
    uint32_t stride = 0;   // paso std140 entre elementos del array
    uint32_t col_stride = 16;
};

struct Sampler {
    std::string name;
    uint8_t binding = 0;
    bool buffer = false;  // usamplerBuffer / samplerBuffer
};

struct Reflection {
    std::vector<Uniform> uniforms;
    std::vector<Sampler> samplers;
    uint32_t ubo_size = 0;  // tamaño del bloque (múltiplo de 16; 0 = sin bloque)
    std::string error;      // vacío si todo fue bien
};

// Devuelve el GLSL convertido; si algo no se reconoce, r.error dice qué (y el texto puede no compilar).
std::string Convert(const std::string& src, Stage stage, Reflection& r);

// Location fija de un varying por nombre (-1 si no está en la tabla).
int VaryingLocation(const std::string& name);

// Identificador del compilador para las claves de la caché DKSH (cambiarlo si cambian uam, sus parches o
// esta conversión: así no se usan DKSH viejos). Clave = FNV-64(GLSL convertido) sembrado con FNV-64 de esto.
constexpr const char* kCompilerId = "uam 1.1.0 r15dk p1-4 / dk_glsl 2";
inline uint64_t Fnv64(const void* p, size_t n, uint64_t h = 14695981039346656037ull) {
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
inline uint64_t CacheKey(const std::string& converted) {
    const std::string id = kCompilerId;
    return Fnv64(converted.data(), converted.size(), Fnv64(id.data(), id.size()));
}

}  // namespace dkglsl
