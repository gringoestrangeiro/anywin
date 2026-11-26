#!/usr/bin/env python3
"""Generate anywin's API coverage assets.

The map follows the AnyPS5 / xbox-emulator progress-map style: one square per
API, grouped into a treemap by DLL.  "Covered" means the API is exercised by at
least one sample executable in the corpus (todo.exe plus the test probes built
under build/).  Everything else exported by the runtime is implemented but not
yet exercised by a test.

Outputs:
  docs/img/progress.svg        treemap used by README.md
  docs/img/badge-*.svg         README badges
  docs/COVERAGE.md             per-module and per-program tables
  tools/coverage/summary.json  machine-readable headline numbers
  tools/coverage/data/corpus.json  snapshot of the sample-program imports
"""
import json
import re
import struct
import sys
from datetime import datetime, timezone
from pathlib import Path
from xml.sax.saxutils import escape

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DATA = HERE / "data"
IMG = ROOT / "docs" / "img"

GREEN, AMBER, RED, GREY, BG, TEXT = "#2ea043", "#d29922", "#da3633", "#6e7681", "#0d1117", "#ffffff"
COL = {"covered": GREEN, "implemented": AMBER, "done": GREEN, "todo": GREY, "missing": RED}

MACROS = {"API", "APIV", "API_ASM", "APIDATA", "ALIAS"}


# ---------------------------------------------------------------- runtime ---
def split_args(s):
    parts, start, depth, quote, esc = [], 0, 0, False, False
    for i, c in enumerate(s):
        if quote:
            if esc:
                esc = False
            elif c == "\\":
                esc = True
            elif c == '"':
                quote = False
            continue
        if c == '"':
            quote = True
        elif c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            parts.append(s[start:i])
            start = i + 1
    parts.append(s[start:])
    return parts


def runtime_exports():
    exports = {}
    for path in sorted((ROOT / "runtime").rglob("*.c")):
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'\bAW_EXPORT_ENTRY\(\s*([A-Za-z0-9_]+)\s*,\s*"([^"]+)"', text):
            exports.setdefault(m.group(1), set()).add(m.group(2))
        for m in re.finditer(r"\b(API|APIV|API_ASM|APIDATA|ALIAS)\s*\(", text):
            kind = m.group(1)
            i, depth, quote, esc = m.end(), 1, False, False
            while i < len(text) and depth:
                c = text[i]
                if quote:
                    if esc:
                        esc = False
                    elif c == "\\":
                        esc = True
                    elif c == '"':
                        quote = False
                else:
                    if c == '"':
                        quote = True
                    elif c == "(":
                        depth += 1
                    elif c == ")":
                        depth -= 1
                i += 1
            args = [a.strip() for a in split_args(text[m.end():i - 1])]
            try:
                if kind in ("API", "APIV", "APIDATA"):
                    module, name = args[0], args[2]
                elif kind == "API_ASM":
                    module, name = args[0], args[1]
                else:  # ALIAS
                    module, name = args[0], args[1]
            except IndexError:
                continue
            name = name.strip().strip('"')
            if not re.fullmatch(r"[A-Za-z0-9_@?$]+", module) or not name:
                continue
            if module == "unimplemented":
                continue
            exports.setdefault(module, set()).add(name)
    return {m: sorted(v) for m, v in sorted(exports.items())}


# ------------------------------------------------------------------- PE -----
def load_inventory():
    data = json.loads((DATA / "win32api.json").read_text(encoding="utf-8"))
    modules = {}
    for dll, names in data["dlls"].items():
        modules.setdefault(normalize_module(dll), set()).update(names)
    return data, {m: sorted(v) for m, v in sorted(modules.items())}


def normalize_module(dll):
    b = re.split(r"[\\/]", dll)[-1].lower()
    if b.endswith((".dll", ".drv", ".exe")):
        b = b[:-4]
    for p in ("api-ms-win-crt-", "ucrtbase", "msvcr", "vcruntime", "msvcrt"):
        if b.startswith(p):
            return "msvcrt"
    if b.startswith("api-ms-win-core-registry") or b.startswith("api-ms-win-security") or \
            b.startswith("api-ms-win-eventing") or b == "sechost":
        return "advapi32"
    if b.startswith("api-ms-win-core-") or b == "kernelbase":
        return "kernel32"
    if b.startswith("api-ms-win-shell-") or b.startswith("api-ms-win-shcore"):
        return "shell32"
    if b.startswith("api-ms-win-ntuser"):
        return "user32"
    if b == "wsock32":
        return "ws2_32"
    return re.sub(r"[^a-z0-9_]", "_", b)


def pe_imports(path):
    try:
        b = path.read_bytes()
        if b[:2] != b"MZ":
            return []
        pe = struct.unpack_from("<I", b, 0x3C)[0]
        if b[pe:pe + 4] != b"PE\0\0":
            return []
        coff = pe + 4
        nsec = struct.unpack_from("<H", b, coff + 2)[0]
        optsize = struct.unpack_from("<H", b, coff + 16)[0]
        opt = coff + 20
        if struct.unpack_from("<H", b, opt)[0] != 0x20B:
            return []
        imp_rva = struct.unpack_from("<I", b, opt + 112 + 8)[0]
        if not imp_rva:
            return []
        sects = []
        sh = opt + optsize
        for n in range(nsec):
            off = sh + 40 * n
            vsize = struct.unpack_from("<I", b, off + 8)[0]
            va = struct.unpack_from("<I", b, off + 12)[0]
            rawsize = struct.unpack_from("<I", b, off + 16)[0]
            rawptr = struct.unpack_from("<I", b, off + 20)[0]
            sects.append((va, max(vsize, rawsize), rawptr, rawsize))

        def off_of(rva):
            for va, size, rawptr, rawsize in sects:
                if va <= rva < va + size and rva - va < rawsize:
                    return rawptr + rva - va
            return None

        def cstr(rva):
            o = off_of(rva)
            if o is None:
                return None
            e = b.find(b"\0", o)
            return b[o:e].decode("latin-1") if e >= 0 else None

        out = []
        desc = off_of(imp_rva)
        while desc:
            oft, _ts, _fc, name_rva, ft = struct.unpack_from("<IIIII", b, desc)
            if not name_rva:
                break
            module = normalize_module(cstr(name_rva) or "")
            thunk = off_of(oft or ft)
            n = 0
            while thunk is not None:
                val = struct.unpack_from("<Q", b, thunk + n * 8)[0]
                if not val:
                    break
                if val >> 63:
                    name = "#%d" % (val & 0xFFFF)
                else:
                    name = cstr((val & 0x7FFFFFFF) + 2) or ""
                if name:
                    out.append((module, name))
                n += 1
            desc += 20
        return sorted(set(out))
    except (OSError, IndexError, struct.error, ValueError):
        return []


def program_name(path):
    rel = path.relative_to(ROOT)
    return str(rel)


def load_corpus():
    snap = DATA / "corpus.json"
    known = json.loads(snap.read_text(encoding="utf-8"))["programs"] if snap.exists() else {}
    programs = dict(known)
    found = False
    candidates = sorted(ROOT.glob("*.exe")) + sorted((ROOT / "build").glob("*.exe"))
    for path in candidates:
        imports = pe_imports(path)
        if imports:
            programs[program_name(path)] = imports
            found = True
    if found:
        DATA.mkdir(parents=True, exist_ok=True)
        snap.write_text(json.dumps(
            {"generated": datetime.now(timezone.utc).strftime("%Y-%m-%d"),
             "programs": programs}, indent=1) + "\n", encoding="utf-8")
    return programs


# ---------------------------------------------------------------- render ----
def squarify(items, x, y, w, h):
    out, items = [], list(items)
    total = sum(a for _, a in items)
    if not total:
        return out
    scale = w * h / total
    items = [(k, a * scale) for k, a in items]

    def worst(row, side):
        s = sum(a for _, a in row)
        return max(max(side * side * a / (s * s), s * s / (side * side * a)) for _, a in row)

    while items:
        side = min(w, h)
        row = [items.pop(0)]
        while items and worst(row + [items[0]], side) <= worst(row, side):
            row.append(items.pop(0))
        s = sum(a for _, a in row)
        if w >= h:
            cw, cy = s / h, y
            for k, a in row:
                out.append((k, x, cy, cw, a / cw))
                cy += a / cw
            x += cw
            w -= cw
        else:
            ch, cx = s / w, x
            for k, a in row:
                out.append((k, cx, y, a / ch, ch))
                cx += a / ch
            y += ch
            h -= ch
    return out


def text(x, y, s, size=12, fill=TEXT):
    return (f'<text x="{x:.2f}" y="{y:.2f}" font-family="sans-serif" font-size="{size}" fill="{fill}" '
            f'stroke="{BG}" stroke-width="3" paint-order="stroke">{escape(s)}</text>')


DISPLAY = {"advapi32": "advapi", "shell32": "shell"}


def tile_label(key, cw, ch):
    if ch < 18:
        return None
    label = DISPLAY.get(key, key)
    for size in (12, 11, 10, 9, 8):
        if len(label) * size * 0.6 <= cw - 10:
            return label, size
    return None


def cell_rows(count, x, y, w, h):
    """Lay out `count` cells row by row, filling the rectangle without tails."""
    if not count or w <= 0 or h <= 0:
        return []
    rows = max(1, min(count, round((count * h / w) ** 0.5)))
    out = []
    for r in range(rows):
        in_row = count // rows + (r < count % rows)
        cw = w / in_row
        out.append([(x + c * cw, y + r * h / rows, cw, h / rows) for c in range(in_row)])
    return out


def section(o, y, title, groups, height, states, legend):
    n = sum(len(v) for v in groups.values())
    done = sum(v.count(states[0]) for v in groups.values())
    pct = 100 * done / n if n else 0
    o.append(text(4, y + 21, f"{title}: {pct:.2f}% ({done}/{n})", 16))
    y += 30
    lx = 4
    for label, colour in legend:
        o.append(f'<rect x="{lx}" y="{y + 2}" width="10" height="10" fill="{colour}"/>')
        o.append(text(lx + 14, y + 11, label, 11))
        lx += 14 + 7 * len(label) + 14
    y += 18
    order = sorted(groups.items(), key=lambda kv: (-len(kv[1]), kv[0]))
    for key, cx, cy, cw, ch in squarify([(k, len(v)) for k, v in order], 0, y, 1000, height):
        st = sorted(groups[key], key=lambda s: list(states).index(s))
        cnt = len(st)
        c = st.count(states[0])
        o.append(f'<g><title>{escape(key)}: {c}/{cnt} ({100 * c // cnt}%)</title>')
        i = 0
        for row in cell_rows(cnt, cx + 1, cy + 1, cw - 2, ch - 2):
            j = 0
            while j < len(row):
                s = st[i + j]
                k = j + 1
                while k < len(row) and st[i + k] == s:
                    k += 1
                px, py, _pw, ph = row[j]
                right = row[k - 1][0] + row[k - 1][2]
                o.append(f'<rect x="{px:.2f}" y="{py:.2f}" width="{right - px:.2f}" '
                         f'height="{ph:.2f}" fill="{COL[s]}" stroke="{BG}" stroke-width="0.5"/>')
                j = k
            i += len(row)
        o.append(f'<rect x="{cx:.2f}" y="{cy:.2f}" width="{cw:.2f}" height="{ch:.2f}" '
                 f'fill="none" stroke="{BG}" stroke-width="2"/>')
        label = tile_label(key, cw, ch)
        if label:
            o.append(text(cx + 5, cy + 16, label[0], label[1]))
        o.append("</g>")
    return y + height + 10, done, n


def badge(name, label, value, colour):
    lw, vw = 7 * len(label) + 12, 7 * len(value) + 12
    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" width="{lw + vw}" height="20" role="img" '
           f'aria-label="{escape(label)}: {escape(value)}">'
           f'<rect width="{lw}" height="20" fill="#555"/>'
           f'<rect x="{lw}" width="{vw}" height="20" fill="{colour}"/>'
           f'<g fill="#fff" font-family="Verdana,DejaVu Sans,sans-serif" font-size="11" text-anchor="middle">'
           f'<text x="{lw / 2}" y="14">{escape(label)}</text>'
           f'<text x="{lw + vw / 2}" y="14">{escape(value)}</text></g></svg>')
    (IMG / f"badge-{name}.svg").write_text(svg, encoding="utf-8")


def pct_colour(p):
    return "#4c1" if p >= 90 else "#97ca00" if p >= 70 else "#dfb317" if p >= 40 else "#fe7d37"


def update_readme(summary):
    path = ROOT / "README.md"
    text = path.read_text(encoding="utf-8")
    block = (
        "<!-- coverage-text:start -->\n"
        f"**Windows APIs implemented: {summary['win32_coverage']['implemented']:,} / "
        f"{summary['win32_coverage']['total']:,} ({summary['win32_coverage']['pct']:.2f}%).** "
        f"Started modules: {summary['started_modules']['implemented']:,} / "
        f"{summary['started_modules']['total']:,} ({summary['started_modules']['pct']:.2f}%). "
        f"Sample-program imports: {summary['sample_imports']['covered']:,} / "
        f"{summary['sample_imports']['total']:,} ({summary['sample_imports']['pct']:.2f}%). "
        f"Runtime exports: {summary['runtime']['exports']:,}.\n"
        "<!-- coverage-text:end -->"
    )
    new = re.sub(r"<!-- coverage-text:start -->.*?<!-- coverage-text:end -->", block, text, flags=re.S)
    if new != text:
        path.write_text(new, encoding="utf-8")


def main():
    exports = runtime_exports()
    inventory_source, inventory = load_inventory()
    corpus = load_corpus()
    module_imports = {}
    for imports in corpus.values():
        for module, name in imports:
            module_imports.setdefault(module, set()).add(name)

    # Section 1: every function in Microsoft's Win32 metadata, grouped by DLL.
    api_groups = {}
    for module, names in inventory.items():
        have = set(exports.get(module, ()))
        api_groups[module] = ["done" if n in have else "todo" for n in names]

    # Section 2: zoom in on modules where work has started.
    started = {m: g for m, g in api_groups.items() if "done" in g}

    # Section 3: the union of sample-program imports, green when implemented.
    import_groups = {}
    for module, names in sorted(module_imports.items()):
        have = set(exports.get(module, ()))
        import_groups[module] = ["done" if n in have else "missing" for n in sorted(names)]

    IMG.mkdir(parents=True, exist_ok=True)
    o, y = [], 0
    y, api_done, api_total = section(
        o, y, "Windows APIs implemented", api_groups, 620,
        ("done", "todo"),
        [("implemented in anywin", GREEN), ("known, not implemented", GREY)])
    y, started_done, started_total = section(
        o, y, "Started modules", started, 260,
        ("done", "todo"),
        [("implemented in anywin", GREEN), ("known, not implemented", GREY)])
    y, imp_done, imp_total = section(
        o, y, "Sample-program imports", import_groups, 170,
        ("done", "missing"),
        [("implemented", GREEN), ("missing", RED)])

    svg = (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 1000 {y:.0f}" '
           f'width="1000" height="{y:.0f}">\n'
           f'<rect x="0" y="0" width="1000" height="{y:.0f}" fill="{BG}"/>\n'
           + "\n".join(o) + "\n</svg>\n")
    (IMG / "progress.svg").write_text(svg, encoding="utf-8")

    api_pct = 100 * api_done / api_total if api_total else 0
    imp_pct = 100 * imp_done / imp_total if imp_total else 0
    badge("api", "Windows APIs implemented", f"{api_pct:.2f}%", pct_colour(api_pct))
    badge("imports", "sample imports", f"{imp_pct:.2f}%", pct_colour(imp_pct))
    badge("programs", "sample programs", str(len(corpus)), "#007ec6")
    badge("apis", "runtime exports", str(sum(len(v) for v in exports.values())), "#007ec6")

    modules = []
    for module, names in inventory.items():
        have = set(exports.get(module, ()))
        done = sum(1 for n in names if n in have)
        modules.append({
            "module": module,
            "apis": len(names),
            "implemented": done,
            "pct": round(100 * done / len(names), 2) if names else 0,
            "started": done > 0,
        })
    modules.sort(key=lambda r: (-r["implemented"], -r["apis"], r["module"]))

    outside = {}
    for module, names in exports.items():
        extra = sorted(set(names) - set(inventory.get(module, ())))
        if extra:
            outside[module] = extra
    outside_count = sum(len(v) for v in outside.values())
    matched = sum(len(set(v) & set(inventory.get(m, ()))) for m, v in exports.items())

    missing = sorted((m, n) for m, names in module_imports.items()
                     for n in names if n not in set(exports.get(m, ())))
    programs = []
    for name in sorted(corpus):
        imports = corpus[name]
        good = sum(1 for m, f in imports if f in set(exports.get(m, ())))
        programs.append({"program": name, "imports": len(imports), "covered": good,
                         "pct": round(100 * good / len(imports), 2) if imports else 0})

    summary = {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "inventory": {
            "source": inventory_source["source"],
            "version": inventory_source["version"],
            "published": inventory_source["published"],
            "url": inventory_source["url"],
            "dlls": len(inventory_source["dlls"]),
            "modules": len(inventory),
            "functions": api_total,
        },
        "win32_coverage": {"implemented": api_done, "total": api_total, "pct": round(api_pct, 2)},
        "started_modules": {"implemented": started_done, "total": started_total,
                            "pct": round(100 * started_done / started_total, 2) if started_total else 0},
        "runtime": {"exports": sum(len(v) for v in exports.values()), "modules": len(exports),
                    "matched": matched, "outside_inventory": outside_count},
        "sample_imports": {"covered": imp_done, "total": imp_total, "pct": round(imp_pct, 2)},
        "sample_programs": len(programs),
        "module_rows": modules,
        "program_rows": programs,
        "implemented_outside_inventory": outside,
        "missing_imports": [{"module": m, "function": n} for m, n in missing],
    }
    (HERE / "summary.json").write_text(json.dumps(summary, indent=1) + "\n", encoding="utf-8")
    update_readme(summary)

    lines = [
        "# Windows API coverage",
        "",
        f"Generated by [`tools/coverage/gen.py`](../tools/coverage/gen.py) on {summary['generated'][:10]}. "
        "One square in the map is one function; the map style follows the AnyPS5 progress map.",
        "",
        f"Inventory: **{inventory_source['source']} {inventory_source['version']}** "
        f"([release]({inventory_source['url']})), "
        f"{api_total:,} P/Invoke entry points across {len(inventory_source['dlls'])} DLLs. "
        "This is Microsoft's own metadata for documented Win32 exports, not a Wine or MinGW list. "
        "It counts exported functions only; COM vtable methods and undocumented exports are not in "
        "this denominator, and C runtime functions are reported separately below.",
        "",
        "| Metric | Implemented | Total | Coverage |",
        "|---|---:|---:|---:|",
        f"| Windows APIs | {api_done:,} | {api_total:,} | {api_pct:.2f}% |",
        f"| Windows APIs in started modules | {started_done:,} | {started_total:,} | "
        f"{summary['started_modules']['pct']:.2f}% |",
        f"| Sample-program imports | {imp_done:,} | {imp_total:,} | {imp_pct:.2f}% |",
        f"| Runtime exports (including C runtime) | {summary['runtime']['exports']:,} | "
        f"{summary['runtime']['exports']:,} | 100.00% |",
        "",
        "The first map section is the full Win32 inventory. The second zooms in on the modules "
        "where anywin has started. Green means an implementation exists in the runtime; grey means "
        "the function is known but not implemented. The third section is the union of imports across "
        "the sample programs, so it answers a different question: whether those particular programs "
        "can resolve everything they import.",
        "",
        "## By module",
        "",
        "| Module | Windows APIs | Implemented | Coverage |",
        "|---|---:|---:|---:|",
    ]
    for row in modules:
        lines.append(f"| `{row['module']}` | {row['apis']} | {row['implemented']} | {row['pct']:.1f}% |")
    lines += ["", "## Implemented outside the Win32 inventory", "",
              "These runtime exports are either C runtime names or functions that Microsoft's "
              "metadata declares under a different DLL (forwarders such as the `Rtl*` unwind "
              "helpers, plus legacy `A` aliases). They are real implementations, but they do not "
              "count toward the Win32 denominator above.", "",
              "| Module | Exports | Functions |", "|---|---:|---|"]
    for module, names in sorted(outside.items()):
        shown = ", ".join(f"`{n}`" for n in names[:8]) + (f" … (+{len(names) - 8})" if len(names) > 8 else "")
        lines.append(f"| `{module}` | {len(names)} | {shown} |")
    lines += ["", "## Sample programs", "", "| Program | Imports | Implemented | Coverage |",
              "|---|---:|---:|---:|"]
    for row in programs:
        lines.append(f"| `{row['program']}` | {row['imports']} | {row['covered']} | {row['pct']:.1f}% |")
    if missing:
        lines += ["", "## Missing sample imports", "", "| Module | Function |", "|---|---|"]
        lines += [f"| `{m}` | `{n}` |" for m, n in missing]
    else:
        lines += ["", "No sample-program import is missing from the runtime."]
    lines += ["", "Regenerate with `make coverage` (after `make` and, for the full sample set, "
              "`make test`). The inventory snapshot is "
              "[`tools/coverage/data/win32api.json`](../tools/coverage/data/win32api.json); the "
              "sample-program import snapshot is "
              "[`tools/coverage/data/corpus.json`](../tools/coverage/data/corpus.json).", ""]
    (ROOT / "docs" / "COVERAGE.md").write_text("\n".join(lines), encoding="utf-8")

    print(f"Windows APIs {api_done}/{api_total} ({api_pct:.2f}%)  "
          f"started {started_done}/{started_total}  "
          f"imports {imp_done}/{imp_total} ({imp_pct:.2f}%)  "
          f"programs {len(programs)}  height {y:.0f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
