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

## 4. Build and run

```powershell
build.bat          # applies patches\ps3recomp\*.patch to the submodule, then CMake + Ninja with clang-cl
mklink /J game\vfs\PS3_GAME game\base
run.bat            # PS3_VFS_ROOT=game/vfs, log in out\run.log
```

Keyboard: Enter = Start, arrows = D-pad, Z = Cross (confirm), X = Circle (back), A = Square, S = Triangle, Q/W = L1/R1.

## Port-specific code

- [src/port_resc.c](src/port_resc.c): cellResc overrides. The game renders 1920x1080 with 2x AA into an offscreen surface and presents it through RESC; this registers RESC's display buffers, makes the surface the backend's scanout and queues the flip into the game's command buffer.
- [patches/ps3recomp](patches/ps3recomp): changes to the ps3recomp D3D12 backend, kept as a patch until they go upstream:
  - scaled composite of an offscreen scanout onto the backbuffer;
  - persistent offscreen scanout, so flips present full frames and clear boundaries do not (this fixed the flicker);
  - executes a full draw batch instead of dropping draws past `MAX_DRAWS`;
  - X16 textures are R16_UNORM presented as {A,R,G,B} = {X,1,X,1}, as in RPCS3 (the text strings were white boxes).
  - texture -> render-target aliases (`rsx_d3d12_alias_texture_to_rt`, `TEX_ALIAS`): the menu UI layer is copied by the CPU from RT 0x02AF9800 to 0x0443C000, which on the host GPU only copied zeros.
  - culling honours SHADER_WINDOW origin: with origin bottom the front face is flipped (the menu's 3D book pages were culled as back faces).
  - diagnostics: `RSX_CAP_ALPHA=1` (capture alpha), `RSX_DBG_VTX=<tex offset>` (dump quad vertices), `TEX_ALIAS_OPAQUE=1`.
- [tools/capture_window.ps1](tools/capture_window.ps1), [tools/flicker_probe.ps1](tools/flicker_probe.ps1): screenshot the game window; sample frames to spot flicker.
- `RSX_CAP=<frame>:1 RSX_CAP_DIR=out\rsxcap`: the backend's frame capture (every draw, texture format and render target of one frame).

## Status

- Boots, title screen renders correctly with text, no flicker, 60+ FPS, keyboard input works, save data loads.
- Main menu renders fully (book pages, options, button hints); Local Play works.
- After Local Play a popup opens with no message text: the game submits no glyph draws for it, and makes no system calls when it opens (under investigation).
- The license check (`sceNpDrmVerifyUpgradeLicense`) is faked as OK, so the game runs as the full version.
- Audio: the ATRAC decoder needs the firmware's `flashATRAC.pic`; likely silent.
- The 118 game PRX modules (characters, enemies) are not loaded yet.

## Notes

- The game loads its characters, enemies and shots as PRX modules (`dynamic/`, `dlc/*/dll/`). They will need `lift_prx.py` besides the EBOOT.
