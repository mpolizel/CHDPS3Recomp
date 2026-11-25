"""find_klic.py - recover a title's custom NPDRM klicensee from its decrypted EBOOT.

Free-license (type 3) SPRX modules are often encrypted with a game-specific klicensee
that the EBOOT passes to sceNpDrmIsAvailable at runtime, so it sits as a 16-byte
constant in EBOOT.elf. This tries every offset of the ELF as the klicensee against an
SPRX's encrypted metadata info and reports the one that decrypts to valid padding.

NPDRM keys are parsed from RPCS3's rpcs3/Crypto/key_vault.cpp and key_vault.h.

Usage:
    python find_klic.py <EBOOT.elf> <module.sprx> <key_vault.cpp> <key_vault.h> [--step 1]
"""
import argparse, re, struct, sys
from Crypto.Cipher import AES

def parse_keys(cpp, h):
    hsrc = open(h, encoding="utf-8").read()
    m = re.search(r"NP_KLIC_KEY\[0x10\]\s*=\s*\{([^}]*)\}", hsrc)
    klic_key = bytes(int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]{2})", m.group(1)))
    src = open(cpp, encoding="utf-8").read()
    body = src[src.index("void KeyVault::LoadSelfNPDRMKeys()"):]
    body = body[:body.index("\n}\n")]
    keys = {}
    for rev, erk, riv in re.findall(
            r'emplace_back\(\s*0x[0-9A-Fa-f]+,\s*0x[0-9A-Fa-f]+,\s*0x([0-9A-Fa-f]+),\s*KEY_NPDRM,\s*"([0-9A-Fa-f]+)",\s*"([0-9A-Fa-f]+)"',
            body):
        keys[int(rev, 16)] = (bytes.fromhex(erk), bytes.fromhex(riv))
    return klic_key, keys

def self_meta(path):
    d = open(path, "rb").read(0x1000)
    assert d[:4] == b"SCE\0", "not a SELF"
    rev, = struct.unpack_from(">H", d, 8)
    meta, = struct.unpack_from(">I", d, 12)
    appinfo, = struct.unpack_from(">Q", d, 0x28)
    ptype, = struct.unpack_from(">I", d, appinfo + 0x0C)
    off = meta + 0x20
    return rev, ptype, d[off:off + 0x40]

def valid(p):
    return p[16:32] == bytes(16) and p[48:64] == bytes(16)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf"); ap.add_argument("sprx"); ap.add_argument("key_vault_cpp"); ap.add_argument("key_vault_h")
    ap.add_argument("--step", type=int, default=4)
    a = ap.parse_args()

    klic_key, keys = parse_keys(a.key_vault_cpp, a.key_vault_h)
    rev, ptype, enc = self_meta(a.sprx)
    print(f"sprx: key revision 0x{rev:x}, program type {ptype}")
    if ptype != 8:
        sys.exit("not an NPDRM SELF")
    erk, riv = keys[rev]

    data = open(a.elf, "rb").read()
    offs = range(0, len(data) - 16, a.step)
    cands = b"".join(data[o:o + 16] for o in offs)
    npkeys = AES.new(klic_key, AES.MODE_ECB).decrypt(cands)   # klic -> npdrm key, in bulk
    outer = AES.new(erk, AES.MODE_CBC, riv)
    seen = set()
    for i, o in enumerate(offs):
        k = data[o:o + 16]
        if k in seen or k == bytes(16):
            continue
        seen.add(k)
        x = AES.new(npkeys[i * 16:i * 16 + 16], AES.MODE_CBC, bytes(16)).decrypt(enc)
        if valid(AES.new(erk, AES.MODE_CBC, riv).decrypt(x)):
            print(f"FOUND klicensee at EBOOT.elf+0x{o:x}: {k.hex().upper()}")
            return
    print(f"not found ({len(seen)} unique candidates, step {a.step})")
    sys.exit(1)

if __name__ == "__main__":
    main()
