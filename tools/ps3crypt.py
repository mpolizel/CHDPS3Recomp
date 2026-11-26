"""ps3crypt.py - decrypt PS3 SELF/SPRX files and extract files from a firmware PUP.

This replaces `rpcs3.exe --decrypt` and RPCS3's "Install Firmware" for the
installer. The repository ships no keys: they are read from RPCS3's public
source files rpcs3/Crypto/key_vault.cpp and key_vault.h, which the installer
downloads on the player's machine (see `fetch_key_vault`).

    python ps3crypt.py self <in.self> <out.elf> --keys <dir> [--klic <hex>]
    python ps3crypt.py pup  <PS3UPDAT.PUP> <out dir> --keys <dir> <dev_flash path>...

SELF layout and algorithm follow RPCS3's rpcs3/Crypto/unself.cpp:
  SCE header (0x20) -> extended header -> metadata info (0x40, AES-256-CBC with
  the revision's NPDRM key, under an NPDRM AES-128-CBC layer keyed by the
  klicensee) -> metadata header, section headers and data keys (AES-128-CTR)
  -> each encrypted section (AES-128-CTR, optionally zlib) at its program
  header's file offset.
"""
import argparse, io, os, re, struct, sys, tarfile, urllib.request, zlib
from Crypto.Cipher import AES
from Crypto.Util import Counter

KEY_VAULT_URL = "https://raw.githubusercontent.com/RPCS3/rpcs3/master/rpcs3/Crypto/"


class Keys:
    def __init__(self, key_dir):
        h = open(os.path.join(key_dir, "key_vault.h"), encoding="utf-8").read()
        cpp = open(os.path.join(key_dir, "key_vault.cpp"), encoding="utf-8").read()

        def arr(name):
            m = re.search(name + r"\[0x[0-9A-Fa-f]+\]\s*=\s*\{([^}]*)\}", h)
            if not m:
                raise ValueError(f"{name} not found in key_vault.h")
            return bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))

        self.klic_free = arr("NP_KLIC_FREE")
        self.klic_key = arr("NP_KLIC_KEY")
        self.scepkg_erk = arr("SCEPKG_ERK")
        self.scepkg_riv = arr("SCEPKG_RIV")
        self.rap_key = arr("RAP_KEY")
        self.rap_pbox = arr("RAP_PBOX")
        self.rap_e1 = arr("RAP_E1")
        self.rap_e2 = arr("RAP_E2")
        body = cpp[cpp.index("void KeyVault::LoadSelfNPDRMKeys()"):]
        body = body[:body.index("\n}\n")]
        self.npdrm = {}
        for rev, erk, riv in re.findall(
                r'emplace_back\(\s*0x[0-9A-Fa-f]+,\s*0x[0-9A-Fa-f]+,\s*0x([0-9A-Fa-f]+),\s*KEY_NPDRM,'
                r'\s*"([0-9A-Fa-f]+)",\s*"([0-9A-Fa-f]+)"', body):
            self.npdrm[int(rev, 16)] = (bytes.fromhex(erk), bytes.fromhex(riv))
        if not self.npdrm:
            raise ValueError("no NPDRM keys found in key_vault.cpp")


def fetch_key_vault(key_dir):
    """Download RPCS3's key_vault.cpp/.h into key_dir unless already there."""
    os.makedirs(key_dir, exist_ok=True)
    for name in ("key_vault.cpp", "key_vault.h"):
        path = os.path.join(key_dir, name)
        if not os.path.exists(path):
            with urllib.request.urlopen(KEY_VAULT_URL + name, timeout=60) as r:
                data = r.read()
            open(path, "wb").write(data)
    return Keys(key_dir)


def _ctr(key, iv, data):
    return AES.new(key, AES.MODE_CTR, counter=Counter.new(128, initial_value=int.from_bytes(iv, "big"))).decrypt(data)


def _sce_header(d):
    if d[:4] != b"SCE\0":
        raise ValueError("not an SCE file")
    key_rev, hdr_type, meta = struct.unpack_from(">HHI", d, 8)
    hsize, = struct.unpack_from(">Q", d, 16)
    flags = key_rev  # se_flags shares the field with the key revision
    return key_rev, hdr_type, meta, hsize, flags


def _metadata(d, meta, hsize, info, erk, riv):
    """Decrypt the metadata. `info` is the 0x40-byte metadata info, already
    stripped of the NPDRM layer. Returns (section headers, data keys)."""
    info = AES.new(erk, AES.MODE_CBC, riv).decrypt(info)
    if info[16] != 0 or info[48] != 0:
        raise ValueError("metadata did not decrypt (wrong key or klicensee)")
    key, iv = info[0:16], info[32:48]
    hdrs = _ctr(key, iv, d[meta + 0x60:hsize])
    _, _, nsec, nkeys = struct.unpack_from(">QIII", hdrs, 0)
    secs = []
    for i in range(nsec):
        o = 0x20 + i * 0x30
        off, size, typ, prog, _hashed, _sha, enc, kidx, ividx, comp = struct.unpack_from(">QQIIIIIIII", hdrs, o)
        secs.append(dict(off=off, size=size, type=typ, prog=prog, enc=enc, kidx=kidx, ividx=ividx, comp=comp))
    ko = 0x20 + nsec * 0x30
    keys = [hdrs[ko + i * 16: ko + i * 16 + 16] for i in range(nkeys)]
    return secs, keys


def _section(d, s, keys):
    data = d[s["off"]:s["off"] + s["size"]]
    if s["enc"] == 3:
        data = _ctr(keys[s["kidx"]], keys[s["ividx"]], data)
    return data


def decrypt_self(data, keys, klic=None):
    """Decrypt an NPDRM SELF/SPRX (bytes) to ELF bytes. klic defaults to the
    free-license klicensee."""
    key_rev, _, meta, hsize, _ = _sce_header(data)
    appinfo, elf_off, phdr_off, shdr_off = struct.unpack_from(">QQQQ", data, 0x28)
    prog_type, = struct.unpack_from(">I", data, appinfo + 0x0C)
    info = data[meta + 0x20:meta + 0x60]
    if prog_type == 8:                                    # NPDRM application
        npkey = AES.new(keys.klic_key, AES.MODE_ECB).decrypt(klic or keys.klic_free)
        info = AES.new(npkey, AES.MODE_CBC, bytes(16)).decrypt(info)
        erk, riv = keys.npdrm[key_rev]
    else:
        raise ValueError(f"unsupported SELF program type {prog_type}")
    secs, dkeys = _metadata(data, meta, hsize, info, erk, riv)

    if data[elf_off + 4] != 2:
        raise ValueError("only 64-bit ELFs are supported")
    ehdr = data[elf_off:elf_off + 0x40]
    e_phoff, e_shoff = struct.unpack_from(">QQ", ehdr, 0x20)
    e_phentsize, e_phnum, e_shentsize, e_shnum = struct.unpack_from(">HHHH", ehdr, 0x36)
    phdrs = [data[phdr_off + i * 0x38: phdr_off + i * 0x38 + 0x38] for i in range(e_phnum)]

    out = bytearray(ehdr)

    def put(off, blob):
        if len(out) < off + len(blob):
            out.extend(bytes(off + len(blob) - len(out)))
        out[off:off + len(blob)] = blob

    for s in secs:
        if s["type"] != 2:
            continue
        p_offset, = struct.unpack_from(">Q", phdrs[s["prog"]], 0x08)
        p_filesz, = struct.unpack_from(">Q", phdrs[s["prog"]], 0x20)
        blob = _section(data, s, dkeys)
        if s["comp"] == 2:
            blob = zlib.decompress(blob)[:p_filesz]
        put(p_offset, blob)
    if shdr_off and e_shnum:
        put(e_shoff, data[shdr_off:shdr_off + e_shnum * e_shentsize])
    put(e_phoff, b"".join(phdrs))
    return bytes(out)


def rap_to_rif(rap, keys):
    """The licence key a DLC's EDATs decrypt with, from the player's 16-byte
    .rap (RPCS3's rap_to_rif): an AES-128-CBC decrypt, then five rounds of a
    byte permutation with two constant masks."""
    if len(rap) != 16:
        raise ValueError("a .rap file is 16 bytes")
    key = bytearray(AES.new(keys.rap_key, AES.MODE_CBC, bytes(16)).decrypt(rap))
    pbox, e1, e2 = keys.rap_pbox, keys.rap_e1, keys.rap_e2
    for _ in range(5):
        for i in range(16):
            p = pbox[i]
            key[p] ^= e1[p]
        for i in range(15, 0, -1):
            key[pbox[i]] ^= key[pbox[i - 1]]
        o = 0
        for i in range(16):
            p = pbox[i]
            kc = (key[p] - o) & 0xFF
            ec2 = e2[p]
            if o != 1 or kc != 0xFF:
                o = 1 if kc < ec2 else 0
                key[p] = (kc - ec2) & 0xFF
            else:                       # o == 1 and kc == 0xFF
                key[p] = (kc - ec2) & 0xFF
    return bytes(key)


def npd_content_id(path):
    """The content ID an EDAT belongs to (its NPD header), or None."""
    with open(path, "rb") as f:
        h = f.read(0x40)
    if len(h) < 0x40 or h[:4] != b"NPD\0":
        return None
    return h[0x10:0x40].split(b"\0")[0].decode("ascii", "replace")


def decrypt_sce_package(data, keys):
    """Decrypt an SCE package (a firmware dev_flash_* file); returns its sections."""
    _, _, meta, hsize, flags = _sce_header(data)
    secs, dkeys = _metadata(data, meta, hsize, data[meta + 0x20:meta + 0x60], keys.scepkg_erk, keys.scepkg_riv)
    out = []
    for s in secs:
        blob = _section(data, s, dkeys)
        out.append(zlib.decompress(blob) if s["comp"] == 2 else blob)
    return out


def pup_entries(path):
    f = open(path, "rb")
    hdr = f.read(0x30)
    if hdr[:5] != b"SCEUF":
        raise ValueError("not a PS3 firmware file (PUP)")
    count, = struct.unpack_from(">Q", hdr, 0x18)
    table = f.read(count * 0x20)
    ents = {}
    for i in range(count):
        eid, off, size = struct.unpack_from(">QQQ", table, i * 0x20)
        ents[eid] = (off, size)
    return f, ents


def pup_extract(pup_path, keys, wanted, progress=None):
    """Extract dev_flash files (paths like 'sys/external/flashATRAC.pic') from a
    PUP. Returns {path: bytes} for the ones found."""
    f, ents = pup_entries(pup_path)
    if 0x300 not in ents:
        raise ValueError("PUP has no update_files.tar")
    off, size = ents[0x300]
    f.seek(off)
    outer = tarfile.open(fileobj=io.BytesIO(f.read(size)))
    pkgs = [m for m in outer.getmembers() if "dev_flash_" in m.name]
    want = {w.strip("/"): None for w in wanted}
    found = {}
    for n, m in enumerate(pkgs):
        if progress:
            progress(n, len(pkgs))
        secs = decrypt_sce_package(outer.extractfile(m).read(), keys)
        if len(secs) < 3:
            continue
        inner = tarfile.open(fileobj=io.BytesIO(secs[2]))
        for im in inner.getmembers():
            name = im.name.lstrip("./")
            if name.startswith("dev_flash/"):
                name = name[len("dev_flash/"):]
            if name in want and im.isfile():
                found[name] = inner.extractfile(im).read()
        if len(found) == len(want):
            break
    return found


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("self"); a.add_argument("input"); a.add_argument("output")
    a.add_argument("--keys", required=True); a.add_argument("--klic")
    b = sub.add_parser("pup"); b.add_argument("input"); b.add_argument("outdir"); b.add_argument("files", nargs="+")
    b.add_argument("--keys", required=True)
    args = ap.parse_args()
    keys = fetch_key_vault(args.keys)
    if args.cmd == "self":
        elf = decrypt_self(open(args.input, "rb").read(), keys, bytes.fromhex(args.klic) if args.klic else None)
        open(args.output, "wb").write(elf)
        print(f"{args.output}: {len(elf)} bytes")
    else:
        got = pup_extract(args.input, keys, args.files, lambda i, n: print(f"\r{i + 1}/{n}", end=""))
        print()
        for name, blob in got.items():
            dst = os.path.join(args.outdir, name)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            open(dst, "wb").write(blob)
            print(f"{dst}: {len(blob)} bytes")
        missing = set(f.strip("/") for f in args.files) - set(got)
        if missing:
            sys.exit("not found in the PUP: " + ", ".join(sorted(missing)))


if __name__ == "__main__":
    main()
