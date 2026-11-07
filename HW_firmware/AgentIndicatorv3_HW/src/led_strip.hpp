// Non-blocking WS2812 output: PIO state machine fed by one DMA channel.
// Owned exclusively by core 1 (the render core).
#pragma once
#include <cstdint>
#include <cstddef>

struct Rgb {
    uint8_t r, g, b;
};

class LedStrip {
public:
    static constexpr size_t kMaxLeds = 240;

    // pin: GPIO for DIN. count: number of LEDs in the chain.
    void init(unsigned pin, size_t count);
    void set_pin(unsigned pin);   // re-route the SM to another GPIO (probe use)

    // Global brightness 0..255 applied on output (default ~15%).
    void set_brightness(uint8_t b) { brightness_ = b; }
    uint8_t brightness() const { return brightness_; }

    // Current budget in milliamps for the whole chain (estimated from
    // ~20 mA per channel at full drive). Frames above it are scaled down.
    void set_current_budget_ma(uint32_t ma) { budget_ma_ = ma; }
    uint32_t last_estimated_ma() const { return last_ma_; }

    // True when the previous frame finished and the latch time has elapsed.
    bool ready() const;
    // Start sending a frame (copies + scales into the DMA buffer). Returns
    // immediately; caller should only call when ready().
    void show(const Rgb* px);

    size_t count() const { return count_; }

private:
    unsigned pin_ = 0;
    size_t count_ = 0;
    int dma_ch_ = -1;
    unsigned sm_ = 0;
    unsigned offset_ = 0;
    uint8_t brightness_ = 38;      // 15 % of 255
    uint32_t budget_ma_ = 1500;
    uint32_t last_ma_ = 0;
    mutable uint64_t done_at_us_ = 0;
    mutable bool in_flight_ = false;
    uint32_t buf_[kMaxLeds];
};
