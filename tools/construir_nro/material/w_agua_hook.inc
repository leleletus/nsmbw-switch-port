// [r44w] frente W: instalación, estadística y modo comprobación del renderizador nativo del agua.
// Se incluye al final de w_agua_gen.cpp (misma unidad que el código generado transformado).
//
// NWII_W_AGUA="1"        engancha 0x8000D000 y los dos thunks del REL (0x807B6E50 / 0x807B6E60, `b 8000D000`) en
//                        g_disp_table: drawProc (80164FA0) llega por nwii_dispatch y bctrl por la tabla hash de g7,
//                        que se monta después (solo con las entradas que siguen apuntando a su función generada).
// NWII_W_AGUA_CHECK="1"  (PC; implica el gancho) en cada llamada: ejecuta la función generada ORIGINAL con el pipe
//                        capturado, deshace lo que escribió en la RAM y los registros, ejecuta la NATIVA igual y
//                        compara: escrituras al pipe (tipo y valor, en orden), registros/estado de la CPU y bytes de
//                        MEM1/MEM2/LC tocados por cualquiera de las dos. Luego repite al pipe las de la nativa.
//                        `[WAGUA] check` cada 5 s.
// NWII_W_AGUA_STATS="1"  sin gancho: solo cuenta llamadas/tipos/quads (para ver qué niveles pasan por aquí).
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

void w_agua_8000D000(CPUContext& ctx);
void w_agua_807B6E50(CPUContext& ctx);
void w_agua_807B6E60(CPUContext& ctx);
void func_8000D000(CPUContext& ctx);
void func_807B6E50(CPUContext& ctx);
void func_807B6E60(CPUContext& ctx);

namespace nwii { namespace runtime {
extern std::vector<uint8_t> *g_w_cap;
extern std::atomic<uint64_t> g_w_commits, g_w_commit_writes, g_w_commit_slow;
int GX_W_ModeSwap(int m);
} }

namespace {
constexpr uint32_t kWAddr[3] = {0x8000D000u, 0x807B6E50u, 0x807B6E60u};
nwii_fn s_w_orig[3] = {nullptr, nullptr, nullptr};
const nwii_fn s_w_nat[3] = {w_agua_8000D000, w_agua_807B6E50, w_agua_807B6E60};
bool s_w_native = false, s_w_check = false, s_w_all = false;
uint64_t s_w_fallback = 0, s_w_route[6] = {};
// estadística (solo el hilo del juego llama aquí)
uint64_t s_w_calls = 0, s_w_type[8] = {}, s_w_quads = 0, s_w_ck_calls = 0, s_w_ck_bad_pipe = 0, s_w_ck_bad_reg = 0,
         s_w_ck_bad_mem = 0, s_w_ck_writes = 0, s_w_ck_membytes = 0;
uint32_t s_w_this_last = 0, s_w_vt_last = 0;
std::chrono::steady_clock::time_point s_w_t0;

struct WSnap {
    std::array<uint32_t, 32> gpr;
    std::array<double, 32> fpr, ps1;
    std::array<ConditionField, 8> cr;
    uint32_t pc, lr, ctr, xer, msr, fpscr, srr0, srr1;
    std::array<uint32_t, 8> gqr;
    std::array<uint32_t, 4> sprg;
    uint64_t ic;
    uint32_t resv;
};
WSnap w_take(const CPUContext& c) {
    WSnap s;
    s.gpr = c.gpr; s.fpr = c.fpr; s.ps1 = c.ps1; s.cr = c.cr;
    s.pc = c.pc; s.lr = c.lr; s.ctr = c.ctr; s.xer = c.xer; s.msr = c.msr; s.fpscr = c.fpscr; s.srr0 = c.srr0;
    s.srr1 = c.srr1; s.gqr = c.gqr; s.sprg = c.sprg; s.ic = c.inst_count; s.resv = c.reservation_addr;
    return s;
}
void w_put(CPUContext& c, const WSnap& s) {
    c.gpr = s.gpr; c.fpr = s.fpr; c.ps1 = s.ps1; c.cr = s.cr;
    c.pc = s.pc; c.lr = s.lr; c.ctr = s.ctr; c.xer = s.xer; c.msr = s.msr; c.fpscr = s.fpscr; c.srr0 = s.srr0;
    c.srr1 = s.srr1; c.gqr = s.gqr; c.sprg = s.sprg; c.inst_count = s.ic; c.reservation_addr = s.resv;
}
// primera diferencia de registros (texto) o "" si iguales
std::string w_regdiff(const WSnap& a, const WSnap& b) {
    char buf[160];
    for (int i = 0; i < 32; ++i) if (a.gpr[i] != b.gpr[i]) { std::snprintf(buf, sizeof buf, "r%d %08x/%08x", i, a.gpr[i], b.gpr[i]); return buf; }
    for (int i = 0; i < 32; ++i) if (std::memcmp(&a.fpr[i], &b.fpr[i], 8)) { std::snprintf(buf, sizeof buf, "f%d %g/%g", i, a.fpr[i], b.fpr[i]); return buf; }
    for (int i = 0; i < 32; ++i) if (std::memcmp(&a.ps1[i], &b.ps1[i], 8)) { std::snprintf(buf, sizeof buf, "ps1_%d %g/%g", i, a.ps1[i], b.ps1[i]); return buf; }
    for (int i = 0; i < 8; ++i) if (std::memcmp(&a.cr[i], &b.cr[i], sizeof a.cr[i])) { std::snprintf(buf, sizeof buf, "cr%d", i); return buf; }
#define WD(f) if (a.f != b.f) { std::snprintf(buf, sizeof buf, #f " %llx/%llx", (unsigned long long)a.f, (unsigned long long)b.f); return buf; }
    WD(pc) WD(lr) WD(ctr) WD(xer) WD(msr) WD(fpscr) WD(srr0) WD(srr1) WD(ic) WD(resv)
#undef WD
    if (a.gqr != b.gqr) return "gqr";
    if (a.sprg != b.sprg) return "sprg";
    return "";
}

struct WRegion { uint8_t *p; size_t n; uint32_t base; std::vector<uint8_t> s0, sa; };
std::vector<WRegion> s_w_regs;

uint32_t w_stats(CPUContext& ctx) {
    ++s_w_calls;
    const uint32_t th = ctx.gpr[3];
    const uint32_t ty = ctx.mmu.read32(th + 0xCE94u);
    ++s_w_type[ty < 7 ? ty : 7];
    s_w_quads += ctx.mmu.read32(th + 0xCE90u);
    s_w_this_last = th;
    s_w_vt_last = ctx.mmu.read32(th);
    const auto now = std::chrono::steady_clock::now();
    if (now - s_w_t0 >= std::chrono::seconds(5)) {
        const double dt = std::chrono::duration<double>(now - s_w_t0).count();
        s_w_t0 = now;
        std::printf("[WAGUA] %s: llamadas %.1f/s (tipo 0:%llu 1:%llu 2:%llu 3:%llu 4:%llu otro:%llu), cuerpos/llamada %.1f, "
                    "entregas %.0f/s (%.0f escrituras/s, lentas %.0f/s), al generado por tipo %llu, this %08x vt %08x\n",
                    s_w_native ? "nativo" : "generado", s_w_calls / dt,
                    (unsigned long long)s_w_type[0], (unsigned long long)s_w_type[1], (unsigned long long)s_w_type[2],
                    (unsigned long long)s_w_type[3], (unsigned long long)s_w_type[4],
                    (unsigned long long)(s_w_type[5] + s_w_type[6] + s_w_type[7]),
                    s_w_calls ? double(s_w_quads) / s_w_calls : 0.0,
                    nwii::runtime::g_w_commits.exchange(0) / dt, nwii::runtime::g_w_commit_writes.exchange(0) / dt,
                    nwii::runtime::g_w_commit_slow.exchange(0) / dt, (unsigned long long)s_w_fallback, s_w_this_last, s_w_vt_last);
        if (s_w_check)
            std::printf("[WAGUA] check: %llu llamadas comprobadas, %llu escrituras al pipe, %llu bytes de RAM tocados; "
                        "distintas: pipe %llu, registros %llu, memoria %llu\n",
                        (unsigned long long)s_w_ck_calls, (unsigned long long)s_w_ck_writes,
                        (unsigned long long)s_w_ck_membytes, (unsigned long long)s_w_ck_bad_pipe,
                        (unsigned long long)s_w_ck_bad_reg, (unsigned long long)s_w_ck_bad_mem);
        std::printf("[WAGUA] rutas acumuladas: B530 %llu, BA70 %llu, C5C0 %llu, CA50 %llu, C020 %llu, ninguna %llu\n",
                    (unsigned long long)s_w_route[0], (unsigned long long)s_w_route[1], (unsigned long long)s_w_route[2],
                    (unsigned long long)s_w_route[3], (unsigned long long)s_w_route[4], (unsigned long long)s_w_route[5]);
        std::fflush(stdout);
        s_w_calls = s_w_quads = 0;
        for (auto& t : s_w_type) t = 0;
    }
    return ty;
}

void w_replay(const std::vector<uint8_t>& c) {
    size_t i = 0;
    auto rd = [&](int n) { uint64_t v = 0; for (int k = 0; k < n; ++k) v = v << 8 | c[i++]; return v; };
    while (i < c.size()) {
        const uint8_t k = c[i++];
        if (k == 1) nwii::runtime::GX_WGPIPE_Write8((uint8_t)rd(1));
        else if (k == 2) nwii::runtime::GX_WGPIPE_Write16((uint16_t)rd(2));
        else if (k == 3) nwii::runtime::GX_WGPIPE_Write32((uint32_t)rd(4));
        else if (k == 4) { union { uint32_t i; float f; } u; u.i = (uint32_t)rd(4); nwii::runtime::GX_WGPIPE_WriteF32(u.f); }
        else { union { uint64_t i; double d; } u; u.i = rd(8); nwii::runtime::GX_WGPIPE_WriteF64(u.d); }
    }
}

void w_check(CPUContext& ctx, int k) {
    static std::vector<uint8_t> capA, capB;
    ++s_w_ck_calls;
    // Durante las dos ejecuciones, MSR[EE] = 0: una interrupción entregada en un sondeo de la primera (la original)
    // no se repetiría en la segunda y se perdería (el juego se colgaba esperándola). Con EE = 0 las dos ven lo
    // mismo y las interrupciones pendientes se entregan en el primer sondeo de después, como si la llamada no
    // hubiera sondeado. (Solo en este modo de comprobación.)
    const uint32_t msr_ee = ctx.msr & 0x8000u;
    ctx.msr &= ~0x8000u;
    const WSnap s0 = w_take(ctx);
    const int32_t pl0 = ctx.poll_left.load(std::memory_order_relaxed);
    for (auto& r : s_w_regs) std::memcpy(r.s0.data(), r.p, r.n);
    const int om = nwii::runtime::GX_W_ModeSwap(0);
    capA.clear();
    nwii::runtime::g_w_cap = &capA;
    s_w_orig[k](ctx);
    const WSnap sa = w_take(ctx);
    // lo que tocó la original: guardar su resultado y volver al estado de antes SOLO en esos bytes
    // (así no se pisa lo que otros hilos escriban mientras tanto)
    std::vector<std::pair<WRegion*, size_t>> touched;
    for (auto& r : s_w_regs) {
        const uint64_t *a = (const uint64_t *)r.p, *b = (const uint64_t *)r.s0.data();
        for (size_t w = 0, nw = r.n / 8; w < nw; ++w)
            if (a[w] != b[w]) touched.push_back({&r, w});
    }
    for (auto& t : touched) {
        std::memcpy(t.first->sa.data() + t.second * 8, t.first->p + t.second * 8, 8);
        std::memcpy(t.first->p + t.second * 8, t.first->s0.data() + t.second * 8, 8);
    }
    w_put(ctx, s0);
    ctx.poll_left.store(pl0, std::memory_order_relaxed);
    capB.clear();
    nwii::runtime::g_w_cap = &capB;
    // NWII_W_AGUA_CHECK_NULL=1: control del propio método: la «nativa» es otra vez la generada (lo que salga
    // distinto ahí es ruido de la comprobación: otros hilos que escriben RAM del invitado mientras tanto)
    static const bool null_mode = std::getenv("NWII_W_AGUA_CHECK_NULL") != nullptr;
    (null_mode ? s_w_orig[k] : s_w_nat[k])(ctx);
    nwii::runtime::g_w_cap = nullptr;
    nwii::runtime::GX_W_ModeSwap(om);
    const WSnap sb = w_take(ctx);
    if (!(ctx.msr & 0x8000u)) ctx.msr |= msr_ee;
    s_w_ck_writes += capB.size() / 3;
    // comparar
    if (capA != capB) {
        if (s_w_ck_bad_pipe++ < 10) {
            size_t i = 0;
            while (i < capA.size() && i < capB.size() && capA[i] == capB[i]) ++i;
            std::printf("[WAGUA] check: PIPE distinto en la llamada %llu (%zu/%zu bytes de captura, primer byte distinto %zu)\n",
                        (unsigned long long)s_w_ck_calls, capA.size(), capB.size(), i);
        }
    }
    const std::string rd = w_regdiff(sa, sb);
    if (!rd.empty() && s_w_ck_bad_reg++ < 10)
        std::printf("[WAGUA] check: REGISTROS distintos en la llamada %llu: %s (original/nativa)\n",
                    (unsigned long long)s_w_ck_calls, rd.c_str());
    uint64_t bad = 0, nbytes = 0, badA = 0, badB = 0;
    uint32_t firstbad = 0, lastbad = 0;
    // palabras que tocó cualquiera de las dos: las de la original (touched) y las de la nativa (distintas de s0)
    for (auto& t : touched) {
        ++nbytes;
        if (std::memcmp(t.first->sa.data() + t.second * 8, t.first->p + t.second * 8, 8)) {
            if (!bad++) firstbad = t.first->base + (uint32_t)t.second * 8;
            lastbad = t.first->base + (uint32_t)t.second * 8;
            ++badA;
        }
    }
    for (auto& r : s_w_regs) {
        const uint64_t *a = (const uint64_t *)r.p, *b = (const uint64_t *)r.s0.data();
        for (size_t w = 0, nw = r.n / 8; w < nw; ++w) {
            if (a[w] == b[w]) continue;
            // tocada por la nativa: ¿la tocó también la original con el mismo valor? (ya comparado arriba)
            bool inA = false;
            for (auto& t : touched) if (t.first == &r && t.second == w) { inA = true; break; }
            if (!inA) { ++nbytes; ++badB; if (!bad++) firstbad = r.base + (uint32_t)w * 8; lastbad = r.base + (uint32_t)w * 8; }
        }
    }
    s_w_ck_membytes += nbytes * 8;
    if (bad && s_w_ck_bad_mem++ < 10)
        std::printf("[WAGUA] check: MEMORIA distinta en la llamada %llu: %llu palabras (%llu tocadas por la original, "
                    "%llu solo en la segunda), de %08x a %08x\n",
                    (unsigned long long)s_w_ck_calls, (unsigned long long)bad, (unsigned long long)badA,
                    (unsigned long long)badB, firstbad, lastbad);
    if (s_w_ck_calls <= 3 || std::getenv("NWII_W_AGUA_CHECK_V")) {
        std::printf("[WAGUA] check #%llu: captura %zu/%zu B, pc %08x/%08x lr %08x/%08x r1 %08x/%08x, palabras tocadas %zu, distintas %llu\n",
                    (unsigned long long)s_w_ck_calls, capA.size(), capB.size(), sa.pc, sb.pc, sa.lr, sb.lr, sa.gpr[1], sb.gpr[1],
                    touched.size(), (unsigned long long)bad);
        uint32_t prev = 0, start = 0; int nr = 0;
        for (auto& t : touched) {
            const uint32_t a = t.first->base + (uint32_t)t.second * 8;
            if (a != prev + 8) { if (start && nr < 12) std::printf("   [%08x..%08x]\n", start, prev + 8); if (start) ++nr; start = a; }
            prev = a;
        }
        if (start && nr < 12) std::printf("   [%08x..%08x]\n", start, prev + 8);
        std::fflush(stdout);
    }
    // al pipe de verdad: las escrituras de la nativa, una a una, por las funciones de siempre
    w_replay(capB);
}

// Tipos (this+0xCE94): 0 -> 8000B530 (el de agua_pinguino, comprobado); 1 -> 8000C5C0 u 8000BA70 (según el
// escenario), 2 -> 8000CA50, 4 -> 8000C020. Por defecto la versión nativa SOLO se usa para los tipos comprobados
// en PC (s_w_ok); los demás van a la función generada de siempre. NWII_W_AGUA_TODOS="1": nativa para todos.
// Subrutina a la que salta 8000D000 (mismo cálculo que su código: tipo en this+0xCE94; para el tipo 1, la elección
// por mundo/nivel/zona de los bytes 4620/4621/4622 del objeto en r13-21688):
//   0: tipo 0 -> 8000B530 · 1: tipo 1 -> 8000BA70 · 2: tipo 1 (W8-3, W8-C zona 3, W2-C zona 0) -> 8000C5C0 ·
//   3: tipo 2 -> 8000CA50 · 4: tipo 4 -> 8000C020 · 5: otro tipo (8000D000 vuelve sin dibujar)
int w_route(CPUContext& ctx, uint32_t ty) {
    if (ty == 0) return 0;
    if (ty == 2) return 3;
    if (ty == 4) return 4;
    if (ty != 1) return 5;
    const uint32_t g = ctx.mmu.read32(ctx.gpr[13] - 21688u);
    const uint32_t w = ctx.mmu.read8(g + 4620), l = ctx.mmu.read8(g + 4621), a = ctx.mmu.read8(g + 4622);
    if ((w == 7 && l == 2) || (w == 7 && l == 23 && a == 3) || (w == 1 && l == 23 && a == 0)) return 2;
    return 1;
}
// Las 5 rutas comprobadas en PC con NWII_W_AGUA_CHECK (0 diferencias de pipe, registros y memoria): B530 en
// agua_pinguino/playa_hielo/playa_yoshi/casa_oscura, BA70 en castillo6/lava/8-C, C5C0 en 2-C/8-3, CA50 en jungla,
// C020 en 2-5 (r9/r44w/LEEME.md). La ruta 5 (tipo sin dibujo) va a la generada (no hay nada que ganar).
// NWII_W_AGUA_SOLO0="1": nativa solo para el tipo 0 (válvula de seguridad); NWII_W_AGUA_TODOS="1": también la ruta 5.
bool s_w_solo0 = false;
inline bool w_route_ok(int r) { return s_w_solo0 ? r == 0 : (r <= 4 || s_w_all); }
template <int K> void w_hook(CPUContext& ctx) {
    const uint32_t ty = w_stats(ctx);
    const int rt = w_route(ctx, ty);
    ++s_w_route[rt];
    if (!w_route_ok(rt)) { ++s_w_fallback; s_w_orig[K](ctx); return; }
    if (s_w_check) w_check(ctx, K);
    else if (s_w_native) s_w_nat[K](ctx);
    else s_w_orig[K](ctx);
}
const nwii_fn s_w_hooks[3] = {w_hook<0>, w_hook<1>, w_hook<2>};
}  // namespace

extern "C" void w_agua_install(nwii::runtime::CPUContext *ctx) {
    const bool nat = std::getenv("NWII_W_AGUA") != nullptr;
    const bool chk = std::getenv("NWII_W_AGUA_CHECK") != nullptr;
    const bool st = std::getenv("NWII_W_AGUA_STATS") != nullptr;
    if (!nat && !chk && !st) return;
    const nwii_fn expect[3] = {func_8000D000, func_807B6E50, func_807B6E60};
    if (!g_disp_table) { std::printf("[WAGUA] sin tabla de despacho (NWII_SLOW_DISPATCH): no se engancha\n"); return; }
    for (int i = 0; i < 3; ++i) {
        nwii_fn &e = g_disp_table[(kWAddr[i] - g_disp_base) >> 2];
        if (e != expect[i]) { std::printf("[WAGUA] la tabla no apunta a la función generada en %08x: no se engancha\n", kWAddr[i]); return; }
    }
    for (int i = 0; i < 3; ++i) {
        nwii_fn &e = g_disp_table[(kWAddr[i] - g_disp_base) >> 2];
        s_w_orig[i] = e;
        e = s_w_hooks[i];
    }
    s_w_native = nat || chk;
    s_w_all = std::getenv("NWII_W_AGUA_TODOS") != nullptr;
    s_w_solo0 = std::getenv("NWII_W_AGUA_SOLO0") != nullptr;
    s_w_check = chk;
    s_w_t0 = std::chrono::steady_clock::now();
    if (chk && ctx) {
        auto add = [](uint8_t *p, size_t n, uint32_t base) {
            WRegion r{p, n, base, std::vector<uint8_t>(n), std::vector<uint8_t>(n)};
            s_w_regs.push_back(std::move(r));
        };
        add(ctx->mmu.mem1.data(), ctx->mmu.mem1.size(), 0x80000000u);
        add(ctx->mmu.mem2.data(), ctx->mmu.mem2.size(), 0x90000000u);
        if (ctx->mmu.lc.size()) add(ctx->mmu.lc.data(), ctx->mmu.lc.size(), 0xE0000000u);
    }
    std::printf("[WAGUA] renderizador del agua: %s (0x8000D000, 0x807B6E50, 0x807B6E60 enganchadas), nativo para %s\n",
                chk ? "NATIVO + COMPROBACIÓN contra el generado" : nat ? "NATIVO" : "generado (solo estadística)",
                s_w_solo0 ? "SOLO el tipo 0 (NWII_W_AGUA_SOLO0; el resto: función generada)" : s_w_all ? "todas las rutas (NWII_W_AGUA_TODOS)" : "las 5 rutas que dibujan (comprobadas en PC)");
}
