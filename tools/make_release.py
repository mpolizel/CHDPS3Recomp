"""make_release.py - build the player's release package.

    python tools/make_release.py

Writes dist/CHDPS3Recomp-v<version>-windows-x64.zip (version from version.txt).
The player unzips it, opens CHDLauncher.exe, picks the game's PKG (and the
.rap for the full game) and installs. It holds:

  CHDLauncher.exe       the launcher (build_launcher.bat), no game code
  python/               the portable Python the installer runs on (tools/bundle_python.py)
  ext/ps3recomp/        ps3recomp's tracked files with patches/ps3recomp applied, marked
                        .chd-patched, so the installer needs no Git
  the port's sources, tools/install.py, ps3crypt.py and build_prx.py, the docs,
  the licences and the launcher manifests

and no game data, keys or generated code: the package is checked for those
before it is zipped, and the script stops if it finds any.
"""
import hashlib, os, re, shutil, subprocess, sys, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(ROOT, "dist")

# The port's own files that go in (development-only tools stay out).
PORT_FILES = [
    "CMakeLists.txt", "cmake/PortExtensions.cmake", "main.cpp", "stubs.cpp",
    "port_extension.cpp", "port_extension.h", "guest_arena.h", "config.toml",
    "install.bat", "version.txt", "README.md", "LICENSE",
    "recomplauncher.json", ".recomp.json",
    "tools/install.py", "tools/ps3crypt.py", "tools/build_prx.py",
]
PORT_DIRS = ["src"]

# Never in a package: the player's own copy and anything derived from it.
FORBIDDEN_EXT = {".pkg", ".rap", ".edat", ".sdat", ".elf", ".self", ".sprx", ".prx", ".pup", ".pic", ".bin"}
FORBIDDEN_NAMES = {"key_vault.cpp", "key_vault.h", "eboot.bin", "param.sfo", "launcher.ini", "config.ini"}
ALLOWED_BIN = {"python"}            # .bin files are fine inside the bundled Python's packages


def run(*args, **kw):
    return subprocess.run(args, cwd=ROOT, check=True, capture_output=True, text=True, **kw).stdout


def tracked(path):
    return [l for l in run("git", "-C", path, "ls-files").splitlines() if l]


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def game_klicensee():
    """The game's own klicensee from the local EBOOT, if this PC has it, to make
    sure it appears nowhere in the package."""
    elf = os.path.join(ROOT, "game", "decrypted", "USRDIR", "EBOOT.elf")
    if not os.path.isfile(elf):
        return None
    with open(elf, "rb") as f:
        f.seek(0xB67DD4)
        return f.read(16)


def audit(stage):
    klic = game_klicensee()
    needles = []
    if klic:
        needles = [klic, klic.hex().encode(), klic.hex().upper().encode()]
    problems = []
    for dp, _, files in os.walk(stage):
        rel_dir = os.path.relpath(dp, stage).replace("\\", "/")
        for f in files:
            rel = f"{rel_dir}/{f}" if rel_dir != "." else f
            ext = os.path.splitext(f)[1].lower()
            top = rel.split("/")[0]
            if f.lower() in FORBIDDEN_NAMES or (ext in FORBIDDEN_EXT and top not in ALLOWED_BIN):
                problems.append(f"game or key file: {rel}")
                continue
            if needles and os.path.getsize(os.path.join(dp, f)) < 64 << 20:
                data = open(os.path.join(dp, f), "rb").read()
                if any(n in data for n in needles):
                    problems.append(f"contains the game's klicensee: {rel}")
    return problems


def main():
    version = open(os.path.join(ROOT, "version.txt")).read().strip()
    if not re.fullmatch(r"\d\.\d\.\d", version):
        sys.exit(f"version.txt should read like 0.0.1, not {version!r}")
    name = f"CHDPS3Recomp-v{version}-windows-x64"

    launcher = os.path.join(ROOT, "build-launcher", "CHDLauncher.exe")
    if not os.path.isfile(launcher):
        sys.exit("CHDLauncher.exe is not built: run build_launcher.bat first")
    if not os.path.isfile(os.path.join(ROOT, "python", "pythonw.exe")):
        print("bundling Python")
        subprocess.run([sys.executable, os.path.join(ROOT, "tools", "bundle_python.py")], check=True)
    if run("git", "status", "--porcelain", "--untracked-files=no", "--", ".", ":!ext/ps3recomp").strip():
        print("warning: the working tree has uncommitted changes; they go into the package")

    stage = os.path.join(DIST, name)
    if os.path.isdir(stage):
        shutil.rmtree(stage)
    os.makedirs(stage)

    for f in PORT_FILES:
        copy(os.path.join(ROOT, f), os.path.join(stage, f))
    for d in PORT_DIRS:
        for f in tracked(ROOT):
            if f.startswith(d + "/"):
                copy(os.path.join(ROOT, f), os.path.join(stage, f))

    # ps3recomp: its tracked files as they are in the working copy, which is the
    # pinned commit with our patch applied (checked: every patched file differs).
    sub = os.path.join(ROOT, "ext", "ps3recomp")
    patched = [l[3:] for l in run("git", "-C", sub, "status", "--porcelain").splitlines() if l.startswith(" M")]
    expected = sorted(set(re.findall(r"^diff --git a/(\S+)", open(os.path.join(
        ROOT, "patches", "ps3recomp", "0001-chd-resc-present-and-x16.patch"), encoding="utf-8").read(), re.M)))
    if sorted(patched) != expected:
        sys.exit("ext/ps3recomp does not match patches/ps3recomp exactly; regenerate the patch first")
    for f in tracked(sub):
        if f.startswith("scratch/"):
            continue
        copy(os.path.join(sub, f), os.path.join(stage, "ext", "ps3recomp", f))
    with open(os.path.join(stage, "ext", "ps3recomp", ".chd-patched"), "w") as f:
        print("patches/ps3recomp applied", file=f)

    # Recomp Achievements SDK (its licence and the rcheevos/cJSON notices come
    # with its tracked files, as its licence asks).
    rla = os.path.join(ROOT, "ext", "rl-achievements")
    for f in tracked(rla):
        copy(os.path.join(rla, f), os.path.join(stage, "ext", "rl-achievements", f))

    copy(launcher, os.path.join(stage, "CHDLauncher.exe"))
    shutil.copytree(os.path.join(ROOT, "python"), os.path.join(stage, "python"),
                    ignore=shutil.ignore_patterns("__pycache__"))

    problems = audit(stage)
    if problems:
        print("\n".join(problems))
        sys.exit("refusing to package: game data or keys found")

    zpath = os.path.join(DIST, name + ".zip")
    if os.path.exists(zpath):
        os.remove(zpath)
    count = 0
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as z:
        for dp, _, files in os.walk(stage):
            for f in sorted(files):
                full = os.path.join(dp, f)
                z.write(full, os.path.join(name, os.path.relpath(full, stage)))
                count += 1
    digest = hashlib.sha256(open(zpath, "rb").read()).hexdigest()
    print(f"{zpath}\n  {count} files, {os.path.getsize(zpath) / 1048576:.1f} MB\n  sha256 {digest}")


if __name__ == "__main__":
    main()
