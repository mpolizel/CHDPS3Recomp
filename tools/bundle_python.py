"""bundle_python.py - build the portable Python the installer runs on.

Makes python/ at the project root from python.org's official "embeddable"
Windows build plus the packages the installer and the ps3recomp tools import.
A release package ships that folder, so the player needs no Python installed,
and its pythonw.exe runs the installer without a console window.

    python tools/bundle_python.py

Run it with any Python 3 that has pip (for the release build); the packages
are fetched as wheels for the bundled version, not the one running this.
"""
import hashlib, os, shutil, subprocess, sys, urllib.request, zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEST = os.path.join(ROOT, "python")
VERSION = "3.14.10"
URL = f"https://www.python.org/ftp/python/{VERSION}/python-{VERSION}-embed-amd64.zip"
SHA256 = "90905e64a840557d8c1bc4f5b74db3d7ee85e3ffb217611cac1c7813acea4159"
REQUIREMENTS = os.path.join(ROOT, "ext", "ps3recomp", "tools", "requirements.txt")


def main():
    if os.path.isdir(DEST):
        shutil.rmtree(DEST)
    os.makedirs(DEST)
    zpath = DEST + ".zip"
    print(f"downloading {URL}")
    with urllib.request.urlopen(URL, timeout=120) as r, open(zpath, "wb") as f:
        shutil.copyfileobj(r, f)
    digest = hashlib.sha256(open(zpath, "rb").read()).hexdigest()
    if digest != SHA256:
        os.remove(zpath)
        sys.exit(f"checksum mismatch: {digest}")
    with zipfile.ZipFile(zpath) as z:
        z.extractall(DEST)
    os.remove(zpath)

    # The ._pth file pins sys.path to the zip and stops the script's own folder
    # being added, which the ps3recomp tools rely on to import their siblings.
    # Without it the interpreter computes the usual paths from its folder.
    for f in os.listdir(DEST):
        if f.endswith("._pth"):
            os.remove(os.path.join(DEST, f))

    site = os.path.join(DEST, "Lib", "site-packages")
    os.makedirs(site, exist_ok=True)
    tag = "".join(VERSION.split(".")[:2])
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--disable-pip-version-check",
                    "--target", site, "--only-binary=:all:", "--platform", "win_amd64",
                    "--python-version", tag, "--implementation", "cp",
                    "-r", REQUIREMENTS], check=True)
    out = subprocess.run([os.path.join(DEST, "python.exe"), "-c",
                          "import sys, Crypto, capstone, construct; print(sys.version.split()[0], 'ok')"],
                         capture_output=True, text=True)
    print(out.stdout.strip() or out.stderr.strip())
    if out.returncode != 0:
        sys.exit("the bundled Python cannot import the tools' packages")


if __name__ == "__main__":
    main()
