# Patched `picoarch` (Offline RetroAchievements Support)

Stock [DrUm78/FunKey-OS](https://github.com/DrUm78/FunKey-OS) ships `/usr/games/picoarch` without `rcheevos` support, so `Connect.opk` can cache achievement sets and upload unlocks from `/mnt/FunKey/raoffline/queue/`, **but in-game unlocks require this patched `picoarch` frontend**.

## How it installs

### Option A: Directly on the handheld (recommended, no PC needed)
1. Copy `Connect.opk` into `/mnt/Native games/` (for **RetroFE**, FunKey-OS default) and/or `/mnt/Applications/` (for **GMenu2X**).
2. Open **Connect → Achievements → Game Patch** and choose **Enable In-Game RA**.
3. On writable ext4 FunKey-OS rootfs builds (e.g. DrUm78's releases for RG Nano, GBA Mini, and FunKey S), Connect checks free space, backs up stock `/usr/games/picoarch` to `/usr/games/picoarch.orig` (if not already backed up), atomically installs the bundled ARM binary, verifies `--help` execution (rolling back to `.orig` automatically if a library check fails), and restores `/` read-only when finished. You can select **Restore Stock** from the same menu anytime.

### Option B: Over USB ADB from a PC
```bash
picoarch/install_picoarch.sh            # install / update /usr/games/picoarch
picoarch/install_picoarch.sh --restore  # revert to /usr/games/picoarch.orig
```

## Source, patches & licenses

- `src/picoarch-ra-offline.patch`: patch against [DrUm78/picoarch](https://github.com/DrUm78/picoarch) (`0fe4612`).
- `src/ra_offline.c`, `src/ra_offline.h`, `src/ra_toast_osd.h`: offline `rcheevos` integration (cached sets in `/mnt/FunKey/raoffline/cache/`, queued softcore unlocks in `/mnt/FunKey/raoffline/queue/`, UTC clock sync via `/mnt/FunKey/raoffline/clock_sync.json`, and in-game unlock toast OSD).
- Built against `rcheevos` (`src/LICENSE.rcheevos`, MIT) with FunKey-OS ARM musl toolchain (`bin/picoarch`, SHA-256 in `bin/picoarch.sha256`). See `src/LICENSE` (`picoarch` BSD-3-Clause / `libpicofe` GPLv2+ / LGPLv2.1+ / MAME).
