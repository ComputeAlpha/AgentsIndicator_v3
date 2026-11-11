#!/usr/bin/env bash
# AgentIndicator v3 - host installer for Linux and macOS.
#
#   ./install.sh                 build, test, install to ~/.local/bin, add hooks, start service
#   ./install.sh --no-service    same, but let the hook autostart the daemon instead
#   ./install.sh --udev          (Linux) also install a udev rule so the device is user-accessible
#   ./install.sh --uninstall     remove hooks + service + binary
#
# Requirements: a C++17 compiler (g++/clang++; Xcode CLT on macOS). CMake is
# used when present, otherwise the compiler is invoked directly.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")" && pwd)"
PREFIX="${PREFIX:-$HOME/.local}"
BIN="$PREFIX/bin/agent-indicator"
OS="$(uname -s)"

want_udev=0
args=()
for a in "$@"; do
  case "$a" in
    --udev) want_udev=1 ;;
    --uninstall)
      if [ -x "$BIN" ]; then "$BIN" uninstall || true; fi
      rm -f "$BIN"
      echo "Removed $BIN"
      exit 0 ;;
    *) args+=("$a") ;;
  esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }

# ---------------------------------------------------------------- build
CXX="${CXX:-}"
if [ -z "$CXX" ]; then
  for c in c++ clang++ g++; do command -v "$c" >/dev/null 2>&1 && { CXX="$c"; break; }; done
fi
[ -n "$CXX" ] || { echo "error: no C++ compiler found (install build-essential / Xcode Command Line Tools)"; exit 1; }

BUILD="$ROOT/host/build"
if command -v cmake >/dev/null 2>&1; then
  say "Building with CMake ($CXX)"
  CXX="$CXX" cmake -S "$ROOT/host" -B "$BUILD" -DCMAKE_BUILD_TYPE=Release >/dev/null
  cmake --build "$BUILD" -j >/dev/null
else
  say "Building with $CXX (no CMake found)"
  mkdir -p "$BUILD"
  core=("$ROOT"/host/src/{json,platform,tracker,composer,config,serial,feed}.cpp)
  "$CXX" -std=c++17 -O2 -Wall -I"$ROOT/host/src" -DAGENT_INDICATOR_VERSION='"1.1.0"' \
    "${core[@]}" "$ROOT"/host/src/{main,hook,daemon,install}.cpp -o "$BUILD/agent-indicator"
  "$CXX" -std=c++17 -O1 -I"$ROOT/host/src" "${core[@]}" "$ROOT/host/tests/tests.cpp" -o "$BUILD/agentind_tests"
fi

say "Running unit tests"
"$BUILD/agentind_tests"

# ---------------------------------------------------------------- install
say "Installing $BIN"
mkdir -p "$PREFIX/bin"
# install(1) writes a new inode, so a running daemon keeps its old binary safely
install -m 0755 "$BUILD/agent-indicator" "$BIN"

if [ "$OS" = "Linux" ]; then
  RULE=/etc/udev/rules.d/60-agent-indicator.rules
  if [ "$want_udev" = 1 ]; then
    say "Installing udev rule $RULE (sudo)"
    echo 'SUBSYSTEM=="tty", ATTRS{idVendor}=="2e8a", ATTRS{product}=="AgentIndicator v3", MODE="0666", SYMLINK+="agent-indicator"' \
      | sudo tee "$RULE" >/dev/null
    echo 'SUBSYSTEM=="usb", ATTRS{idVendor}=="2e8a", MODE="0666"' | sudo tee -a "$RULE" >/dev/null
    sudo udevadm control --reload-rules && sudo udevadm trigger
  else
    for d in /dev/serial/by-id/*AgentIndicator_v3*; do
      [ -e "$d" ] || continue
      if [ ! -w "$(readlink -f "$d")" ]; then
        echo "note: $d is not writable by $(id -un). Re-run with --udev, or: sudo usermod -aG dialout $(id -un) (then log out/in)."
      fi
    done
  fi
fi

say "Registering Claude Code hooks and the background service"
"$BIN" install "${args[@]+"${args[@]}"}"

case ":$PATH:" in
  *":$PREFIX/bin:"*) ;;
  *) echo "note: add $PREFIX/bin to your PATH to use 'agent-indicator status' directly." ;;
esac
