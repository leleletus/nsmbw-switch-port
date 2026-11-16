// PC: convierte con dk_glsl los shaders del renderer (VS clásico de un estado TEV vacío, VS de GPU XF 0/1,
// autoprueba XF, y los FS de un directorio) y los compila con uamlib. Uso: dkglsl_test <dir_fs> <dir_salida>
#include "uam_api.h"
#include "../nWiiRuntime/src/hle/gx/dk/dk_glsl.h"
#include "runtime/gx/tev_shader_gen.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
using namespace nwii::runtime::gx;
static int ok = 0, bad = 0;
// = sso::Patch de renderer_gl.cpp (modo separable de la Switch)
static std::string Patch(const std::string &src, bool vs) {
  std::string s = src;
  const size_t nl = s.find('\n');
  if (nl == std::string::npos) return s;
  std::string ins = "#extension GL_ARB_separate_shader_objects : require\n";
  if (vs) ins += "out gl_PerVertex { vec4 gl_Position; };\n";
  size_t at0 = nl + 1;
  while (at0 < s.size() && s[at0] == '#') {
    const size_t e = s.find('\n', at0);
    if (e == std::string::npos) break;
    at0 = e + 1;
  }
  s.insert(at0, ins);
  static const char *const kVar[10] = {"vec4 vColor0", "vec4 vColor1", "vec2 vTex0", "vec2 vTex1", "vec2 vTex2",
                                       "vec2 vTex3", "vec2 vTex4", "vec2 vTex5", "vec2 vTex6", "vec2 vTex7"};
  for (int i = 0; i < 10; ++i) {
    const std::string decl = std::string("\n") + (vs ? "out " : "in ") + kVar[i] + ";\n";
    const size_t at = s.find(decl);
    if (at == std::string::npos) continue;
    s.insert(at + 1, "layout(location = " + std::to_string(i) + ") ");
  }
  return s;
}
static double total_ms = 0;
static void Do(const std::string& name, const std::string& src, dkglsl::Stage st, const std::string& outdir) {
    dkglsl::Reflection r;
    std::ofstream(outdir + "/" + name + ".gl.glsl") << src;
    std::string g = dkglsl::Convert(src, st, r);
    std::ofstream(outdir + "/" + name + ".dk.glsl") << g;
    std::vector<uint8_t> d;
    auto t0 = std::chrono::steady_clock::now();
    bool c = uamlib::Compile((uamlib::Stage)st, g.c_str(), d);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    total_ms += ms;
    if (!r.error.empty()) printf("[%s] conversión: %s", name.c_str(), r.error.c_str());
    if (!c) { ++bad; printf("[%s] FALLA uam\n", name.c_str()); return; }
    ++ok;
    std::ofstream(outdir + "/" + name + ".dksh", std::ios::binary).write((const char*)d.data(), d.size());
    // DKCACHE=<fichero>: semilla de la caché DKSH de la consola (dkshcache.bin) con lo que usa el modo SSO
    static FILE* cache = [] {
        const char* p = getenv("DKCACHE");
        FILE* f = p ? fopen(p, "wb") : nullptr;
        if (f) fwrite("DKS1", 1, 4, f);
        return f;
    }();
    // vs_xf0/1 sin parchear: xf::Init los compila una vez como shader suelto (CompileVs) antes del SSO
    if (cache && (name.compare(0, 4, "sso_") == 0 || name.compare(0, 5, "copy_") == 0 || name == "vs_xf0" || name == "vs_xf1")) {
        const uint64_t key = dkglsl::CacheKey(g);
        const uint32_t len = (uint32_t)d.size();
        fwrite(&key, 8, 1, cache); fwrite(&len, 4, 1, cache); fwrite(d.data(), 1, len, cache); fflush(cache);
    }
    if (name[0] != 'f' || ok < 3) printf("[%s] ok %zu B, ubo %u B, %zu uniformes, %zu samplers, %.1f ms\n", name.c_str(), d.size(), r.ubo_size, r.uniforms.size(), r.samplers.size(), ms);
}
int main(int argc, char** argv) {
    if (argc < 3) return 2;
    std::string outdir = argv[2];
    GXState st{};
    auto gs = GenerateTEVShader(st, 0x90);
    Do("vs_classic", gs.vertex_source, dkglsl::Vertex, outdir);
    Do("fs_empty_state", gs.fragment_source, dkglsl::Fragment, outdir);
    Do("vs_xf0", GenerateVertexShaderXF(0, false), dkglsl::Vertex, outdir);
    Do("vs_xf1", GenerateVertexShaderXF(1, false), dkglsl::Vertex, outdir);
    Do("vs_xfself0", GenerateXfSelfTestShader(0), dkglsl::Vertex, outdir);
    {
        // = InitCopyShader de renderer_gl.cpp (copia EFB -> textura)
        const std::string rg = [] { std::ifstream f("/home/djvemo/nsmbw-port/r9/r15dk/nWiiRuntime/src/hle/gx/renderer_gl.cpp"); std::stringstream ss; ss << f.rdbuf(); return ss.str(); }();
        auto lit = [&](size_t from) {  // concatena los literales "..." de un bloque static const char *x = ...;
            std::string out; size_t end = rg.find(";\n", from), p = from;
            while ((p = rg.find('"', p)) != std::string::npos && p < end) {
                size_t q = p + 1; std::string piece;
                while (rg[q] != '"') { if (rg[q] == '\\') { char c = rg[q + 1]; piece += c == 'n' ? '\n' : c; q += 2; } else piece += rg[q++]; }
                out += piece; p = q + 1;
            }
            return out;
        };
        const size_t ic = rg.find("static void InitCopyShader()");
        const size_t vs = rg.find("static const char *vs =", ic), fs = rg.find("static const char *fs =", ic);
        Do("copy_vs", lit(vs), dkglsl::Vertex, outdir);
        Do("copy_fs", lit(fs), dkglsl::Fragment, outdir);
    }
    Do("sso_vs_xf0", Patch(GenerateVertexShaderXF(0, false), true), dkglsl::Vertex, outdir);
    Do("sso_vs_xf1", Patch(GenerateVertexShaderXF(1, false), true), dkglsl::Vertex, outdir);
    Do("sso_vs_classic", Patch(gs.vertex_source, true), dkglsl::Vertex, outdir);
    Do("sso_fs_empty", Patch(gs.fragment_source, false), dkglsl::Fragment, outdir);
    DIR* d = opendir(argv[1]);
    while (dirent* e = d ? readdir(d) : nullptr) {
        std::string n = e->d_name;
        if (n.size() < 6 || n.substr(n.size() - 5) != ".glsl") continue;
        std::ifstream f(std::string(argv[1]) + "/" + n);
        std::stringstream ss; ss << f.rdbuf();
        Do(n.substr(0, n.size() - 5), ss.str(), dkglsl::Fragment, outdir);
        if (getenv("SSO")) Do("sso_" + n.substr(0, n.size() - 5), Patch(ss.str(), false), dkglsl::Fragment, outdir);
    }
    printf("compilados %d, fallos %d, %.0f ms en total (%.1f ms de media)\n", ok, bad, total_ms, total_ms / (ok + bad));
    return bad ? 1 : 0;
}
