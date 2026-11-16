#!/usr/bin/env python3
"""apply_r9_cpucode.py <export_dir>

R9 cpucode post-pass (runs after apply_fastdispatch.py and apply_r8_codegen.py).
nwii_dispatch (every indirect guest call: virtual calls, function pointers,
blr/bctr) read g_disp_base and g_disp_span, two constants, from memory, and
g_disp_table through the GOT: on AArch64 (PIE, no GOT relaxation in ld) that is
adrp + ldr (GOT) + ldr per value. Here the two constants become inline constexpr
values in functions.h (same values, taken from fastdispatch.cpp) and
g_disp_table gets hidden visibility (one static module), so nwii_dispatch reads
one pointer PC-relative. Same table, same tests, same calls.
Applied once: marks functions.h with // [r9cpucode] and refuses to run twice.
"""
import re, sys, os

MARK = '// [r9cpucode]'

def main():
    d = sys.argv[1]
    fh = os.path.join(d, 'functions.h')
    fd = os.path.join(d, 'fastdispatch.cpp')
    h = open(fh).read()
    if MARK in h:
        sys.exit('apply_r9_cpucode: already applied to ' + d)
    c = open(fd).read()
    base = re.search(r'^const uint32_t g_disp_base = (0x[0-9A-Fa-f]+u);$', c, re.M)
    span = re.search(r'^const uint32_t g_disp_span = (0x[0-9A-Fa-f]+u);$', c, re.M)
    if not base or not span:
        sys.exit('apply_r9_cpucode: g_disp_base/g_disp_span definitions not found')
    old_decl = 'extern const uint32_t g_disp_base, g_disp_span;\nextern nwii_fn *g_disp_table;\n'
    if h.count(old_decl) != 1:
        sys.exit('apply_r9_cpucode: dispatch declarations not found in functions.h')
    new_decl = (MARK + ' constants inline, table hidden (no GOT loads in nwii_dispatch)\n'
                'inline constexpr uint32_t g_disp_base = %s, g_disp_span = %s;\n'
                'extern nwii_fn *g_disp_table __attribute__((visibility("hidden")));\n'
                % (base.group(1), span.group(1)))
    h = h.replace(old_decl, new_decl)
    c = c.replace(base.group(0) + '\n', '').replace(span.group(0) + '\n', '')
    c = c.replace('nwii_fn *g_disp_table = [] {', MARK + ' g_disp_base / g_disp_span: functions.h\nnwii_fn *g_disp_table = [] {', 1)
    open(fh, 'w').write(h)
    open(fd, 'w').write(c)
    print('apply_r9_cpucode: g_disp_base=%s g_disp_span=%s' % (base.group(1), span.group(1)))

if __name__ == '__main__':
    main()
