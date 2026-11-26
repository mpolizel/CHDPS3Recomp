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
import argparse, hashlib, json, os, re, shutil, struct, subprocess, sys, time, urllib.error, urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import ps3crypt  # noqa: E402

# Under pythonw the tools still run on the console python.exe beside it (with
# their window hidden), so their output reaches the pipe and the log.
PY = sys.executable
if os.path.basename(PY).lower() == "pythonw.exe" and os.path.exists(os.path.join(os.path.dirname(PY), "python.exe")):
    PY = os.path.join(os.path.dirname(PY), "python.exe")
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
TOOLCHAIN_DIR = os.path.join(ROOT, "cache", "toolchain")
BUILD_DIR = os.path.join(ROOT, "build-mingw")
GAME_EXE = os.path.join(BUILD_DIR, "CHDPS3Recomp.exe")
PATCH_MARK = os.path.join(ROOT, "ext", "ps3recomp", ".chd-patched")
# Portable build tools, pinned and checked by SHA-256. Nothing gets installed:
# they are unpacked under cache/toolchain and only put on PATH for the build.
TOOLCHAIN = [
    ("llvm-mingw-20261117-ucrt-x86_64",
     "https://github.com/mstorsjo/llvm-mingw/releases/download/20261117/llvm-mingw-20261117-ucrt-x86_64.zip",
     "4d002e330a0f703625aa5ccbc37e96f8f824413e163bfc7336774aabd2c3a412", "bin"),
    ("cmake-4.5.1-windows-x86_64",
     "https://github.com/Kitware/CMake/releases/download/v4.5.1/cmake-4.5.1-windows-x86_64.zip",
     "bfe6eadb245dc8213e0d3feea316b520cb2aae92f5667ffd76059ba6b05c430c", "bin"),
    ("ninja",
     "https://github.com/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
     "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65", ""),
]


# Exit codes, which recomplauncher.json maps to messages for the player.
EXIT_FAILED, EXIT_WRONG_GAME, EXIT_DOWNLOAD, EXIT_BUILD, EXIT_LICENCE = 1, 3, 4, 5, 6


class InstallError(Exception):
    def __init__(self, msg, code=EXIT_FAILED):
        super().__init__(msg)
        self.code = code


# Nothing opens a window: under pythonw there is no console at all, and every
# tool is started with CREATE_NO_WINDOW (its own children share that hidden
# console). So all output also goes to out/install.log.
NO_WINDOW = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
LOG_PATH = os.path.join(ROOT, "out", "install.log")
_log = None


def say(msg):
    global _log
    if _log is None:
        os.makedirs(os.path.dirname(LOG_PATH), exist_ok=True)
        _log = open(LOG_PATH, "a", encoding="utf-8", buffering=1)
    print(msg, file=_log)
    if sys.stdout is not None:          # None under pythonw with no pipe attached
        try:
            print(msg, flush=True)
        except (OSError, ValueError):
            pass


def load_state():
    try:
        return json.load(open(STATE, encoding="utf-8-sig"))   # a BOM from an editor is fine
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


def run(args, progress=None, env=None):
    """Run a tool, echoing its output; raise on failure. `progress(line, state)`
    turns output lines into (done, total) for @progress lines."""
    say("> " + " ".join(os.path.basename(a) if i == 0 else a for i, a in enumerate(args)))
    if env is not None and not os.path.isabs(args[0]):
        args = [shutil.which(args[0], path=env["PATH"]) or args[0]] + list(args[1:])
    p = subprocess.Popen(args, cwd=ROOT, env=env, creationflags=NO_WINDOW, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
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


def download(url, dst):
    """Download url to dst with @progress lines (via dst.part, so a
    half-finished download is never taken for a whole one)."""
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with urllib.request.urlopen(url, timeout=120) as r, open(dst + ".part", "wb") as f:
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
    say(f"@progress {got} {total or got}")
    os.replace(dst + ".part", dst)


def sha256(path):
    h = hashlib.sha256()
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
        raise InstallError("This file is not a PS3 package (PKG).", EXIT_WRONG_GAME)
    content_id = hdr[0x30:0x54].split(b"\0")[0].decode("ascii", "replace")
    say(f"package: {content_id}")
    if TITLE_ID not in content_id:
        raise InstallError(f"This package is not Castlevania: Harmony of Despair ({TITLE_ID}): {content_id}", EXIT_WRONG_GAME)
    if a.rap:
        if os.path.getsize(a.rap) != 16:
            raise InstallError("The licence file (.rap) is not valid: it should be 16 bytes.", EXIT_LICENCE)
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
        raise InstallError("The package did not contain the game (no EBOOT.BIN).", EXIT_WRONG_GAME)
    for cid in overlay_dlcs():
        say(f"DLC {cid} put back")
    vfs = os.path.join(GAME, "vfs", "PS3_GAME")
    os.makedirs(os.path.dirname(vfs), exist_ok=True)
    if not os.path.exists(vfs):
        if os.name == "nt":
            subprocess.run(["cmd", "/c", "mklink", "/J", vfs, BASE], check=True, stdout=subprocess.DEVNULL, creationflags=NO_WINDOW)
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
            raise InstallError(f"Could not decrypt {rel}: {e}. Is this the USA release, version 1.01?", EXIT_WRONG_GAME)
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
            raise InstallError("Sony's update list has no firmware link. Choose a PS3UPDAT.PUP you already have.", EXIT_DOWNLOAD)
        tmp = pup = os.path.join(ROOT, "cache", "PS3UPDAT.PUP")
        download(url, pup)
    found = ps3crypt.pup_extract(pup, keys, [ATRAC_PIC])
    blob = found.get(ATRAC_PIC)
    if blob is None or hashlib.sha1(blob).hexdigest() != ATRAC_SHA1:
        raise InstallError("The firmware file did not contain the expected ATRAC decoder.")
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    open(dst, "wb").write(blob)
    if tmp:
        os.remove(tmp)
    say("firmware decoder installed")


# The build's inputs: the port, the runtime and the achievements SDK (not the
# translated game, which only the installer itself writes).
SOURCE_ROOTS = ["CMakeLists.txt", "main.cpp", "stubs.cpp", "port_extension.cpp", "port_extension.h",
                "guest_arena.h", "config.toml", "cmake", "src", "ext/ps3recomp/CMakeLists.txt",
                "ext/ps3recomp/cmake", "ext/ps3recomp/include", "ext/ps3recomp/runtime",
                "ext/ps3recomp/libs", "ext/rl-achievements/sdk"]
SOURCE_HASHES = os.path.join(GAME, "source_hashes.json")
SOURCE_EXT = (".c", ".cpp", ".h", ".hpp", ".inc", ".txt", ".cmake", ".toml")


def _source_files():
    for r in SOURCE_ROOTS:
        full = os.path.join(ROOT, r)
        if os.path.isfile(full):
            yield r.replace("\\", "/"), full
        elif os.path.isdir(full):
            for dp, _, fs in os.walk(full):
                for f in fs:
                    if f.lower().endswith(SOURCE_EXT):
                        p = os.path.join(dp, f)
                        yield os.path.relpath(p, ROOT).replace(os.sep, "/"), p


def mark_changed_sources():
    """Touch every build input whose content differs from the last build's, so
    an update's files are rebuilt whatever their dates. With no record of the
    last build (an install from before 0.0.4), touch every source and build
    file but not the headers: those would rebuild the whole translated game,
    and a header change always comes with the sources that use it."""
    try:
        old = json.load(open(SOURCE_HASHES, encoding="utf-8"))
    except (OSError, ValueError):
        old = None
    new, changed = {}, 0
    for rel, full in _source_files():
        h = hashlib.sha1(open(full, "rb").read()).hexdigest()
        new[rel] = h
        if old is None:
            touch = not rel.lower().endswith((".h", ".hpp", ".inc"))
        else:
            touch = old.get(rel) != h
        if touch:
            os.utime(full, None)
            changed += 1
    os.makedirs(GAME, exist_ok=True)
    json.dump(new, open(SOURCE_HASHES, "w", encoding="utf-8"), indent=0)
    return changed


def apply_patches():
    """Apply patches/ps3recomp/*.patch to a clean submodule (as build.bat does).
    The lifters need them too, not only the build: lift_prx.py's relocations
    are part of the patch. A submodule with local changes is taken as already
    patched."""
    sub = os.path.join(ROOT, "ext", "ps3recomp")
    if os.path.exists(PATCH_MARK):          # a release package ships it patched
        return
    if shutil.which("git") is None:
        raise InstallError("ext/ps3recomp is not patched and Git is not available to patch it.")
    if subprocess.run(["git", "-C", sub, "diff", "--quiet"], creationflags=NO_WINDOW).returncode != 0:
        return
    pdir = os.path.join(ROOT, "patches", "ps3recomp")
    for p in sorted(os.listdir(pdir)):
        if p.endswith(".patch"):
            say(f"applying {p}")
            if subprocess.run(["git", "-C", sub, "apply", os.path.join(pdir, p)], creationflags=NO_WINDOW).returncode != 0:
                raise InstallError(f"Could not apply {p} to ext/ps3recomp.")
    with open(PATCH_MARK, "w") as f:
        print("patches/ps3recomp applied", file=f)


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


def step_toolchain(a):
    for name, url, digest, _ in TOOLCHAIN:
        if os.path.isdir(os.path.join(TOOLCHAIN_DIR, name)):
            continue
        say(f"downloading {os.path.basename(url)}")
        zpath = os.path.join(TOOLCHAIN_DIR, os.path.basename(url))
        download(url, zpath)
        if sha256(zpath) != digest:
            os.remove(zpath)
            raise InstallError(f"{os.path.basename(url)} did not download correctly (checksum mismatch). Try again.", EXIT_DOWNLOAD)
        import zipfile
        dest = TOOLCHAIN_DIR if name != "ninja" else os.path.join(TOOLCHAIN_DIR, name)
        with zipfile.ZipFile(zpath) as z:
            z.extractall(dest)
        os.remove(zpath)


def toolchain_env():
    env = dict(os.environ)
    for k in ("INCLUDE", "LIB", "LIBPATH", "CC", "CXX"):
        env.pop(k, None)
    paths = [os.path.join(TOOLCHAIN_DIR, name, sub) for name, _, _, sub in TOOLCHAIN]
    sysroot = os.environ.get("SystemRoot", r"C:\Windows")
    env["PATH"] = os.pathsep.join(paths + [os.path.join(sysroot, "System32"), sysroot])
    return env


def step_build(a):
    if os.name != "nt":
        raise InstallError("Building is only supported on Windows for now.")
    apply_patches()
    env = toolchain_env()
    if not os.path.isfile(os.path.join(BUILD_DIR, "build.ninja")):
        run(["cmake", "-S", ROOT, "-B", BUILD_DIR, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
             "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++"], env=env)
    try:
        run(["cmake", "--build", BUILD_DIR], ninja_progress, env=env)
    except InstallError as e:
        raise InstallError(str(e), EXIT_BUILD)
    if not os.path.isfile(GAME_EXE):
        raise InstallError("The build finished without producing the game.", EXIT_BUILD)


STEPS = [
    ("check",    "Checking the game files",          step_check),
    ("extract",  "Extracting the package",           step_extract),
    ("decrypt",  "Decrypting the game",              step_decrypt),
    ("firmware", "Getting the PS3 firmware decoder", step_firmware),
    ("eboot",    "Translating the game code",        step_lift_eboot),
    ("prx",      "Translating the game modules",     step_lift_prx),
    ("spu",      "Translating the audio program",    step_lift_spu),
    ("toolchain", "Getting the compiler",            step_toolchain),
    ("build",    "Compiling (this is the long one)", step_build),
]


# ----------------------------------------------------------------------- DLC
#
# A DLC is its PSN package plus the player's .rap, usually zipped together.
# Most of the DLC content is already in the base package; what a DLC adds is
# its key.edat (and for the stages, the stage's data and music), encrypted for
# its content ID. The runtime decrypts those EDATs with a key derived from the
# .rap, written beside each one as <file>.edat.rifkey (libs/filesystem/edat.c).
# The DLC's files are kept in dlc/<content id>/ and laid over game/base, so a
# reinstall of the game puts them back.

DLC_DIR = os.path.join(ROOT, "dlc")
DLC_PREFIX = "UP0101-NPUB30505_00-"


def _pkg_content_id(path):
    with open(path, "rb") as f:
        hdr = f.read(0x60)
    if hdr[:4] != b"\x7fPKG":
        return None
    return hdr[0x30:0x54].split(b"\0")[0].decode("ascii", "replace")


def overlay_dlcs():
    """Lay every installed DLC's files over game/base."""
    if not os.path.isdir(DLC_DIR) or not os.path.isdir(BASE):
        return []
    done = []
    for cid in sorted(os.listdir(DLC_DIR)):
        src = os.path.join(DLC_DIR, cid, "USRDIR")
        if not os.path.isdir(src):
            continue
        for dp, _, files in os.walk(src):
            for f in files:
                s = os.path.join(dp, f)
                d = os.path.join(BASE, "USRDIR", os.path.relpath(s, src))
                os.makedirs(os.path.dirname(d), exist_ok=True)
                shutil.copy2(s, d)
                if os.path.exists(d + ".dec"):        # a stale decrypted copy
                    os.remove(d + ".dec")
        done.append(cid)
    return done


def add_dlc(path, rap_arg=None):
    """Install a DLC from a zip (PKG + .rap), a PKG (with --rap, or its .rap
    beside it) or a folder holding both."""
    import tempfile, zipfile
    if not path or not os.path.exists(path):
        raise InstallError("Choose the DLC's zip (its .pkg and its .rap).")
    if "build" not in load_state().get("done", []):
        raise InstallError("Install the game first, then add DLC.")
    keys = ps3crypt.fetch_key_vault(KEYS)
    tmp = tempfile.mkdtemp(prefix="chd-dlc-", dir=os.path.join(ROOT, "cache"))
    try:
        pkgs, raps = [], []
        if zipfile.is_zipfile(path):
            with zipfile.ZipFile(path) as z:
                for m in z.infolist():
                    name = os.path.basename(m.filename)
                    if name.lower().endswith((".pkg", ".rap")) and not m.is_dir():
                        out = os.path.join(tmp, name)
                        with z.open(m) as zs, open(out, "wb") as fo:
                            shutil.copyfileobj(zs, fo)
                        (pkgs if name.lower().endswith(".pkg") else raps).append(out)
        elif os.path.isdir(path):
            for f in os.listdir(path):
                full = os.path.join(path, f)
                if f.lower().endswith(".pkg"): pkgs.append(full)
                if f.lower().endswith(".rap"): raps.append(full)
        else:
            pkgs.append(path)
            folder = os.path.dirname(os.path.abspath(path))
            raps += [os.path.join(folder, f) for f in os.listdir(folder) if f.lower().endswith(".rap")]
        if rap_arg:
            raps.append(rap_arg)
        if not pkgs:
            raise InstallError("There is no DLC package (.pkg) in what you chose.", EXIT_WRONG_GAME)

        installed = []
        for pkg in pkgs:
            cid = _pkg_content_id(pkg)
            if not cid or not cid.startswith(DLC_PREFIX) or cid.endswith("CASTHARMONYOFDES"):
                raise InstallError(f"{os.path.basename(pkg)} is not a DLC of Castlevania: Harmony of Despair (USA).", EXIT_WRONG_GAME)
            rap = next((r for r in raps if os.path.basename(r).lower() == (cid + ".rap").lower()), None)
            if not rap:
                raise InstallError(f"The licence for this DLC is missing: {cid}.rap. Put it in the zip with the package.", EXIT_LICENCE)
            rap_bytes = open(rap, "rb").read()
            if len(rap_bytes) != 16:
                raise InstallError(f"{os.path.basename(rap)} is not a valid licence (a .rap is 16 bytes).", EXIT_LICENCE)
            say(f"DLC {cid}")
            out = os.path.join(tmp, cid)
            run([PY, "-I", os.path.join(TOOLS, "pkg_extract.py"), pkg, out])
            usr = os.path.join(out, "USRDIR")
            if not os.path.isdir(usr):
                raise InstallError(f"{cid} has no game files: it is not a playable DLC (an avatar or theme?).", EXIT_WRONG_GAME)
            rif = ps3crypt.rap_to_rif(rap_bytes, keys)
            new_code = []
            dest = os.path.join(DLC_DIR, cid)
            if os.path.isdir(dest):
                shutil.rmtree(dest)
            for dp, _, files in os.walk(usr):
                for f in files:
                    s = os.path.join(dp, f)
                    rel = os.path.relpath(s, usr)
                    d = os.path.join(dest, "USRDIR", rel)
                    os.makedirs(os.path.dirname(d), exist_ok=True)
                    shutil.copy2(s, d)
                    if f.lower().endswith(".edat") and ps3crypt.npd_content_id(s) == cid:
                        with open(d + ".rifkey", "wb") as k:
                            k.write(rif)
                    if f.lower().endswith(".sprx"):
                        base = os.path.join(BASE, "USRDIR", rel)
                        if not os.path.exists(base) or open(base, "rb").read() != open(s, "rb").read():
                            new_code.append(rel)
            os.makedirs(LICENSES, exist_ok=True)
            shutil.copyfile(rap, os.path.join(LICENSES, cid + ".rap"))
            installed.append((cid, new_code))
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    overlay_dlcs()
    code = [c for _, cs in installed for c in cs]
    if code:
        # New game code: decrypt, translate and rebuild it like the game's own.
        say(f"the DLC brings {len(code)} new code module(s): translating and building them")
        st = load_state()
        st["done"] = [s for s in st["done"] if s not in ("decrypt", "prx", "build")]
        save_state(st)
        redo = [(sid, title, fn) for sid, title, fn in STEPS if sid in ("decrypt", "prx", "build")]
        for n, (sid, title, fn) in enumerate(redo, 1):
            if True:
                say(f"@step {n} {len(redo)} {sid} {title}")
                fn(argparse.Namespace(pkg=None, rap=None, pup=None))
                st["done"].append(sid)
                save_state(st)
    for cid, _ in installed:
        say(f"installed DLC {cid}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pkg", help="the game's PKG (UP0101-NPUB30505_00-...)")
    ap.add_argument("--rap", help="licence for the full version (.rap), optional")
    ap.add_argument("--pup", help="a PS3UPDAT.PUP you already have, instead of downloading it")
    ap.add_argument("--restart", action="store_true", help="redo every step")
    ap.add_argument("--dlc", help="add a DLC: its zip (.pkg + .rap), its .pkg, or a folder with both")
    a = ap.parse_args()

    if a.dlc:
        try:
            say("@step 1 1 dlc Adding the DLC")
            add_dlc(a.dlc, a.rap)
        except InstallError as e:
            say(f"@error {e}")
            return e.code
        except (urllib.error.URLError, TimeoutError) as e:
            say(f"@error Download failed: {e}. Check your internet connection and try again.")
            return EXIT_DOWNLOAD
        say("@done")
        return 0

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
        # A new version unzipped over an installed one: its files keep the
        # zip's dates, which can be older than what the previous version built,
        # so neither Ninja nor CMake would see them changed. Compare contents
        # with what was built last time and mark exactly the changed files
        # (mark_changed_sources); the build step always runs (seconds when
        # there is nothing to do).
        version = open(os.path.join(ROOT, "version.txt")).read().strip()
        if st.get("version") != version:
            n = mark_changed_sources()
            say(f"version {version}: {n} changed source files to rebuild")
            st["version"] = version
            save_state(st)
        for n, (sid, title, fn) in enumerate(STEPS, 1):
            say(f"@step {n} {len(STEPS)} {sid} {title}")
            if sid in st["done"] and sid not in ("check", "build"):
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
        return e.code
    except (urllib.error.URLError, TimeoutError) as e:  # no connection, server down
        say(f"@error Download failed: {e}. Check your internet connection and try again.")
        return EXIT_DOWNLOAD
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
