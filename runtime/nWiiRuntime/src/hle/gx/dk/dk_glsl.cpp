#include "dk_glsl.h"
#include <cstring>
#include <sstream>

namespace dkglsl {

int VaryingLocation(const std::string& n) {
    static const char* const kNames[] = {
        "vColor0", "vColor1", "vTex0", "vTex1", "vTex2", "vTex3", "vTex4", "vTex5", "vTex6", "vTex7",
        // GPU XF (comprobación) y autoprueba
        "xP", "xN", "xT01", "xT23", "xC", "oDiv", "oSqrt", "oXf",
        // pruebas (dk3test) y shaders auxiliares
        "vUV", "vCol", "vId", "oCase", "oB"};
    for (size_t i = 0; i < sizeof(kNames) / sizeof(kNames[0]); ++i)
        if (n == kNames[i]) return (int)i;
    return -1;
}

namespace {

struct TypeInfo { const char* name; BaseType base; uint8_t comps, cols; };
const TypeInfo kTypes[] = {
    {"float", kFloat, 1, 1}, {"vec2", kFloat, 2, 1}, {"vec3", kFloat, 3, 1}, {"vec4", kFloat, 4, 1},
    {"int", kInt, 1, 1},     {"ivec2", kInt, 2, 1},  {"ivec3", kInt, 3, 1},  {"ivec4", kInt, 4, 1},
    {"uint", kUint, 1, 1},   {"uvec2", kUint, 2, 1}, {"uvec3", kUint, 3, 1}, {"uvec4", kUint, 4, 1},
    {"bool", kBool, 1, 1},
    {"mat2", kFloat, 2, 2},  {"mat3", kFloat, 3, 3}, {"mat4", kFloat, 4, 4},
};

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> w;
    std::string cur;
    for (char c : s) {
        if (c == ' ' || c == '\t' || c == ';') {
            if (!cur.empty()) w.push_back(cur), cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) w.push_back(cur);
    return w;
}

uint32_t Align(uint32_t x, uint32_t a) { return (x + a - 1) & ~(a - 1); }

}  // namespace

std::string Convert(const std::string& src, Stage stage, Reflection& r) {
    r = Reflection();
    std::istringstream in(src);
    std::ostringstream out, block;
    std::string line;
    bool first = true;
    uint32_t off = 0;
    uint8_t next_binding = 0;
    bool block_written = false;
    std::vector<std::string> body;
    std::string header;
    std::vector<std::string> pending;  // líneas generadas al partir declaraciones múltiples
    while (!pending.empty() || std::getline(in, line)) {
        if (!pending.empty()) { line = pending.front(); pending.erase(pending.begin()); }
        // sin comentario de final de línea (las declaraciones se reconocen por su texto)
        const size_t cpos = line.find("//");
        const std::string t = Trim(cpos == std::string::npos ? line : line.substr(0, cpos));
        if (first && t.compare(0, 9, "#version ") == 0) {
            header = "#version 460\n";
            first = false;
            continue;
        }
        first = false;
        // extensiones que en 4.60 son núcleo (gpu_shader5: precise/fma; separate_shader_objects: el
        // renderer en modo SSO las pide)
        if (t.compare(0, 10, "#extension") == 0 && (t.find("GL_ARB_gpu_shader5") != std::string::npos ||
                                                   t.find("GL_ARB_separate_shader_objects") != std::string::npos))
            continue;
        if (t.compare(0, 8, "uniform ") == 0 && t.back() == ';' && t.find(',') != std::string::npos &&
            t.find('{') == std::string::npos) {
            // "uniform T a, b, c;" -> una línea por nombre (se vuelve a pasar por aquí)
            auto w = Words(t);
            std::string rest;
            for (size_t i = 2; i < w.size(); ++i) rest += w[i];
            std::vector<std::string> names;
            size_t a0 = 0;
            for (;;) {
                const size_t c = rest.find(',', a0);
                names.push_back(rest.substr(a0, c == std::string::npos ? std::string::npos : c - a0));
                if (c == std::string::npos) break;
                a0 = c + 1;
            }
            // reinyectar: se procesan como si fueran las líneas siguientes
            pending.insert(pending.begin(), names.size(), std::string());
            for (size_t i = 0; i < names.size(); ++i) pending[i] = "uniform " + w[1] + " " + names[i] + ";";
            continue;
        }
        if (t.compare(0, 8, "uniform ") == 0 && t.back() == ';') {
            auto w = Words(t);  // uniform T name[N]
            if (w.size() != 3) { r.error += "uniform raro: " + t + "\n"; body.push_back(line); continue; }
            const std::string& ty = w[1];
            std::string name = w[2];
            uint16_t count = 1;
            const size_t br = name.find('[');
            bool arr = false;
            if (br != std::string::npos) {
                count = (uint16_t)std::atoi(name.c_str() + br + 1);
                name = name.substr(0, br);
                arr = true;
            }
            if (ty.find("sampler") != std::string::npos) {
                Sampler s;
                s.name = name;
                s.binding = next_binding++;
                s.buffer = ty.find("Buffer") != std::string::npos;
                r.samplers.push_back(s);
                body.push_back("layout(binding = " + std::to_string(s.binding) + ") uniform " + ty + " " + w[2] + ";");
                continue;
            }
            const TypeInfo* ti = nullptr;
            for (const auto& k : kTypes)
                if (ty == k.name) ti = &k;
            if (!ti) { r.error += "tipo de uniforme no soportado: " + t + "\n"; continue; }
            Uniform u;
            u.name = name;
            u.base = ti->base;
            u.comps = ti->comps;
            u.cols = ti->cols;
            u.count = count;
            // std140: escalares 4, vec2 8, vec3/vec4 16; arrays y columnas de matriz con paso múltiplo de 16
            uint32_t base_align = ti->comps == 1 ? 4 : ti->comps == 2 ? 8 : 16;
            uint32_t size = ti->comps * 4;
            if (ti->cols > 1) { base_align = 16; u.col_stride = 16; size = 16 * ti->cols; }
            if (arr) { base_align = 16; u.stride = Align(size, 16); size = u.stride * count; }
            else u.stride = Align(size, 16);
            off = Align(off, base_align);
            u.offset = off;
            off += size;
            r.uniforms.push_back(u);
            block << "    " << (ty == "bool" ? std::string("bool") : ty) << " " << w[2] << ";\n";
            if (!block_written) { body.push_back("@BLOCK@"); block_written = true; }
            continue;
        }
        // varyings sin layout (solo declaraciones globales: empiezan por in/out/flat)
        if ((t.compare(0, 3, "in ") == 0 || t.compare(0, 4, "out ") == 0 || t.compare(0, 5, "flat ") == 0) &&
            t.back() == ';' && t.find('(') == std::string::npos && t.find('{') == std::string::npos) {
            auto w = Words(t);
            const std::string& name = w.back();
            const bool is_in = (w[0] == "in") || (w.size() > 1 && w[1] == "in");
            if (stage == Vertex && is_in) { r.error += "atributo sin location: " + t + "\n"; body.push_back(line); continue; }
            int loc;
            if (stage == Fragment && !is_in) loc = 0;  // única salida de color sin layout (FragColor, o...)
            else loc = VaryingLocation(name);
            if (loc < 0) { r.error += "varying sin location fija: " + t + "\n"; body.push_back(line); continue; }
            body.push_back("layout(location = " + std::to_string(loc) + ") " + t);
            continue;
        }
        body.push_back(line);
    }
    r.ubo_size = Align(off, 16);
    out << header;
    if (header.empty()) r.error += "sin #version\n";
    for (const auto& b : body) {
        if (b == "@BLOCK@") {
            out << "layout(std140, binding = 0) uniform DkUniforms {\n" << block.str() << "};\n";
            continue;
        }
        out << b << "\n";
    }
    return out.str();
}

}  // namespace dkglsl
