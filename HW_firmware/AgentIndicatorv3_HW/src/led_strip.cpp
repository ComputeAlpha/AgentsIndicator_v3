#include "led_strip.hpp"

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "pico/time.h"
#include "ws2812.pio.h"

static PIO const kPio = pio0;
// WS2812B (newer revisions) need >280 us low to latch.
static constexpr uint64_t kLatchUs = 300;

void LedStrip::init(unsigned pin, size_t count) {
    count_ = count > kMaxLeds ? kMaxLeds : count;
    pin_ = pin;
    offset_ = pio_add_program(kPio, &ws2812_program);
    sm_ = pio_claim_unused_sm(kPio, true);
    set_pin(pin);

    dma_ch_ = dma_claim_unused_channel(true);
    dma_channel_config c = dma_channel_get_default_config(dma_ch_);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(kPio, sm_, true));
    dma_channel_configure(dma_ch_, &c, &kPio->txf[sm_], buf_, count_, false);
}

void LedStrip::set_pin(unsigned pin) {
    pio_sm_set_enabled(kPio, sm_, false);
    pin_ = pin;
    pio_gpio_init(kPio, pin);
    pio_sm_set_consecutive_pindirs(kPio, sm_, pin, 1, true);

    pio_sm_config c = ws2812_program_get_default_config(offset_);
    sm_config_set_sideset_pins(&c, pin);
    sm_config_set_out_shift(&c, false, true, 24);   // MSB first, autopull 24 bits
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);
    const int cycles_per_bit = ws2812_T1 + ws2812_T2 + ws2812_T3;
    const float div = clock_get_hz(clk_sys) / (800000.0f * cycles_per_bit);
    sm_config_set_clkdiv(&c, div);
    pio_sm_init(kPio, sm_, offset_, &c);
    pio_sm_set_enabled(kPio, sm_, true);
}

bool LedStrip::ready() const {
    if (!in_flight_) return true;
    if (dma_channel_is_busy(dma_ch_)) return false;
    // DMA done; wait for the PIO FIFO to drain, then the latch gap.
    if (!pio_sm_is_tx_fifo_empty(kPio, sm_)) return false;
    if (done_at_us_ == 0) {
        // +40 us covers the last word still shifting out of the OSR.
        done_at_us_ = time_us_64() + 40 + kLatchUs;
    }
    if (time_us_64() < done_at_us_) return false;
    in_flight_ = false;
    done_at_us_ = 0;
    return true;
}

void LedStrip::show(const Rgb* px) {
    // Pass 1: scale by brightness and estimate current.
    uint32_t sum = 0;
    for (size_t i = 0; i < count_; ++i) {
        sum += (px[i].r * brightness_ + 127) / 255;
        sum += (px[i].g * brightness_ + 127) / 255;
        sum += (px[i].b * brightness_ + 127) / 255;
    }
    // ~20 mA per channel at 255, plus ~1 mA quiescent per LED.
    uint32_t ma = (sum * 20) / 255 + count_;
    uint32_t scale = 256;
    if (ma > budget_ma_ && ma > count_) {
        scale = ((budget_ma_ - count_) * 256) / (ma - count_);
    }
    last_ma_ = ma;

    for (size_t i = 0; i < count_; ++i) {
        uint32_t r = (px[i].r * brightness_ + 127) / 255;
        uint32_t g = (px[i].g * brightness_ + 127) / 255;
        uint32_t b = (px[i].b * brightness_ + 127) / 255;
        if (scale != 256) {
            r = (r * scale) >> 8;
            g = (g * scale) >> 8;
            b = (b * scale) >> 8;
        }
        buf_[i] = (g << 24) | (r << 16) | (b << 8);   // GRB, MSB first
    }
    in_flight_ = true;
    done_at_us_ = 0;
    dma_channel_set_read_addr(dma_ch_, buf_, false);
    dma_channel_set_trans_count(dma_ch_, count_, true);
}
