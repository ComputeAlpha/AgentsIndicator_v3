#!/usr/bin/env bash
# Build and flash the AgentIndicator v3 firmware onto the XIAO RP2350.
#   ./flash.sh            main firmware
#   ./flash.sh probe      layout-discovery probe (raw LED index shell)
# Uses the Pico SDK installed by the Raspberry Pi Pico VS Code extension
# (~/.pico-sdk). No button presses needed: picotool reboots a running
# device into BOOTSEL over USB. A blank board: hold BOOT while plugging in.
set -euo pipefail

HERE="$(cd "$(dirname "$0")/AgentIndicatorv3_HW" && pwd)"
SDK="$HOME/.pico-sdk"
export PICO_SDK_PATH="${PICO_SDK_PATH:-$SDK/sdk/2.3.1}"
export PATH="$SDK/toolchain/15_2_Rel1/bin:$SDK/cmake/v4.3.4/bin:$SDK/ninja/v1.13.2:$SDK/picotool/2.3.1/picotool:$PATH"

TARGET=AgentIndicatorv3_HW
[ "${1:-}" = "probe" ] && TARGET=agentind_probe

cmake -S "$HERE" -B "$HERE/build" -G Ninja -DPICO_BOARD=seeed_xiao_rp2350 >/dev/null
ninja -C "$HERE/build" "$TARGET"
UF2="$HERE/build/$TARGET.uf2"

# A running daemon holds the port; it reconnects by itself afterwards.
picotool reboot -f -u >/dev/null 2>&1 || true
for _ in $(seq 1 50); do
  picotool info >/dev/null 2>&1 && break
  sleep 0.2
done
picotool load -x "$UF2"
echo "Flashed $UF2"
