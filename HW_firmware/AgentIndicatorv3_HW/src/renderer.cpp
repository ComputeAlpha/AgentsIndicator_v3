#include "renderer.hpp"

#include <cstring>

using disp::Colour;
using disp::PanelValue;
using disp::Slot;
using disp::Unit;

namespace {

// 3x5 font, bit 2 = left column. Rows top to bottom.
constexpr uint8_t kFont[10][5] = {
    {7, 5, 5, 5, 7}, {2, 6, 2, 2, 7}, {7, 1, 7, 4, 7}, {7, 1, 7, 1, 7}, {5, 5, 7, 1, 1},
    {7, 4, 7, 1, 7}, {7, 4, 7, 5, 7}, {7, 1, 1, 1, 1}, {7, 5, 7, 5, 7}, {7, 5, 7, 1, 7},
};
constexpr int kDigitX[2] = {1, 5};   // panel-local columns of the two digits
constexpr int kDotsX = 9;            // unit dots column; adjacent rows 4,3,2 (bottom-up)

constexpr Rgb kCyan{0, 255, 255}, kYellow{255, 170, 0}, kRed{255, 0, 0}, kGreen{0, 255, 0},
    kWhite{255, 255, 255}, kMagenta{255, 0, 200}, kOrange{255, 80, 0}, kBlue{0, 60, 255};

Rgb scale(Rgb c, uint32_t s /*0..256*/) {
    return {(uint8_t)((c.r * s) >> 8), (uint8_t)((c.g * s) >> 8), (uint8_t)((c.b * s) >> 8)};
}

Rgb hue(uint8_t h) {
    uint8_t region = h / 43, rem = (h - region * 43) * 6;
    uint8_t q = 255 - rem, t = rem;
    switch (region) {
    case 0: return {255, t, 0};
    case 1: return {q, 255, 0};
    case 2: return {0, 255, t};
    case 3: return {0, q, 255};
    case 4: return {t, 0, 255};
    default: return {255, 0, q};
    }
}

// Triangle wave 0..256 with the given period.
uint32_t tri(uint32_t t, uint32_t period) {
    uint32_t ph = t % period;
    uint32_t half = period / 2;
    return ph < half ? (ph * 256) / half : ((period - ph) * 256) / half;
}

Rgb colour_of(Colour c) {
    switch (c) {
    case Colour::Cyan: return kCyan;
    case Colour::Yellow: return kYellow;
    case Colour::Red: return kRed;
    case Colour::Green: return kGreen;
    case Colour::White: return kWhite;
    case Colour::Magenta: return kMagenta;
    case Colour::Orange: return kOrange;
    case Colour::Blue: return kBlue;
    default: return {0, 0, 0};
    }
}

// Panel pixel shader: solid colours, or a slowly drifting rainbow (anti burn-in).
Rgb panel_shade(int x, int y, uint32_t t, Colour c) {
    if (c == Colour::Rainbow) return hue((uint8_t)(t / 60 + x * 6 + y * 3));
    return colour_of(c);
}

}  // namespace

uint32_t Renderer::rand() {
    rng_ ^= rng_ << 13;
    rng_ ^= rng_ >> 17;
    rng_ ^= rng_ << 5;
    return rng_;
}

void Renderer::set(int x, int y, Rgb c) {
    if (x < 0 || x >= disp::kWidth || y < 0 || y >= disp::kHeight) return;
    fb_[layout::index_of(x, y)] = c;
}

void Renderer::render(Screen screen, const disp::Model& m, uint32_t t, Rgb* fb) {
    fb_ = fb;
    memset(fb, 0, sizeof(Rgb) * layout::kLedCount);
    switch (screen) {
    case Screen::Boot: draw_boot(t); break;
    case Screen::Test: draw_test(t); break;
    case Screen::Starry: draw_starry(t); break;
    case Screen::NoLink: {
        // "--" on every panel in dim white, bar dark, one slow red blink.
        PanelValue dash{PanelValue::kDashes, Unit::None, Colour::White};
        for (int p = 0; p < disp::kPanels; ++p) draw_panel(p, dash, t);
        for (auto* px = fb; px < fb + layout::kLedCount; ++px) *px = scale(*px, 60);
        if ((t % 2000) < 150) set(disp::kWidth - 1, disp::kBarY, scale(kRed, 120));
        break;
    }
    case Screen::Live:
        if (m.starry) { draw_starry(t); break; }
        for (int p = 0; p < disp::kPanels; ++p) draw_panel(p, m.panels[p], t);
        draw_bar(m, t);
        break;
    }
    if (screen != Screen::Starry && !(screen == Screen::Live && m.starry)) {
        // leaving starry: forget stars so they restart cleanly next time
        memset(stars_, 0, sizeof stars_);
    }
}

// ---------------------------------------------------------------- status bar

void Renderer::draw_bar(const disp::Model& m, uint32_t t) {
    const int n = m.nslots;
    if (n == 0) return;
    const int gap = (n > 1 && disp::kWidth / n >= 3) ? 1 : 0;
    const int usable = disp::kWidth - gap * (n - 1);
    const int base = usable / n, rem = usable % n;
    int x = 0;
    for (int i = 0; i < n; ++i) {
        const int w = base + (i < rem ? 1 : 0);
        draw_slot(m.slots[i], x, w, i, t);
        x += w + gap;
    }
    if (m.overflow && (t % 1000) < 500) set(disp::kWidth - 1, disp::kBarY, kWhite);
}

void Renderer::draw_slot(Slot s, int x0, int w, int idx, uint32_t t) {
    const int y = disp::kBarY;
    switch (s) {
    case Slot::Working: {
        // cyan comet running left -> right over a dim cyan bed
        if (w == 1) { set(x0, y, scale(kCyan, 60 + (tri(t, 1000) * 196 >> 8))); break; }
        const uint32_t step = 90;
        const uint32_t period = step * (uint32_t)(w + 2);
        const int head = (int)((t % period) / step);   // 0..w+1 (runs off the end)
        for (int i = 0; i < w; ++i) {
            const int d = head - i;
            uint32_t lvl = 40;
            if (d == 0) lvl = 256;
            else if (d == 1) lvl = 120;
            else if (d == 2) lvl = 70;
            set(x0 + i, y, scale(kCyan, lvl));
        }
        break;
    }
    case Slot::Waiting: {
        // yellow scanner bouncing inside the slot
        if (w <= 2) {
            for (int i = 0; i < w; ++i) set(x0 + i, y, scale(kYellow, 40 + (tri(t, 1400) * 216 >> 8)));
            break;
        }
        const uint32_t step = 110;
        const uint32_t period = step * (uint32_t)(2 * (w - 1));
        int pos = (int)((t % period) / step);
        if (pos >= w) pos = 2 * (w - 1) - pos;
        for (int i = 0; i < w; ++i) {
            const int d = pos > i ? pos - i : i - pos;
            set(x0 + i, y, scale(kYellow, d == 0 ? 256 : d == 1 ? 70 : 25));
        }
        break;
    }
    case Slot::Idle: {
        // dim red bed, short bright flash every 2.5 s (staggered per slot)
        const uint32_t ph = (t + (uint32_t)idx * 310) % 2500;
        const uint32_t lvl = ph < 140 ? 256 : 18;
        for (int i = 0; i < w; ++i) set(x0 + i, y, scale(kRed, lvl));
        break;
    }
    case Slot::Done: {
        const bool on = (t % 400) < 220;
        for (int i = 0; i < w; ++i) set(x0 + i, y, on ? kGreen : Rgb{0, 0, 0});
        break;
    }
    case Slot::Error: {
        const bool on = (t % 125) < 62;   // 8 Hz
        for (int i = 0; i < w; ++i) set(x0 + i, y, on ? kRed : Rgb{0, 0, 0});
        break;
    }
    case Slot::Empty: break;
    }
}

// --------------------------------------------------------------- digit panels

void Renderer::draw_digit(int x0, int digit, Rgb (*shade)(int, int, uint32_t, Colour), Colour c,
                          uint32_t t) {
    for (int row = 0; row < 5; ++row) {
        const uint8_t bits = digit < 0 ? (row == 2 ? 7 : 0) : kFont[digit][row];
        for (int col = 0; col < 3; ++col)
            if (bits & (4 >> col)) set(x0 + col, row, shade(x0 + col, row, t, c));
    }
}

void Renderer::draw_panel(int p, const PanelValue& v, uint32_t t) {
    if (v.value == PanelValue::kBlank || v.colour == Colour::Off) return;
    const int px = p * disp::kPanelW;
    if (v.value == PanelValue::kDashes) {
        draw_digit(px + kDigitX[0], -1, panel_shade, v.colour, t);
        draw_digit(px + kDigitX[1], -1, panel_shade, v.colour, t);
    } else {
        const int tens = v.value / 10, ones = v.value % 10;
        if (tens) draw_digit(px + kDigitX[0], tens, panel_shade, v.colour, t);
        draw_digit(px + kDigitX[1], ones, panel_shade, v.colour, t);
    }
    // unit dots, stacked bottom-up with no gap (as on v2): s = 1, m = 2, h = 3
    const int dots = v.unit == Unit::Sec ? 1 : v.unit == Unit::Min ? 2 : v.unit == Unit::Hour ? 3 : 0;
    for (int i = 0; i < dots; ++i) {
        const int y = 4 - i;
        set(px + kDotsX, y, scale(panel_shade(px + kDotsX, y, t, v.colour), 160));
    }
}

// -------------------------------------------------------------- full screens

void Renderer::draw_starry(uint32_t t) {
    // Mostly dark sky; a few pixels twinkle in and out with random colours.
    uint32_t dt = t - last_star_t_;
    if (dt > 100) dt = 100;
    last_star_t_ = t;
    int alive = 0;
    for (auto& s : stars_) {
        if (!s.life) continue;
        s.age += dt;
        if (s.age >= s.life) s = {};
        else ++alive;
    }
    // spawn ~4 stars per second, at most 14 lit at once
    if (alive < 14 && (rand() % 1000) < dt * 4) {
        auto& s = stars_[rand() % layout::kLedCount];
        if (!s.life) s = {0, (uint16_t)(1200 + rand() % 2400), (uint8_t)rand()};
    }
    for (int i = 0; i < layout::kLedCount; ++i) {
        const auto& s = stars_[i];
        if (!s.life) continue;
        const uint32_t half = s.life / 2;
        const uint32_t lvl = s.age < half ? (s.age * 256) / half : ((s.life - s.age) * 256) / half;
        fb_[i] = scale(hue(s.hue), lvl);
    }
}

void Renderer::draw_boot(uint32_t t) {
    // rainbow wipe across the bar
    const int lit = (int)(t / 25);
    for (int x = 0; x < disp::kWidth && x < lit; ++x) set(x, disp::kBarY, hue((uint8_t)(x * 6)));
}

void Renderer::draw_test(uint32_t t) {
    // Panels read "12 34 56 78" with 1/2/3/0 unit dots; bar fades red -> blue
    // left to right with a white pixel at the far left.
    const Colour cols[4] = {Colour::Red, Colour::Green, Colour::Blue, Colour::Rainbow};
    const Unit units[4] = {Unit::Sec, Unit::Min, Unit::Hour, Unit::None};
    for (int p = 0; p < disp::kPanels; ++p)
        draw_panel(p, PanelValue{(int8_t)((p * 2 + 1) * 10 + p * 2 + 2), units[p], cols[p]}, t);
    for (int x = 1; x < disp::kWidth; ++x)
        set(x, disp::kBarY, Rgb{(uint8_t)(255 - x * 6), 0, (uint8_t)(x * 6)});
    set(0, disp::kBarY, kWhite);
}
