// AgentIndicator v3 firmware - XIAO RP2350 + 4x Seeed 6x10 WS2812 matrix.
//
// Core split (no RTOS; each core runs one cooperative loop and never waits on
// the other):
//   core 0 : USB CDC (TinyUSB via pico_stdio_usb, IRQ driven), line assembly,
//            protocol parsing, watchdog supervision.
//   core 1 : renderer at a fixed 60 fps + PIO/DMA LED output.
// The only shared data is a lock-protected queue of complete disp::Model
// snapshots (core 0 -> core 1) plus a few single-word atomics. A model is
// either fully old or fully new, never half written.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "display_model.hpp"
#include "hardware/watchdog.h"
#include "layout.hpp"
#include "led_strip.hpp"
#include "pico/bootrom.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "pico/util/queue.h"
#include "protocol.hpp"
#include "renderer.hpp"

namespace {

constexpr unsigned kDataPin = 26;                    // XIAO D0
constexpr unsigned kRxLedPin = PICO_DEFAULT_LED_PIN; // GPIO25, yellow user LED, active-low
constexpr uint32_t kRxLedHoldMs = 30;                // blink length after the last byte
constexpr uint32_t kFrameUs = 16667;                 // 60 fps
constexpr uint32_t kNoLinkMs = 10'000;               // host silent -> "--"
constexpr uint32_t kStarryAfterNoLinkMs = 30 * 60'000;
constexpr uint32_t kBootAnimMs = 1200;

queue_t g_models;                                    // core0 -> core1
std::atomic<uint32_t> g_brightness{38};              // 15 %
std::atomic<uint32_t> g_budget_ma{900};
std::atomic<uint32_t> g_test_until_ms{0};
std::atomic<uint32_t> g_core1_frames{0};

LedStrip g_strip;
Renderer g_renderer;
Rgb g_fb[layout::kLedCount];

uint32_t now_ms() { return (uint32_t)(time_us_64() / 1000); }

void core1_main() {
    disp::Model model{};
    bool have_model = false;
    uint32_t last_model_ms = 0;
    uint64_t next_frame = time_us_64();

    while (true) {
        // newest snapshot wins; older ones are just dropped
        disp::Model incoming;
        while (queue_try_remove(&g_models, &incoming)) {
            model = incoming;
            have_model = true;
            last_model_ms = now_ms();
        }

        const uint32_t t = now_ms();
        Renderer::Screen screen;
        if (t < g_test_until_ms.load(std::memory_order_relaxed)) screen = Renderer::Screen::Test;
        else if (!have_model)
            screen = t < kBootAnimMs ? Renderer::Screen::Boot
                     : t > kStarryAfterNoLinkMs ? Renderer::Screen::Starry
                                                : Renderer::Screen::NoLink;
        else if (t - last_model_ms > kStarryAfterNoLinkMs) screen = Renderer::Screen::Starry;
        else if (t - last_model_ms > kNoLinkMs) screen = Renderer::Screen::NoLink;
        else screen = Renderer::Screen::Live;

        g_renderer.render(screen, model, t, g_fb);

        while (!g_strip.ready()) tight_loop_contents();   // <8 ms, DMA does the work
        g_strip.set_brightness((uint8_t)g_brightness.load(std::memory_order_relaxed));
        g_strip.set_current_budget_ma(g_budget_ma.load(std::memory_order_relaxed));
        g_strip.show(g_fb);
        g_core1_frames.fetch_add(1, std::memory_order_relaxed);

        next_frame += kFrameUs;
        const uint64_t now = time_us_64();
        if (next_frame > now) sleep_until(from_us_since_boot(next_frame));
        else next_frame = now;   // fell behind; don't try to catch up
    }
}

void publish(const disp::Model& m) {
    if (!queue_try_add(&g_models, &m)) {
        disp::Model drop;
        queue_try_remove(&g_models, &drop);   // keep only the newest
        queue_try_add(&g_models, &m);
    }
}

void handle_line(char* line) {
    switch (line[0]) {
    case 'F': {
        disp::Model m;
        if (proto::parse_frame(line + 1, m)) publish(m);
        else printf("ERR frame\n");
        break;
    }
    case '?':
        printf("AGENTIND v3 fw=%s w=%d h=%d leds=%d bright=%lu budget=%lu\n", proto::kFirmwareVersion,
               disp::kWidth, disp::kHeight, layout::kLedCount, (unsigned long)g_brightness.load(),
               (unsigned long)g_budget_ma.load());
        break;
    case 'B': {
        long v = strtol(line + 1, nullptr, 10);
        if (v >= 0 && v <= 255) g_brightness = (uint32_t)v;
        break;
    }
    case 'L': {
        long v = strtol(line + 1, nullptr, 10);
        if (v >= 100 && v <= 5000) g_budget_ma = (uint32_t)v;
        break;
    }
    case 'T': {
        long s = strtol(line + 1, nullptr, 10);
        if (s <= 0 || s > 600) s = 20;
        g_test_until_ms = now_ms() + (uint32_t)s * 1000;
        break;
    }
    case 'Z':
        sleep_ms(50);
        reset_usb_boot(0, 0);
        break;
    default:
        printf("ERR unknown\n");
    }
}

}  // namespace

int main() {
    stdio_init_all();
    queue_init(&g_models, sizeof(disp::Model), 4);
    g_strip.init(kDataPin, layout::kLedCount);
    multicore_launch_core1(core1_main);

    // Reset if either core stops making progress for 2 s.
    watchdog_enable(2000, true);
    uint32_t last_frames = 0, last_progress_ms = now_ms();

    // RX activity LED: lit only while bytes are arriving from the host.
    gpio_init(kRxLedPin);
    gpio_set_dir(kRxLedPin, GPIO_OUT);
    gpio_put(kRxLedPin, 1);   // off
    uint32_t rx_led_off_ms = 0;
    bool rx_led_on = false;

    char line[160];
    size_t len = 0;
    bool overflowed = false;
    while (true) {
        const int ch = getchar_timeout_us(5'000);
        if (ch != PICO_ERROR_TIMEOUT) {
            if (!rx_led_on) { gpio_put(kRxLedPin, 0); rx_led_on = true; }
            rx_led_off_ms = now_ms() + kRxLedHoldMs;
            if (ch == '\n' || ch == '\r') {
                if (len && !overflowed) {
                    line[len] = 0;
                    handle_line(line);
                }
                len = 0;
                overflowed = false;
            } else if (len < sizeof line - 1) {
                line[len++] = (char)ch;
            } else {
                overflowed = true;   // drop over-long line entirely
            }
        }

        const uint32_t frames = g_core1_frames.load(std::memory_order_relaxed);
        const uint32_t t = now_ms();
        if (rx_led_on && (int32_t)(t - rx_led_off_ms) >= 0) { gpio_put(kRxLedPin, 1); rx_led_on = false; }
        if (frames != last_frames) {
            last_frames = frames;
            last_progress_ms = t;
        }
        if (t - last_progress_ms < 1000) watchdog_update();   // core1 alive too
    }
}
