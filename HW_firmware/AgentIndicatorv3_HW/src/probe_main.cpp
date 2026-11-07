// Layout-discovery probe firmware.
// core0: USB CDC command shell.  core1: pushes the framebuffer at ~50 fps.
//
// Commands (one per line):
//   ?                 identify
//   c                 clear
//   p <i> [r g b]     light raw index i (default white)
//   r <a> <b> [r g b] light raw index range a..b inclusive
//   f <r> <g> <b>     fill all
//   k <panel>         light whole panel (60 LEDs) panel 0..3, each its own colour
//   m                 marker pattern on every panel (see help)
//   w [ms]            walk a single pixel through all LEDs (0 = stop)
//   n <count>         number of LEDs in chain (<=240)
//   g <gpio>          move data output to another GPIO
//   b <0..255>        brightness (default 38 = 15 %)
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "led_strip.hpp"
#include "pico/multicore.h"
#include "pico/stdlib.h"

static constexpr unsigned kDefaultPin = 26;   // XIAO D0
static LedStrip strip;
static Rgb fb[LedStrip::kMaxLeds];
static volatile bool fb_dirty = true;
static volatile int walk_ms = 0;

static void core1_main() {
    int walk_i = -1;
    uint64_t next_walk = 0;
    while (true) {
        if (walk_ms > 0 && time_us_64() >= next_walk) {
            if (walk_i >= 0) fb[walk_i] = {0, 0, 0};
            walk_i = (walk_i + 1) % (int)strip.count();
            fb[walk_i] = {255, 255, 255};
            printf("walk %d (panel %d, local %d)\n", walk_i, walk_i / 60, walk_i % 60);
            next_walk = time_us_64() + (uint64_t)walk_ms * 1000;
            fb_dirty = true;
        }
        if (fb_dirty && strip.ready()) {
            fb_dirty = false;
            strip.show(fb);
        }
        sleep_us(500);
    }
}

static void clear() { memset(fb, 0, sizeof fb); }

static const Rgb kPanelColour[4] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 160, 0}};

static void handle(char* line) {
    char cmd = line[0];
    int a[6] = {0};
    int n = sscanf(line + 1, "%d %d %d %d %d %d", &a[0], &a[1], &a[2], &a[3], &a[4], &a[5]);
    switch (cmd) {
    case '?':
        printf("AGENTIND-PROBE v3 leds=%u pin=%u bright=%u\n", (unsigned)strip.count(),
               kDefaultPin, strip.brightness());
        break;
    case 'c': walk_ms = 0; clear(); break;
    case 'p':
        if (n >= 1 && a[0] >= 0 && a[0] < (int)strip.count())
            fb[a[0]] = n >= 4 ? Rgb{(uint8_t)a[1], (uint8_t)a[2], (uint8_t)a[3]} : Rgb{255, 255, 255};
        break;
    case 'r':
        for (int i = a[0]; n >= 2 && i <= a[1] && i < (int)strip.count(); ++i)
            fb[i] = n >= 5 ? Rgb{(uint8_t)a[2], (uint8_t)a[3], (uint8_t)a[4]} : Rgb{255, 255, 255};
        break;
    case 'f':
        for (size_t i = 0; i < strip.count(); ++i) fb[i] = {(uint8_t)a[0], (uint8_t)a[1], (uint8_t)a[2]};
        break;
    case 'k':
        if (n >= 1 && a[0] >= 0 && a[0] < 4)
            for (int i = a[0] * 60; i < a[0] * 60 + 60 && i < (int)strip.count(); ++i) fb[i] = kPanelColour[a[0]];
        break;
    case 'm':
        // Per panel: local 0 RED, 1 GREEN, 2 GREEN, 9 BLUE, 10 YELLOW, 59 WHITE
        clear();
        for (size_t base = 0; base < strip.count(); base += 60) {
            fb[base + 0] = {255, 0, 0};
            fb[base + 1] = {0, 255, 0};
            fb[base + 2] = {0, 255, 0};
            fb[base + 9] = {0, 0, 255};
            fb[base + 10] = {255, 200, 0};
            fb[base + 59] = {255, 255, 255};
        }
        break;
    case 'w': walk_ms = n >= 1 ? a[0] : 300; if (!walk_ms) clear(); break;
    case 'n': if (n >= 1 && a[0] > 0 && a[0] <= 240) { clear(); /* count fixed at init */ } break;
    case 'g': if (n >= 1 && a[0] >= 0 && a[0] < 30) strip.set_pin(a[0]); break;
    case 'b': if (n >= 1) strip.set_brightness((uint8_t)a[0]); break;
    default: printf("ERR unknown\n"); return;
    }
    fb_dirty = true;
    printf("OK\n");
}

int main() {
    stdio_init_all();
    strip.init(kDefaultPin, 240);
    clear();
    multicore_launch_core1(core1_main);

    char line[96];
    size_t len = 0;
    while (true) {
        int ch = getchar_timeout_us(100000);
        if (ch == PICO_ERROR_TIMEOUT) continue;
        if (ch == '\r' || ch == '\n') {
            if (len) {
                line[len] = 0;
                handle(line);
                len = 0;
            }
        } else if (len < sizeof line - 1) {
            line[len++] = (char)ch;
        }
    }
}
