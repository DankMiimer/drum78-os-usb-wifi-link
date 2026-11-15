#!/bin/sh
# Install or restore the RetroAchievements-enabled picoarch emulator frontend
# on a FunKey-OS handheld (RG Nano, FunKey S, GBA Mini) connected in ADB mode.
#
#   ./install_picoarch.sh            install / update /usr/games/picoarch
#   ./install_picoarch.sh --restore  restore stock /usr/games/picoarch.orig
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$HERE/.."
NANOCTL="${NANOCTL:-$REPO/nanoctl}"
BIN="${PICOARCH_BIN:-$HERE/bin/picoarch}"
SHA_FILE="$HERE/bin/picoarch.sha256"

usage() {
  sed -n '2,6p' "$0"
  exit 2
}

MODE="${1:-install}"
case "$MODE" in
  install|--install) MODE="install" ;;
  --restore|restore) MODE="restore" ;;
  -h|--help) usage ;;
  *) usage ;;
esac

if [ "$MODE" = "restore" ]; then
  "$NANOCTL" run '
    set -e
    if [ ! -x /usr/games/picoarch.orig ]; then
      echo "No backup found at /usr/games/picoarch.orig" >&2
      exit 1
    fi
    mount -o remount,rw /
    trap "mount -o remount,ro / 2>/dev/null || true" EXIT
    cp -p /usr/games/picoarch.orig /usr/games/picoarch.restore
    chmod 755 /usr/games/picoarch.restore
    mv -f /usr/games/picoarch.restore /usr/games/picoarch
    sync
    ls -la /usr/games/picoarch /usr/games/picoarch.orig
  '
  exit 0
fi

[ -f "$BIN" ] || { echo "Missing binary: $BIN" >&2; exit 1; }
if [ -f "$SHA_FILE" ] && command -v sha256sum >/dev/null 2>&1; then
  expected="$(awk '{print $1}' "$SHA_FILE")"
  actual="$(sha256sum "$BIN" | awk '{print $1}')"
  if [ -n "$expected" ] && [ "$expected" != "$actual" ]; then
    echo "SHA-256 mismatch for $BIN ($actual != $expected)" >&2
    exit 1
  fi
fi

"$NANOCTL" run '
  set -e
  mount -o remount,rw /
  trap "mount -o remount,ro / 2>/dev/null || true" EXIT
  if [ ! -e /usr/games/picoarch.orig ]; then
    if ! grep -qa "/mnt/FunKey/raoffline" /usr/games/picoarch 2>/dev/null; then
      cp -p /usr/games/picoarch /usr/games/picoarch.orig
      echo "Backed up stock binary to /usr/games/picoarch.orig"
    fi
  fi
'

"$NANOCTL" push "$BIN" /usr/games/picoarch.new 755

"$NANOCTL" run '
  set -e
  mount -o remount,rw /
  trap "mount -o remount,ro / 2>/dev/null || true" EXIT
  chmod 755 /usr/games/picoarch.new
  mv -f /usr/games/picoarch.new /usr/games/picoarch
  sync
  ls -la /usr/games/picoarch /usr/games/picoarch.orig 2>/dev/null || ls -la /usr/games/picoarch
'
echo "In-game RetroAchievements picoarch installed."
