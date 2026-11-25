# Castlevania: Harmony of Despair — PS3 Recomp

Static recompilation of *Castlevania: Harmony of Despair* (PSN, **NPUB30505**, USA) to native Windows, built with [ps3recomp](https://github.com/sp00nznet/ps3recomp) (submodule in `ext/ps3recomp`).

## Original files (not in the repo)

| File | Content ID | Purpose |
|---|---|---|
| base PKG (~809 MB) | `UP0101-NPUB30505_00-CASTHARMONYOFDES` | trial game (EBOOT + data) |
| unlock PKG (6 KB) | `UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ` | unlock entitlement (`.edat`) |
| `UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ.rap` | same as above | license for the unlock |

## Layout

```
ext/ps3recomp/        toolchain + runtime (git submodule)
game/base/            base PKG extracted (gitignored)
game/unlock/          unlock PKG extracted (gitignored)
game/decrypted/       EBOOT.ELF + .prx decrypted with RPCS3 (gitignored)
out/                  ppu_loader output (gitignored)
recompiled/           lifted C++ (gitignored)
```

## Setup

```powershell
git submodule update --init
py -3 -m venv .venv
.venv\Scripts\python -m pip install -r ext\ps3recomp\requirements.txt
```

## 1. Extract the PKGs (in-tree, no RPCS3)

```powershell
.venv\Scripts\python -I ext\ps3recomp\tools\pkg_extract.py <base.pkg> game\base
.venv\Scripts\python -I ext\ps3recomp\tools\pkg_extract.py <unlock.pkg> game\unlock
```

## 2. Decrypt the binaries (RPCS3)

All binaries are NPDRM SELFs (key rev 0x0A) with a **free license (type 3)**, so the `.rap` is not needed to decrypt. ps3recomp ships no keys, so this uses RPCS3 headless (`rpcs3.exe --decrypt`), unpacked into `tools/rpcs3/` (gitignored, official build from `RPCS3/rpcs3-binaries-win`).

- **EBOOT.BIN** decrypts with the default free klicensee.
- **The 118 `.sprx`** use the game's own klicensee, a 16-byte constant at `EBOOT.elf+0xb67dd4`. Recover it from your own EBOOT with [tools/find_klic.py](tools/find_klic.py), which tries every offset of the ELF against the encrypted SPRX metadata. The script reads the NPDRM keys from RPCS3's `rpcs3/Crypto/key_vault.cpp` and `key_vault.h`. RPCS3 reads the klic from stdin when the first attempt fails.

```bash
# copy the encrypted files to game/decrypted/USRDIR first; output is written next to the input
tools/rpcs3/rpcs3.exe --decrypt game\decrypted\USRDIR\EBOOT.BIN                    # -> EBOOT.elf
python tools/find_klic.py game\decrypted\USRDIR\EBOOT.elf <x.sprx> key_vault.cpp key_vault.h
printf '<KLIC>\n' | tools/rpcs3/rpcs3.exe --decrypt <x.sprx>  # -> x.prx
```

The `.rap` (unlock `CVHDZDLCZUNLOCKZ`) only matters at runtime, when the game checks whether the full version is owned.

## 3. Pipeline

```powershell
.venv\Scripts\python ext\ps3recomp\tools\ppu_loader.py game\decrypted\USRDIR\EBOOT.elf -o out\
.venv\Scripts\python ext\ps3recomp\tools\ppu_lifter.py game\decrypted\USRDIR\EBOOT.elf --functions out\EBOOT.functions.json --hle-stubs out\EBOOT.imports.json --code-end 0x9ced28 --chunks 32 -o recompiled\
.venv\Scripts\python ext\ps3recomp\tools\gen_hle_nids.py --all --out recompiled\ppu_hle_nids.cpp
```

`--code-end 0x9ced28` is the end of the executable sections (the section names are stripped).

Current result: 22,711 functions lifted into 33 chunks (~2.9M lines), 501 jump tables, 266 imports. 73 imports have no HLE handler in ps3recomp yet: sysPrxForUser 18, cellRudp 16, sceNp 13, sys_net 5, sceNp2 5, cellSysutilAvc2 4, sys_io 3, cellResc 3, cellSysutil 2, cellGcmSys 2, sys_fs 1, sceNpCommerce2 1.

Build needs **clang-cl** (VS Installer → "C++ Clang tools for Windows") plus CMake and Ninja.

## Notes

- The game loads its characters, enemies and shots as PRX modules (`dynamic/`, `dlc/*/dll/`). They will need `lift_prx.py` besides the EBOOT.
