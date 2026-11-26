"""install.py - set up Castlevania: Harmony of Despair from the player's own PKG.

One command does every step: check the package, extract it, decrypt the game
binaries, fetch the PS3 firmware's ATRAC decoder from Sony, lift the game to
C++ and build it. Each finished step is recorded in game/install_state.json,
so running it again after an interruption resumes where it stopped.

    python tools/install.py --pkg <UP0101-NPUB30505_00-...pkg> [--rap <file.rap>] [--pup <PS3UPDAT.PUP>]

Progress goes to stdout as plain lines, plus machine-readable ones for the
launcher:
    @step <n> <total> <id> <title>
    @progress <done> <total>
    @done
    @error <message>
"""
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, time, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ps3crypt  # noqa: E402

PY = sys.executable
TOOLS = os.path.join(ROOT, "ext", "ps3recomp", "tools")
GAME = os.path.join(ROOT, "game")
BASE = os.path.join(GAME, "base")
DECRYPTED = os.path.join(GAME, "decrypted", "USRDIR")
STATE = os.path.join(GAME, "install_state.json")
KEYS = os.path.join(ROOT, "cache", "keys")
DEV_FLASH = os.path.join(ROOT, "dev_flash")
LICENSES = os.path.join(ROOT, "licenses")

TITLE_ID = "NPUB30505"
KLIC_OFFSET = 0xB67DD4                 # the game's own klicensee inside EBOOT.elf
ATRAC_PIC = "sys/external/flashATRAC.pic"
ATRAC_SHA1 = "ed2e46e9947c191e27b3b25a6be67ba1901a8810"
UPDATE_LIST = "http://fus01.ps3.update.playstation.net/update/ps3/list/us/ps3-updatelist.txt"
CODE_END = "0x9ced28"


class InstallError(Exception):
    pass


def say(msg):
    print(msg, flush=True)


def load_state():
    try:
        return json.load(open(STATE))
    except (OSError, ValueError):
        return {"done": []}


def save_state(st):
    os.makedirs(GAME, exist_ok=True)
    json.dump(st, open(STATE, "w"), indent=2)


NINJA_RE = re.compile(r"^\[(\d+)/(\d+)\]")
PRX_TOTAL_RE = re.compile(r"^(\d+) modules, images")
PRX_DONE_RE = re.compile(r"^\s+\S+\.prx: base ")


def ninja_progress(line, st):
    m = NINJA_RE.match(line)
    return (int(m.group(1)), int(m.group(2))) if m else None


def prx_progress(line, st):
    m = PRX_TOTAL_RE.match(line)
    if m:
        st["total"], st["done"] = int(m.group(1)), 0
        return 0, st["total"]
    if PRX_DONE_RE.match(line) and st.get("total"):
        st["done"] += 1
        return st["done"], st["total"]
    return None


def run(args, progress=None):
    """Run a tool, echoing its output; raise on failure. `progress(line, state)`
    turns output lines into (done, total) for @progress lines."""
    say("> " + " ".join(os.path.basename(a) if i == 0 else a for i, a in enumerate(args)))
    p = subprocess.Popen(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         text=True, encoding="utf-8", errors="replace", bufsize=1)
    st = {}
    for line in p.stdout:
        line = line.rstrip("\r\n")
        say(line)
        if progress:
            pr = progress(line, st)
            if pr:
                say(f"@progress {pr[0]} {pr[1]}")
    p.wait()
    if p.returncode != 0:
        raise InstallError(f"{os.path.basename(args[1] if args[0] == PY else args[0])} failed (exit {p.returncode})")


def sha1(path):
    h = hashlib.sha1()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


# --------------------------------------------------------------------- steps

def step_check(a):
    if not a.pkg or not os.path.isfile(a.pkg):
        raise InstallError("Choose the game's PKG file.")
    hdr = open(a.pkg, "rb").read(0x60)
    if hdr[:4] != b"\x7fPKG":
        raise InstallError("This file is not a PS3 package (PKG).")
    content_id = hdr[0x30:0x54].split(b"\0")[0].decode("ascii", "replace")
    say(f"package: {content_id}")
    if TITLE_ID not in content_id:
        raise InstallError(f"This package is not Castlevania: Harmony of Despair ({TITLE_ID}): {content_id}")
    if a.rap:
        if os.path.getsize(a.rap) != 16:
            raise InstallError("The licence file (.rap) is not valid: it should be 16 bytes.")
        os.makedirs(LICENSES, exist_ok=True)
        shutil.copyfile(a.rap, os.path.join(LICENSES, os.path.basename(a.rap)))
        say(f"licence: {os.path.basename(a.rap)}")
    else:
        say("no licence (.rap): the game will run as the trial")


def step_extract(a):
    if os.path.isdir(BASE):
        shutil.rmtree(BASE)
    run([PY, "-I", os.path.join(TOOLS, "pkg_extract.py"), a.pkg, BASE])
    if not os.path.isfile(os.path.join(BASE, "USRDIR", "EBOOT.BIN")):
        raise InstallError("The package did not contain the game (no EBOOT.BIN).")
    vfs = os.path.join(GAME, "vfs", "PS3_GAME")
    os.makedirs(os.path.dirname(vfs), exist_ok=True)
    if not os.path.exists(vfs):
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "mklink", "/J", vfs, BASE], check=True, stdout=subprocess.DEVNULL)
        else:
            os.symlink(BASE, vfs)


def step_decrypt(a):
    keys = ps3crypt.fetch_key_vault(KEYS)
    src_dir = os.path.join(BASE, "USRDIR")
    os.makedirs(DECRYPTED, exist_ok=True)
    eboot = ps3crypt.decrypt_self(open(os.path.join(src_dir, "EBOOT.BIN"), "rb").read(), keys)
    open(os.path.join(DECRYPTED, "EBOOT.elf"), "wb").write(eboot)
    klic = eboot[KLIC_OFFSET:KLIC_OFFSET + 16]
    mods = []
    for dp, _, files in os.walk(src_dir):
        mods += [os.path.join(dp, f) for f in files if f.endswith(".sprx")]
    for i, src in enumerate(sorted(mods)):
        say(f"@progress {i} {len(mods)}")
        rel = os.path.relpath(src, src_dir)
        dst = os.path.join(DECRYPTED, rel[:-5] + ".prx")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        try:
            open(dst, "wb").write(ps3crypt.decrypt_self(open(src, "rb").read(), keys, klic))
        except ValueError as e:
            raise InstallError(f"Could not decrypt {rel}: {e}. Is this the USA release, version 1.01?")
    say(f"@progress {len(mods)} {len(mods)}")
    say(f"decrypted EBOOT and {len(mods)} modules")


def step_firmware(a):
    dst = os.path.join(DEV_FLASH, *ATRAC_PIC.split("/"))
    if os.path.isfile(dst) and sha1(dst) == ATRAC_SHA1:
        say("firmware decoder already present")
        return
    keys = ps3crypt.fetch_key_vault(KEYS)
    pup = a.pup
    tmp = None
    if not pup:
        say("downloading the PS3 firmware from Sony's servers (about 200 MB)")
        with urllib.request.urlopen(UPDATE_LIST, timeout=60) as r:
            lst = r.read().decode("ascii", "replace")
        url = next((part[4:] for line in lst.splitlines() for part in line.split(";")
                    if part.startswith("CDN=") and part.endswith("PS3UPDAT.PUP")), None)
        if not url:
            raise InstallError("Sony's update list has no firmware link. Choose a PS3UPDAT.PUP you already have.")
        tmp = pup = os.path.join(ROOT, "cache", "PS3UPDAT.PUP")
        os.makedirs(os.path.dirname(pup), exist_ok=True)
        with urllib.request.urlopen(url, timeout=120) as r, open(pup + ".part", "wb") as f:
            total = int(r.headers.get("Content-Length") or 0)
            got = 0
            last = 0
            while True:
                b = r.read(1 << 20)
                if not b:
                    break
                f.write(b)
                got += len(b)
                if time.time() - last > 0.5:
                    say(f"@progress {got} {total}")
                    last = time.time()
        os.replace(pup + ".part", pup)
    found = ps3crypt.pup_extract(pup, keys, [ATRAC_PIC])
    blob = found.get(ATRAC_PIC)
    if blob is None or hashlib.sha1(blob).hexdigest() != ATRAC_SHA1:
        raise InstallError("The firmware file did not contain the expected ATRAC decoder.")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(blob)
    if tmp:
        os.remove(tmp)
    say("firmware decoder installed")


def apply_patches():
    """Apply patches/ps3recomp/*.patch to a clean submodule (as build.bat does).
    The lifters need them too, not only the build: lift_prx.py's relocations
    are part of the patch. A submodule with local changes is taken as already
    patched."""
    sub = os.path.join(ROOT, "ext", "ps3recomp")
    if subprocess.run(["git", "-C", sub, "diff", "--quiet"]).returncode != 0:
        return
    pdir = os.path.join(ROOT, "patches", "ps3recomp")
    for p in sorted(os.listdir(pdir)):
        if p.endswith(".patch"):
            say(f"applying {p}")
            if subprocess.run(["git", "-C", sub, "apply", os.path.join(pdir, p)]).returncode != 0:
                raise InstallError(f"Could not apply {p} to ext/ps3recomp.")


def step_lift_eboot(a):
    apply_patches()
    elf = os.path.join(DECRYPTED, "EBOOT.elf")
    run([PY, os.path.join(TOOLS, "ppu_loader.py"), elf, "-o", "out" + os.sep])
    run([PY, os.path.join(TOOLS, "ppu_lifter.py"), elf,
         "--functions", os.path.join("out", "EBOOT.functions.json"),
         "--hle-stubs", os.path.join("out", "EBOOT.imports.json"),
         "--code-end", CODE_END, "--chunks", "32", "-o", "recompiled" + os.sep])
    run([PY, os.path.join(TOOLS, "gen_hle_nids.py"), "--all", "--out", os.path.join("recompiled", "ppu_hle_nids.cpp")])


def step_lift_prx(a):
    apply_patches()
    run([PY, "-u", os.path.join(ROOT, "tools", "build_prx.py"), "--jobs", str(os.cpu_count() or 4)], prx_progress)


def step_lift_spu(a):
    run([PY, os.path.join(TOOLS, "extract_spu_images.py"), os.path.join(DECRYPTED, "EBOOT.elf"), "--output", "spu_images"])
    run([PY, os.path.join(TOOLS, "build_spu_workloads.py"), "--images", "spu_images", "--lifted", "spu",
         "--out", os.path.join("spu", "spu_workloads.c"), "--constructor"])


def step_build(a):
    if os.name != "nt":
        raise InstallError("Building is only supported on Windows for now.")
    run(["cmd", "/c", os.path.join(ROOT, "build.bat")], ninja_progress)
    if not os.path.isfile(os.path.join(ROOT, "build", "CHDPS3Recomp.exe")):
        raise InstallError("The build finished without producing the game.")


STEPS = [
    ("check",    "Checking the game files",          step_check),
    ("extract",  "Extracting the package",           step_extract),
    ("decrypt",  "Decrypting the game",              step_decrypt),
    ("firmware", "Getting the PS3 firmware decoder", step_firmware),
    ("eboot",    "Translating the game code",        step_lift_eboot),
    ("prx",      "Translating the game modules",     step_lift_prx),
    ("spu",      "Translating the audio program",    step_lift_spu),
    ("build",    "Compiling (this is the long one)", step_build),
]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pkg", help="the game's PKG (UP0101-NPUB30505_00-...)")
    ap.add_argument("--rap", help="licence for the full version (.rap), optional")
    ap.add_argument("--pup", help="a PS3UPDAT.PUP you already have, instead of downloading it")
    ap.add_argument("--restart", action="store_true", help="redo every step")
    a = ap.parse_args()

    st = {"done": []} if a.restart else load_state()
    # A different package invalidates everything after the check.
    if a.pkg:
        ident = f"{os.path.getsize(a.pkg)}:{os.path.basename(a.pkg)}" if os.path.isfile(a.pkg) else a.pkg
        if st.get("pkg") != ident:
            st = {"done": [], "pkg": ident}
    elif "check" not in st["done"]:
        ap.error("--pkg is required for a new installation")

    t0 = time.time()
    try:
        for n, (sid, title, fn) in enumerate(STEPS, 1):
            say(f"@step {n} {len(STEPS)} {sid} {title}")
            if sid in st["done"] and sid != "check":
                say("already done")
                continue
            if sid == "check" and not a.pkg:
                continue
            fn(a)
            if sid not in st["done"]:
                st["done"].append(sid)
            save_state(st)
    except InstallError as e:
        say(f"@error {e}")
        return 1
    except Exception as e:  # unexpected: keep the traceback for the log
        import traceback
        traceback.print_exc()
        say(f"@error Unexpected error: {e}")
        return 1
    say(f"installed in {int(time.time() - t0) // 60} min")
    say("@done")
    return 0


if __name__ == "__main__":
    sys.exit(main())
