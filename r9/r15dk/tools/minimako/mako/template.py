# Subconjunto mínimo de mako.template.Template para generar ir_expression_operation*.h de uam/Mesa:
# líneas de control "% for/if/elif/else/endfor/endif", sustituciones ${expr} y "\" al final de línea.
import re

class Template:
    def __init__(self, text):
        self.text = text

    def render(self, **kw):
        src = ["def __render(__out):"]
        depth = 1
        lines = self.text.split("\n")
        for i, line in enumerate(lines):
            nl = "\n" if i < len(lines) - 1 else ""
            st = line.strip()
            if st.startswith("%") and not st.startswith("%%"):
                c = st[1:].strip()
                if c.startswith("end"):
                    depth -= 1
                    continue
                if c.startswith(("else", "elif")):
                    src.append("    " * (depth - 1) + c)
                    continue
                src.append("    " * depth + c)
                depth += 1
                continue
            if st.startswith("##"):
                continue
            if line.endswith("\\"):
                line, nl = line[:-1], ""
            # partir en texto y ${...} (con llaves anidadas)
            parts = []
            j = 0
            while True:
                k = line.find("${", j)
                if k < 0:
                    parts.append(("t", line[j:]))
                    break
                parts.append(("t", line[j:k]))
                d, m = 1, k + 2
                while d:
                    if line[m] == "{": d += 1
                    elif line[m] == "}": d -= 1
                    m += 1
                parts.append(("e", line[k + 2:m - 1]))
                j = m
            parts.append(("t", nl))
            for kind, s in parts:
                if not s:
                    continue
                if kind == "t":
                    src.append("    " * depth + "__out.append(%r)" % s)
                else:
                    src.append("    " * depth + "__out.append(str(%s))" % s)
        src.append("    " * depth + "pass")
        g = dict(kw)
        exec("\n".join(src), g)
        out = []
        g["__render"](out)
        return "".join(out)
