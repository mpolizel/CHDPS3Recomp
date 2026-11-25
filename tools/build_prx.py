"""build_prx.py - relocate and lift the game's own PRX modules.

Castlevania: HoD loads its characters, enemies and shots as PRX modules at run
time (sys_prx_load_module by path). A static recompile has no loader for guest
code, so every module is pre-linked at its own fixed guest base, lifted with a
symbol prefix, and described in a generated table the runtime uses
(src/port_prx.c).

For each game/decrypted/**/*.prx:
  1. ext/ps3recomp/tools/lift_prx.py relocates it at base B (from 0x40000000,
     a range the ps3recomp memory map leaves unused) and emits the image,
     function seeds, exports (module_start) and imports.
  2. The imports (sys_libc stubs) become --hle-stubs, so each stub turns into
     ps3_hle_call(nid) like the EBOOT's.
  3. ppu_lifter.py lifts the image in --raw mode with prefix chdprx_<name>_.
Outputs:
  game/prx/<name>.bin               relocated images, read at run time
  recompiled_prx/<name>.cpp/.h      lifted modules
  recompiled_prx/prx_table.c        path -> {base, image, functions, module_start}

Usage: python tools/build_prx.py [--only SUBSTRING] [--jobs N]
"""
import argparse, concurrent.futures as cf, json, os, re, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "ext", "ps3recomp", "tools")
DECRYPTED = os.path.join(ROOT, "game", "decrypted", "USRDIR")
WORK = os.path.join(ROOT, "out", "prx")
IMAGES = os.path.join(ROOT, "game", "prx")
LIFTED = os.path.join(ROOT, "recompiled_prx")
BASE0 = 0x40000000
BASE_END = 0x50000000
ALIGN = 0x10000
MODULE_START_NID = "0xBC9A0086"


def find_modules():
    mods = []
    for dirpath, _, files in os.walk(DECRYPTED):
        for f in files:
            if f.endswith(".ppu.prx"):
                full = os.path.join(dirpath, f)
                rel = os.path.relpath(full, DECRYPTED).replace("\\", "/")
                mods.append((rel, full))
    mods.sort()
    return mods


def c_ident(rel):
    name = os.path.basename(rel).replace(".ppu.prx", "")
    return re.sub(r"[^A-Za-z0-9_]", "_", name)


def load_segments(prx):
    """(code_end_offset, image_top). The first PT_LOAD segment holds .text,
    the import stubs AND .rodata, so the code end comes from the section
    headers (last SHF_EXECINSTR section): lifting .rodata as code turned string
    tables into thousands of bogus functions and ~10x the C++."""
    import struct
    d = open(prx, "rb").read()
    phoff = struct.unpack_from(">Q", d, 0x20)[0]
    phnum = struct.unpack_from(">H", d, 0x38)[0]
    loads = []
    for i in range(phnum):
        t, flags, off, va, pa, fs, ms, al = struct.unpack_from(">IIQQQQQQ", d, phoff + i * 56)
        if t == 1:
            loads.append((va, fs, ms, flags))
    top = max(va + ms for va, fs, ms, fl in loads)
    code_end = loads[0][0] + loads[0][1]
    shoff = struct.unpack_from(">Q", d, 0x28)[0]
    shentsize, shnum = struct.unpack_from(">HH", d, 0x3A)
    execs = []
    for i in range(shnum):
        s = struct.unpack_from(">IIQQQQIIQQ", d, shoff + i * shentsize)
        if s[2] & 0x4 and s[5]:          # SHF_EXECINSTR
            execs.append(s[3] + s[5])
    if execs:
        code_end = max(execs)
    return code_end, top


def relocate(rel, prx, ident, base):
    out = os.path.join(WORK, ident)
    if os.path.isdir(out):
        shutil.rmtree(out)
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "lift_prx.py"), prx,
                        "--base", hex(base), "-o", out], capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{rel}: lift_prx failed: {r.stdout}{r.stderr}")
    stem = os.path.basename(prx)[:-4]
    exp = json.load(open(os.path.join(out, stem + "_exports.json")))
    imp = json.load(open(os.path.join(out, stem + "_imports.json")))
    stubs = [{"library": lib, "nid": nid, "stub": stub}
             for lib, m in imp.items() for nid, stub in m.items()]
    json.dump(stubs, open(os.path.join(out, "hle_stubs.json"), "w"), indent=1)
    start_opd = 0
    for lib, m in exp.get("exports", {}).items():
        for nid, opd in m.items():
            if int(nid, 16) == int(MODULE_START_NID, 16):
                start_opd = int(opd, 16)
    return {"rel": rel, "ident": ident, "base": base, "toc": int(exp["toc"], 16),
            "work": out, "stem": stem, "module_start": start_opd}


def lift(m, code_end):
    out = os.path.join(m["work"], "lifted")
    prefix = f"chdprx_{m['ident']}_"
    r = subprocess.run([sys.executable, os.path.join(TOOLS, "ppu_lifter.py"),
                        os.path.join(m["work"], m["stem"] + "_image.bin"),
                        "--raw", "--base", hex(m["base"]), "--toc", hex(m["toc"]),
                        "--code-end", hex(code_end),
                        "--functions", os.path.join(m["work"], m["stem"] + "_functions.json"),
                        "--hle-stubs", os.path.join(m["work"], "hle_stubs.json"),
                        "--symbol-prefix", prefix, "--single-file", "--jobs", "1",
                        "--header-name", f"chdprx_{m['ident']}.h",
                        "--source-name", f"chdprx_{m['ident']}.cpp", "-o", out],
                       capture_output=True, text=True)
    if r.returncode:
        raise RuntimeError(f"{m['rel']}: ppu_lifter failed:\n{r.stdout[-2000:]}{r.stderr[-2000:]}")
    for f in (f"chdprx_{m['ident']}.cpp", f"chdprx_{m['ident']}.h"):
        shutil.copy(os.path.join(out, f), os.path.join(LIFTED, f))
    n = re.search(r"(\d+) functions lifted", r.stdout)
    return int(n.group(1)) if n else 0


def write_table(mods):
    lines = ["/* Generated by tools/build_prx.py -- do not edit. */",
             "#include <stdint.h>",
             "#include \"port_prx_table.h\"", ""]
    for m in mods:
        lines.append(f"extern const chd_func_entry chdprx_{m['ident']}_function_table[];")
        lines.append(f"extern const uint64_t chdprx_{m['ident']}_function_table_count;")
    lines += ["", "const chd_prx_module chd_prx_modules[] = {"]
    for m in mods:
        lines.append(f"    {{ \"{m['rel']}\", \"{m['ident']}.bin\", 0x{m['base']:08X}u, 0x{m['size']:X}u, "
                     f"0x{m['module_start']:08X}u, chdprx_{m['ident']}_function_table, "
                     f"&chdprx_{m['ident']}_function_table_count }},")
    lines += ["};", f"const uint32_t chd_prx_module_count = {len(mods)};", ""]
    open(os.path.join(LIFTED, "prx_table.c"), "w").write("\n".join(lines))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", help="lift only modules whose path contains this")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    a = ap.parse_args()
    for d in (WORK, IMAGES, LIFTED):
        os.makedirs(d, exist_ok=True)

    mods = find_modules()
    base = BASE0
    plan = []
    for rel, prx in mods:          # bases are assigned over ALL modules, so they are stable
        code_end_off, top = load_segments(prx)
        plan.append((rel, prx, c_ident(rel), base, code_end_off, top))
        base = (base + top + ALIGN - 1) & ~(ALIGN - 1)
        if base > BASE_END:
            sys.exit("PRX images do not fit in 0x40000000-0x50000000")
    if a.only:
        plan = [p for p in plan if a.only in p[0]]
    print(f"{len(plan)} modules, images 0x{BASE0:08X}..0x{base:08X}")

    done = []
    def one(p):
        rel, prx, ident, b, code_end_off, top = p
        m = relocate(rel, prx, ident, b)
        m["size"] = top
        shutil.copy(os.path.join(m["work"], m["stem"] + "_image.bin"), os.path.join(IMAGES, ident + ".bin"))
        m["funcs"] = lift(m, b + code_end_off)
        return m
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for m in ex.map(one, plan):
            print(f"  {m['rel']}: base 0x{m['base']:08X}, {m['funcs']} functions, "
                  f"module_start 0x{m['module_start']:08X}")
            done.append(m)
    if not a.only:
        write_table(done)
        print(f"wrote {os.path.join(LIFTED, 'prx_table.c')}")


if __name__ == "__main__":
    main()
