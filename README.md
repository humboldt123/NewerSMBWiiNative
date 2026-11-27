# Newer Super Mario Bros. Wii, native

[Newer Super Mario Bros. Wii](https://newerteam.com/wii/) running as a native PC and Mac game instead of in an emulator.

New Super Mario Bros. Wii's PowerPC code, with Newer's code linked in, is translated ahead of time into C++ and compiled for your machine, the same way [NSMBWiicompiled](https://github.com/ral1009/NSMBWiicompiled) runs the original game. This fork adds:

- **Newer SMBW 1.30**, built in rather than applied as a Riivolution patch at runtime
- **USA revision 2 discs** (SMNE01 rev 2), the version Newer targets
- **Native macOS on Apple Silicon**, drawn through Metal, next to Windows
- **Online co-op, work in progress.** Lockstep netplay between two machines, Mac and Windows included, on top of deterministic emulated time

> This repository contains no game files. You need your own dump of the USA revision 2 disc and the Newer SMBW 1.30 release files.

## Status

| | |
| --- | --- |
| Windows x64 | Boots and plays Newer: title, intro level, world map, levels |
| macOS ARM64 | Boots and plays Newer at full speed on an M2 MacBook Air |
| Online co-op | Works between a Mac and a Windows PC over Tailscale. They stay in sync from boot to the world map and into levels. Still rough: about 45 FPS, and audio sample buffers are falsely flagged as a desync during the file select |
| Linux | The upstream Linux build should work for the stock game; Newer on Linux is untested |

Not yet checked by hand: every world, bosses, shops and saving through a whole playthrough. The upstream port's setup screen doesn't know about rev 2 or Newer yet, so the disc and Newer folder are set in `Config.toml` (below).

## How it works

1. `projects/nsmbw/tools/newer_link.py` reads Newer's `System.bin` (memory patches), `DLCode` (its code) and `DLRelocs` (relocations), the files Newer's own loader reads on a Wii. It bakes them into `main.dol` and the four game RELs, with Newer's code at `0x81800000`. This makes a new game version, `E2N`.
2. The translator turns that into C++ (`generated_nsmbw-E2N/`), and CMake builds it with `-DNSMBW_VARIANT=N`.
3. At runtime, Newer's replacement files (levels, tilesets, music, layouts) come from your Newer folder through the port's Riivolution overlay support (`overlay_roots`).

## Building

You need your extracted USA rev 2 disc (the `DATA` partition) and the Newer SMBW 1.30 folder (`NewerSMBW/` plus `riivolution/`).

### Translate (on Windows, or anywhere with .NET 8)

Follow [docs/BUILDING.md](docs/BUILDING.md) for the toolchain. Then, from the repo root:

```powershell
python -I projects/nsmbw/tools/unpack_game_code.py <disc>\DATA NSMBW-Versions/E2
python -I projects/nsmbw/tools/newer_link.py NSMBW-Versions/E2 <newer>\NewerSMBW US_2 NSMBW-Versions/E2N 80768500,8076D6C0,809A2CE0,80B1C960
# extra_seeds.txt for E2N = the E2 seeds + newer_seeds.txt + newer_game_seeds.txt
python -I projects/nsmbw/tools/map_function_map.py E2N NSMBW-Versions/E2N NSMBW-Versions/E2N/function_map.txt --no-source
# append every address in NSMBW-Versions/E2N/extra_seeds.txt that function_map.txt doesn't list yet
python -I projects/nsmbw/tools/gen_version_tables.py E2N
```

Then run the translator steps from BUILDING.md against `generated_nsmbw-E2N/projects/*.yml`, with `NSMBW_REL_BSS_CONSOLE=1` set. Newer hard-codes the console's REL BSS addresses. The REL start addresses are `d_profileNP 0x80768500`, `d_basesNP 0x8076D6C0`, `d_enemiesNP 0x809A2CE0` and `d_en_bossNP 0x80B1C960`.

**Important:** if the seeds aren't appended to `function_map.txt`, Newer's entry points aren't translated, and the game stops right after boot with `missing_target 0x8184E068`.

### Build on Windows

```powershell
cmake -S runtime -B build_nsmbw-E2N -G Ninja -DCMAKE_BUILD_TYPE=Release -DMKW_BUILD_NSMBW=ON -DMKW_BUILD_MKW=OFF -DNSMBW_GAME_VERSION=E2 -DNSMBW_VARIANT=N -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake --build build_nsmbw-E2N --target NSMBWCompiled
```

### Build on macOS (Apple Silicon)

You need Xcode's command line tools, CMake and Ninja. Copy `generated_nsmbw-E2N/` over from the machine that translated it, then:

```bash
python3 -I projects/nsmbw/tools/relocate_generated.py generated_nsmbw-E2N C:/path/to/the/windows/checkout
cmake -S runtime -B build-mac-E2N -G Ninja -DCMAKE_BUILD_TYPE=Release -DMKW_BUILD_NSMBW=ON -DMKW_BUILD_MKW=OFF -DNSMBW_GAME_VERSION=E2 -DNSMBW_VARIANT=N -DMKW_TRANSLATED_COMPILE_JOBS=2
cmake --build build-mac-E2N --target NSMBWCompiled
```

This makes `build-mac-E2N/Newer Super Mario Bros. Wii (Recompiled).app`. If `projects/nsmbw/branding/newer.icns` exists, the app uses it as its icon. That folder is gitignored, since the icon is Nintendo artwork.

### Point it at your files

Put an empty `portable.txt` next to `NSMBWCompiled`. Then create `UserData/Config.toml` beside it:

```toml
[paths]
dvd_root = "/path/to/disc/DATA"
overlay_roots = ["/path/to/Newer folder"]
```

## Online co-op (experimental)

Both machines need the same build, the same disc and Newer files, and the same save. Start both from a fresh save with `NSMBW_RESET_SAVE=1` for now. A direct connection is needed: same network, or [Tailscale](https://tailscale.com).

```bash
NSMBW_NETPLAY=host:7777 ./NSMBWCompiled                 # player 1
NSMBW_NETPLAY=join:<host address>:7777 ./NSMBWCompiled  # player 2
```

How it works:

- Each machine plays its own player 1 controls. The host is Mario, the joiner is Luigi.
- Every frame, each machine sends its input for a few frames ahead and waits for the other's input. Change how far ahead with `NSMBW_NETPLAY_DELAY`, in frames (default 3).
- The game only sees inputs both machines agreed on. Time is emulated, not read from the host's clock (`runtime/include/guest_clock.h`). So both machines compute exactly the same game, and each one draws its own copy of it.
- Every second both machines hash the game's memory and compare. A mismatch is logged as `[netplay] DESYNC`.

### Rollback (experimental)

Add `NSMBW_NETPLAY_ROLLBACK=1` on a Mac. Your own input applies on the next frame instead of waiting for the network:
- The game guesses the other player's input (their last known buttons).
- When the real input arrives and the guess was wrong, it rewinds to that frame and silently replays to the present.

Each frame is saved by copy-on-write: guest memory is write-protected and the first write to a page records its old bytes. Saving also copies every guest thread's native stack and the runtime's own state (`runtime/include/savestate.h`). That's about 0.1 ms and a few MB per frame.

It works on macOS and Windows: game threads are libco coroutines on both, so every stack can be copied. A rollback machine can also play against a lockstep one. `NSMBW_NETPLAY_FAKE_LAG_MS=<ms>` adds simulated one-way lag, to see rollback at work on a LAN.

`NSMBW_SAVESTATE_TEST=1` checks the savestates alone: save, play 30 frames, rewind, replay, and compare guest memory.

`NSMBW_DETERMINISTIC=1` turns on emulated time without netplay. `NSMBW_DET_HASH_FILE=<path>` logs the memory hashes, for checking that two runs or two machines stay identical.

## Credits

- [NSMBWiicompiled](https://github.com/ral1009/NSMBWiicompiled) by ral1009, the New Super Mario Bros. Wii port this is a fork of
- [WiiCompiled](https://github.com/patchzyy/Wiicompiled), the Mario Kart Wii recompilation the translator and runtime come from
- [Newer Team](https://newerteam.com/), for Newer Super Mario Bros. Wii
- [aurora](https://github.com/encounter/aurora), which draws the game's graphics on modern GPUs
- [sse2neon](https://github.com/DLTcollab/sse2neon), for the ARM64 build
- [Dolphin](https://github.com/dolphin-emu/dolphin), whose Riivolution code the overlay support is based on
- [NSMBW-Community](https://github.com/NSMBW-Community), for the address maps and decompilation used as reference

Licensed under [GPL-3.0](LICENSE). Third-party licenses are in [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).
