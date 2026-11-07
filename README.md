# AgentsIndicator v3

Indicates whether and how many Claude Code agents are working, idle or waiting for a human, on a
40×6 WS2812 matrix (4 × Seeed 6x10 RGB MATRIX for XIAO, driven by a XIAO RP2350).

```
 panel 1        panel 2        panel 3        panel 4
┌──────────┐ ┌──────────┐ ┌──────────┐ ┌──────────┐
│ 1 2      │ │ 3 4      │ │ 5 6    • │ │ 7 8      │   rows 0-4: two 3x5 digits + unit dots
│ …        │ │ …      • │ │ …      • │ │ …        │   (dots stacked bottom-up, no gaps)
│ …      • │ │ …      • │ │ …      • │ │ …        │
│▬▬▬▬▬▬▬▬▬▬│ │▬▬▬▬▬▬▬▬▬▬│ │▬▬▬▬▬▬▬▬▬▬│ │▬▬▬▬▬▬▬▬▬▬│   row 5: 40-pixel session status bar
└──────────┘ └──────────┘ └──────────┘ └──────────┘
  XIAO RP2350 (data in = D0/GPIO26) → chained left to right
```

## What the display means

**Status bar (bottom row).** The 40 pixels are divided among the sessions, oldest on the left, so
each session keeps its place. Slots wider than 2 px get a dark gap between them.

| Look | Meaning |
|---|---|
| Cyan comet running → | agent working |
| Yellow scanner ↔ | waiting for you: a permission/question prompt, or a finished turn |
| Dim red, short flash | idle: nothing assigned (a finished turn turns red after 10 min) |
| Green blink, then gone | session closed normally |
| Red rapid flashing | API error (rate limit, overload…) or the process crashed mid-work |
| White blink at far right | more than 40 sessions |

**Digit panels.** Unit dots stacked on the right edge from the bottom, with no gaps (as on v2):
1 = seconds, 2 = minutes, 3 = hours (up to 99 h). Pages alternate every 5 s and are configurable.

| Panel | Default pages |
|---|---|
| 1 | longest current **wait** (yellow) ↔ longest current **working streak** (cyan) |
| 2 | **agent time today**, summed over all sessions (rainbow) ↔ **actions/min**, tool calls per minute over the last 5 min (magenta, no dots) |
| 3 | **average wait** for you today (yellow) ↔ **average idle gap** today (red) |
| 4 | number of **live sessions** (rainbow, slowly drifting to avoid burn-in) |

The XIAO's yellow user LED (GPIO25, active-low) blinks only while data is arriving from the host;
with the daemon running that is a short blink about once a second.

Nothing working or waiting for 30 min → **starry night** (random sparse twinkles). If the host goes
silent, the panels show `--` after 10 s, and the device switches to starry night by itself after 30 min.

## Install on a computer (Linux / macOS)

Needs a C++17 compiler (`build-essential`, or Xcode Command Line Tools on macOS). CMake is used if
present but not required.

```sh
./install.sh            # build + tests, ~/.local/bin/agent-indicator, Claude hooks, background service
./install.sh --udev     # Linux: also add a udev rule if the port isn't user-writable
./install.sh --uninstall
```

`install` merges 17 hook events into `~/.claude/settings.json` (a timestamped backup is written
first; your other settings and hooks are kept, and re-running is idempotent). It registers a
systemd user unit (Linux) or a launchd agent (macOS). Without either, the hook starts the daemon on
demand. Sessions that are already running pick up the hooks when they restart.

```sh
agent-indicator status    # sessions, panels, today's metrics, device
agent-indicator test      # layout test pattern on the device
agent-indicator raw B 50  # send a device command through the daemon (here: brightness)
```

Config: `~/.config/agent-indicator/config` (brightness, current cap, timeouts, panel pages).
State/logs: `~/.local/state/agent-indicator/`; Linux logs: `journalctl --user -u agent-indicator`.

## Firmware

```sh
HW_firmware/flash.sh          # build + flash (no buttons; picotool reboots the board to BOOTSEL)
HW_firmware/flash.sh probe    # raw-index probe firmware used to discover the LED layout
```

Requires the Pico SDK from the Raspberry Pi Pico VS Code extension (`~/.pico-sdk`, SDK 2.3.1). The
firmware only needs flashing once; other computers only need `./install.sh`.

### LED layout (measured on the v3 build, 2026-11-07)

Each panel is wired **column-major, progressive** (not serpentine). Local LED 0 is bottom-left,
0–5 go up the first column, 6–11 go up the second, and so on; LED 59 is top-right. The panels chain
left → right starting at the XIAO panel:

```
index = panel*60 + col*6 + (5 - y)      x = panel*10 + col, y = 0 (top) .. 5 (bottom)
```

### Core split, and why there's no RTOS

| Core 0 | Core 1 |
|---|---|
| USB CDC (TinyUSB, IRQ driven), line parsing, watchdog | 60 fps renderer, PIO + DMA WS2812 output |

Both cores run their own simple loop. They share one lock-protected queue of complete display
snapshots (core 0 → core 1, newest wins) plus a few atomics, so the renderer never sees a
half-updated frame and neither core waits on the other. Sending all 240 LEDs takes about 7.2 ms of
DMA. Rendering doesn't stall while USB traffic arrives. A watchdog resets the board if either core
stops making progress.

An RTOS would add a scheduler, tick and stacks to size, but it wouldn't fix the v2 symptom (timers
moving between sessions). That symptom comes from **who owns time**, not from scheduling. In v3:
- the host daemon is the only clock. It uses `CLOCK_BOOTTIME` on Linux and `CLOCK_MONOTONIC` on
  macOS, which don't jump with NTP or DST changes but keep counting through suspend;
- timers are stored per Claude `session_id`, never per bar position, so a slot shifting left when
  another session closes can't take over its neighbour's timer (there's a unit test for exactly this);
- the device receives finished numbers once per second and only adds animation.

### Serial protocol (host → device, ASCII lines)

```
F <seq> <A|S> <bar> <p1> <p2> <p3> <p4>   bar: W working, Y waiting, I idle, D done, E error ('-' none, '+' overflow)
                                          pN: <0-99|-|_>:<n|s|m|h>:<c|y|r|g|w|m|o|b|x|k>
?            identify → "AGENTIND v3 fw=… w=40 h=6 leds=240 …"
B <0-255>    brightness (default 38 = 15 %)     L <mA>  current cap (default 900 mA)
T [s]        layout test pattern                Z       reboot to USB bootloader
```

At 15 % brightness the firmware also estimates the frame's current draw and scales the whole frame
down if it would exceed the cap. 900 mA is the USB 3 port limit; adjust it with `current_budget_ma`.

## How Claude Code events map to states

| Event | State |
|---|---|
| `SessionStart` | idle (new slot; after `/clear`, the new session takes over the old slot) |
| `UserPromptSubmit`, tool/subagent/compaction activity | working |
| `PermissionRequest`, `Elicitation`, `Notification` (permission / elicitation / agent_needs_input) | waiting (permission; never decays) |
| `Stop`, or a tool interrupted with Esc | waiting (finished turn; becomes idle after 10 min) |
| `StopFailure` (rate_limit, overloaded, …) | error (flashes 10 min, then idle) |
| `SessionEnd` | done (green blink, removed after 4 s) |
| process vanished, no `SessionEnd` | crashed (error) if it was mid-work; otherwise treated as closed |

Activity from background subagents never clears a "waiting for you" state. Only the main thread
or the user can clear it.
