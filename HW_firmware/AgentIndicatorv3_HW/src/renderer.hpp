// Turns a disp::Model into pixels. Runs only on core 1. All animation timing
// comes from the local microsecond timer, so it is smooth regardless of when
// host frames arrive.
#pragma once
#include <cstdint>

#include "display_model.hpp"
#include "led_strip.hpp"
#include "layout.hpp"

class Renderer {
public:
    enum class Screen { Boot, Live, NoLink, Starry, Test };

    // t_ms: monotonic milliseconds. Writes the full framebuffer.
    void render(Screen screen, const disp::Model& m, uint32_t t_ms, Rgb* fb);

private:
    void set(int x, int y, Rgb c);
    void draw_bar(const disp::Model& m, uint32_t t);
    void draw_slot(disp::Slot s, int x0, int w, int idx, uint32_t t);
    void draw_panel(int p, const disp::PanelValue& v, uint32_t t);
    void draw_digit(int x0, int digit, Rgb (*shade)(int x, int y, uint32_t t, disp::Colour c),
                    disp::Colour c, uint32_t t);
    void draw_starry(uint32_t t);
    void draw_boot(uint32_t t);
    void draw_test(uint32_t t);

    Rgb* fb_ = nullptr;

    // starry night: per-pixel star phase (0 = dark)
    struct Star { uint16_t age; uint16_t life; uint8_t hue; };
    Star stars_[layout::kLedCount] = {};
    uint32_t last_star_t_ = 0;
    uint32_t rng_ = 0x12345678;
    uint32_t rand();
};
