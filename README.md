# Castlevania: Harmony of Despair — PS3 Recomp

Static recompilation of *Castlevania: Harmony of Despair* (PSN, **NPUB30505**, USA) to native Windows, built with [ps3recomp](https://github.com/sp00nznet/ps3recomp) (submodule in `ext/ps3recomp`).

> [!IMPORTANT]
> **Windows only.** This version runs on Windows 10 and 11 (64-bit). There are no Linux or macOS builds.

> [!IMPORTANT]
> **For the full game you need two files of your own: the game and its licence.**
> - **The game:** the PSN package `UP0101-NPUB30505_00-CASTHARMONYOFDES.pkg`.
> - **The licence (key):** `UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ.rap`, from the purchase of the full version.
>
> With the package alone you get only the trial. No game files or keys are included here.

## Minimum requirements

| | |
|---|---|
| System | Windows 10 or 11, 64-bit |
| Processor | 64-bit (x86-64) with SSE4.1 |
| Graphics | DirectX 12 capable GPU (feature level 11_0) |
| Memory | 8 GB RAM |
| Disk | about 6 GB free during installation |
| Internet | during installation: the PS3 firmware (from Sony) and the build tools, about 500 MB |

Installation takes about 15 to 30 minutes, depending on the PC. It happens once.

## Support

If you enjoy this project, you can support it here: [Buy me a coffee](https://buymeacoffee.com/mpolizel) ☕

## Install

1. Download `CHDPS3Recomp-v<version>-windows-x64.zip` from the [Releases](https://github.com/mpolizel/CHDPS3Recomp/releases) page and unzip it to a folder of your choice.
2. Open **`CHDLauncher.exe`**.

Nothing else needs to be installed: the package carries its own Python, and the installer downloads its build tools.

In the launcher, under **Installation**, choose your game package, then **Install**. For the full game, add your `.rap` under **Licences**. When it's done, **Game → Play** starts it.

The launcher's menu is inspired by the PS3's, with its own art. It runs with keyboard, controller (XInput) or mouse, shows the installation's progress, and comes back when the game closes. The source is [launcher/launcher.cpp](launcher/launcher.cpp). `build_launcher.bat` builds it with the portable toolchain; the release ships it built, since it holds no game code.

You can also install through [RecompLauncher](https://recomplauncher.com): pick the package and, for the full game, the `.rap`. It runs the installer with no console window ([recomplauncher.json](recomplauncher.json)).

From a copy of this repository, run:

```powershell
install.bat <UP0101-NPUB30505_00-CASTHARMONYOFDES.pkg> [UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ.rap]
```

Once it's done, open `build-mingw\CHDPS3Recomp.exe` to play. It needs no arguments and opens no console; its log goes to `out\run.log`.

## Building a release

```powershell
build_launcher.bat                        # CHDLauncher.exe (needs cache\toolchain: run the installer once)
.venv\Scripts\python tools\make_release.py  # dist\CHDPS3Recomp-v<version>-windows-x64.zip
```

`make_release.py` packages the following:
- the launcher and a portable Python (`tools/bundle_python.py`);
- the port's sources, the installer, and ps3recomp's tracked files with `patches/ps3recomp` applied (marked `.chd-patched`, so no Git is needed);
- the docs, licences and manifests.

It checks that `ext/ps3recomp` matches the patch exactly. Before zipping, it refuses to package any game file, key file or the game's klicensee, which it reads from the local EBOOT. Bump `version.txt` first (0.0.1 … 0.0.9, then 0.1.0).

Windows Defender's machine-learning check may flag an unsigned `CHDLauncher.exe` as a false positive. Signing the exe, or reporting it at https://www.microsoft.com/wdsi/filesubmission, avoids that for players.

## Settings

Open them in the launcher under **Settings**, or press **F1** in the game to open them over it, with **Back to game** at the top. They are saved in `config.ini` in the game's folder.

| Setting | Choices | Takes effect |
|---|---|---|
| Screen | Windowed, Fullscreen (borderless) | right away; **F11** or **Alt+Enter** in the game too |
| Fullscreen picture | Keep the game's shape (black bars on ultrawide or 16:10), Stretch | right away |
| Window size | 1280x720, 1600x900, 1920x1080, 2560x1440 | right away |
| Volume | 0 to 100% | right away (the game's session in Windows' mixer) |
| VSync | On, Off | next start |
| Show FPS | Off, On (in the title) | right away |
| Esc closes the game | No, Yes | right away |

While the settings are open over the game, the controller doesn't reach the game.

The image the game is presented at stays 1280x720. A `Resolution=1920x1080` line is read, but at that size the title screen draws only its text, so it isn't offered yet.

The installer ([tools/install.py](tools/install.py)) does the rest:
1. Checks the package.
2. Extracts the package.
3. Decrypts the EBOOT and the 118 modules ([tools/ps3crypt.py](tools/ps3crypt.py); no RPCS3 needed).
4. Downloads the PS3 firmware from Sony's servers and keeps only its ATRAC decoder.
5. Translates the game to C++ and builds it.

If it stops, run it again and it picks up where it left off. Nothing opens a console window: every tool runs hidden, and the progress goes to `out\install.log`.

From this repository the installer needs Git and Python 3. A release package carries a portable Python in `python/` ([tools/bundle_python.py](tools/bundle_python.py): python.org's embeddable build plus the tools' packages), and RecompLauncher runs the installer on its `pythonw.exe`. Visual Studio is not needed.

The installer downloads a portable toolchain into `cache/toolchain`, pinned and checked by SHA-256: [llvm-mingw](https://github.com/mstorsjo/llvm-mingw), CMake and Ninja. It builds into `build-mingw/`. libc++ is linked statically, so the exe needs only DLLs that ship with Windows 10/11 (no Visual C++ Redistributable).

A full install takes about 20 minutes. `build.bat` (Visual Studio clang-cl, into `build/`) remains for development.

No keys are in the repository. `ps3crypt.py` reads them from RPCS3's public `key_vault.cpp`/`.h`, downloaded at install time. The game's own klicensee is read from the player's EBOOT.

The `.rap` is copied to `licenses/`. Without it the game runs as the trial, as on a PS3 without the purchase: Alucard only, single player. A small **Trial Mode** label stays in the corner of the window. Adding the `.rap` later (launcher: **Licences > Add licence**) unlocks the full game the next time it starts, with no reinstall.

The manual steps below are what the installer automates.

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

## 4. Lift the game's PRX modules

The game loads its characters, enemies and shots as 118 PRX modules at run time. Each one is relocated at its own fixed guest base (0x40000000-0x42990000) and lifted with its own symbol prefix:

```powershell
.venv\Scripts\python tools\build_prx.py --jobs 16   # -> game\prx\*.bin, recompiled_prx\*.cpp + prx_table.c
```

At run time [src/port_prx.c](src/port_prx.c) answers `sys_prx_load_module` by copying the module image to its base and registering its functions, calls `module_start` for `sys_prx_start_module`, and implements the 12 `sys_libc` functions the modules import.

## 5. Lift the audio SPU program

All sound goes through Multistream, whose mixer is an SPU program embedded in the EBOOT (69,140 bytes at file offset 0xAF4480). Extract it and lift it to C:

```powershell
.venv\Scripts\python ext\ps3recomp\tools\extract_spu_images.py game\decrypted\USRDIR\EBOOT.elf --output spu_images
.venv\Scripts\python ext\ps3recomp\tools\build_spu_workloads.py --images spu_images --lifted spu --out spu\spu_workloads.c --constructor
```

`spu_images/` and `spu/` come from the game and are not in the repo. CMake picks up `spu/*/spu_recomp.c` and `spu/spu_workloads.c`.

### PS3 firmware (music)

The music is ATRAC3. Multistream decodes it with the firmware's `flashATRAC.pic`, which it reads from `/dev_flash` and loads into the SPU at run time. Sony distributes the firmware for free:

1. Download `PS3UPDAT.PUP` from https://www.playstation.com/en-us/support/hardware/ps3/system-software/.
2. In RPCS3 (`tools\rpcs3\rpcs3.exe`), choose **File → Install Firmware** and select the file.

At startup the port looks for `dev_flash` in `tools/rpcs3/`, `rpcs3/` or on its own, next to the executable, one level above it, and under the working directory. `PS3_DEV_FLASH=<dir>` overrides the search. The decoder runs in the SPU interpreter (`SPU_INTERP_UNLIFTED=1`, on by default). No lift exists for code loaded at run time.

Without the firmware, the SPU stops at the first music stream and there is no sound at all. `SPU_NULL_CALL_RETURN=1` keeps the sound effects alive (the music streams then buzz).

## 6. Build and run

The first build takes about 45 minutes (the EBOOT plus 118 modules, ~850 MB of generated C++).

```powershell
build.bat          # applies patches\ps3recomp\*.patch to the submodule, then CMake + Ninja with clang-cl
mklink /J game\vfs\PS3_GAME game\base
run.bat            # PS3_VFS_ROOT=game/vfs, log in out\run.log
```

Keyboard: Enter = Start, Tab = Select, arrows = D-pad/left stick, Z = Cross (confirm), X = Circle (back), A = Square, S = Triangle, Q/W = L1/R1, 1/2 = L2/R2, 3/4 = L3/R3, I/J/K/L = right stick. **4 (R3) toggles the in-stage camera zoom** between the whole map and the close view. An Xbox controller replaces the keyboard when connected.

## Port-specific code

- [src/port_resc.c](src/port_resc.c): cellResc overrides. The game renders 1920x1080 with 2x AA into an offscreen surface and presents it through RESC; this registers RESC's display buffers, makes the surface the backend's scanout and queues the flip into the game's command buffer. `cellRescSetWaitFlip` really waits for the flip: as a no-op the game ran ahead, the FIFO drain ran past flips, and frames were shown half drawn (HUD, pause menu and player flickering from the lobby on).
- [patches/ps3recomp](patches/ps3recomp): changes to the ps3recomp D3D12 backend, kept as a patch until they go upstream:
  - scaled composite of an offscreen scanout onto the backbuffer;
  - persistent offscreen scanout, so flips present full frames and clear boundaries do not (this fixed the flicker); the main loop's flip ticker presents at every flip in that mode;
  - `GCM_CATCHUP_LOG=1` logs each flip the FIFO drain runs past (frames mixed in one present);
  - fragment-program inline constants go to `fp_constants[]` in b1, filled per draw, instead of HLSL literals: the pipeline cache is keyed on the code alone, so every draw of a program used the constants of the first one compiled and all eight colours on the character colour screen were the same palette (`FP_LITERAL=1` restores the literals);
  - `cellFsReaddir` clears the entry at the end of a directory: the game read the stale last name again, listed each DLC folder's last file twice (its player module eight times), built 29 character entries for 12 and the character-select panel overran its 16 slots and ran the heap out of memory ("Change Characters" closed the game);
  - executes a full draw batch instead of dropping draws past `MAX_DRAWS`;
  - X16 textures are R16_UNORM presented as {A,R,G,B} = {X,1,X,1}, as in RPCS3 (the text strings were white boxes).
  - texture -> render-target aliases (`rsx_d3d12_alias_texture_to_rt`, `TEX_ALIAS`): the menu UI layer is copied by the CPU from RT 0x02AF9800 to 0x0443C000, which on the host GPU only copied zeros.
  - culling honours SHADER_WINDOW origin: with origin bottom the front face is flipped (the menu's 3D book pages were culled as back faces).
  - diagnostics: `RSX_CAP_ALPHA=1` (capture alpha), `RSX_DBG_VTX=<tex offset>` (dump quad vertices), `TEX_ALIAS_OPAQUE=1`.
  - `SPU_NULL_CALL_RETURN=1`: an SPU call through a null function pointer returns 0 instead of stopping the SPU.
- [stubs.cpp](stubs.cpp): finds the installed firmware (`PS3_DEV_FLASH`) and turns on `SPU_INTERP_UNLIFTED` for the ATRAC decoder.
- [src/port_settings.c](src/port_settings.c): applies `config.ini` to the game. It reads the resolution and VSync before the renderer starts, then watches the file. It subclasses the game window for the title, Esc, F11, Alt+Enter and F1. Borderless fullscreen keeps the picture's shape over a black backdrop window, and the volume goes through the process's Windows audio session.
- `PAD_FOCUS_ONLY=1` (runtime patch): the controller only reaches the game while its window is in front, as the keyboard already did.
- [src/port_debug.c](src/port_debug.c): `CHD_DEBUG=<file>` starts a debug console that reads commands from that file and writes the answers to `<file>.out`: `mem <ea> [len]`, `dump <ea> <len> <host path>` and `poke8 <ea> <val>` (addresses in hex).
- [tools/capture_window.ps1](tools/capture_window.ps1), [tools/flicker_probe.ps1](tools/flicker_probe.ps1): screenshot the game window; sample frames to spot flicker.
- `RSX_CAP=<frame>:1 RSX_CAP_DIR=out\rsxcap`: the backend's frame capture (every draw, texture format and render target of one frame).

## Status

- Boots, title screen renders correctly with text, no flicker, 60+ FPS, keyboard input works, save data loads.
- Main menu, Local Play lobby and the PSN notice render fully.
- **Chapter 1 loads and plays** with Soma: HUD, timer and enemies run, keyboard controls the character.
- The full-version check (`sceNpDrmVerifyUpgradeLicense`) answers as the console does: the licence is there when `licenses/UP0101-NPUB30505_00-CVHDZDLCZUNLOCKZ.rap` is ([src/port_licence.c](src/port_licence.c)). Before, ps3recomp had no handler and an unresolved import answers OK, so every copy ran as the full game.
- Audio works when the PS3 firmware is installed (see step 5): sound effects and voices (PS-ADPCM, decoded by Multistream itself) and the music (ATRAC3, decoded by the firmware). Without the firmware there is no sound.
- ps3recomp's `lift_prx.py` gained the REL24/REL14/ADDR64/ADDR16_LO_DS relocations (and a corrected REL64) the game's modules use.

## Notes

- The game loads its characters, enemies and shots as PRX modules (`dynamic/`, `dlc/*/dll/`). They will need `lift_prx.py` besides the EBOOT.
