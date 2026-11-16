#include "uam_api.h"
#include "compiler_iface.h"
namespace uamlib {
static void* Grow(void* user, size_t size) {
    auto* v = static_cast<std::vector<uint8_t>*>(user);
    v->resize(size);
    return v->data();
}
bool Compile(Stage stage, const char* glsl, std::vector<uint8_t>& out) {
    out.clear();
    DekoCompiler c{(pipeline_stage)stage};
    if (!c.CompileGlsl(glsl)) return false;
    c.OutputDkshMem(&Grow, &out);
    return !out.empty();
}
}
