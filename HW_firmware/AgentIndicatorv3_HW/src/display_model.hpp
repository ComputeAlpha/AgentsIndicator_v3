// The complete description of what should be on the display, as sent by the
// host. It carries no clocks: every duration is computed on the host and
// arrives here as a finished number, so the device can never drift or carry a
// timer over from one session to another. The device only adds animation.
#pragma once
#include <cstdint>

namespace disp {

constexpr int kWidth = 40;          // 4 panels x 10 columns
constexpr int kHeight = 6;
constexpr int kPanels = 4;
constexpr int kPanelW = 10;
constexpr int kBarY = kHeight - 1;  // bottom row is the session status bar
constexpr int kMaxSlots = kWidth;

enum class Slot : uint8_t { Empty, Working, Waiting, Idle, Done, Error };

enum class Unit : uint8_t { None, Sec, Min, Hour };

enum class Colour : uint8_t { Off, Cyan, Yellow, Red, Green, White, Magenta, Orange, Blue, Rainbow };

struct PanelValue {
    static constexpr int8_t kBlank = -1;
    static constexpr int8_t kDashes = -2;
    int8_t value = kBlank;          // 0..99, or kBlank / kDashes
    Unit unit = Unit::None;
    Colour colour = Colour::Off;
};

struct Model {
    uint32_t seq = 0;
    bool starry = false;            // host asks for the idle "starry night"
    bool overflow = false;          // more sessions than bar pixels
    uint8_t nslots = 0;
    Slot slots[kMaxSlots] = {};
    PanelValue panels[kPanels];
};

}  // namespace disp
