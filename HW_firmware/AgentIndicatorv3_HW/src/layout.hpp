// Physical pixel mapping, determined experimentally on the v3 build (2026-11-07):
//  * 4 Seeed 6x10 XIAO matrix panels in a single row, XIAO panel leftmost and
//    first in the data chain (GPIO26 / D0 -> panel0 -> panel1 -> ...).
//  * Inside a panel the LEDs run column-major, progressive (not serpentine):
//    local 0 is bottom-left, 0..5 go up column 0, 6..11 go up column 1, ...
//    local 59 is top-right.
#pragma once
#include "display_model.hpp"

namespace layout {

constexpr int kLedsPerPanel = 60;
constexpr int kRowsPerPanel = 6;
constexpr int kLedCount = disp::kPanels * kLedsPerPanel;

// x: 0..39 left->right, y: 0..5 top->bottom (screen coordinates).
constexpr int index_of(int x, int y) {
    const int panel = x / disp::kPanelW;
    const int col = x % disp::kPanelW;
    const int row_from_bottom = (kRowsPerPanel - 1) - y;
    return panel * kLedsPerPanel + col * kRowsPerPanel + row_from_bottom;
}

static_assert(index_of(0, 5) == 0, "bottom-left of panel 0 is LED 0");
static_assert(index_of(0, 4) == 1, "LED 1 is directly above LED 0");
static_assert(index_of(1, 2) == 9, "LED 9: column 2, 4th row from bottom");
static_assert(index_of(1, 1) == 10, "LED 10 sits right above LED 9");
static_assert(index_of(9, 0) == 59, "top-right of panel 0 is LED 59");
static_assert(index_of(10, 5) == 60, "panel 1 starts bottom-left");

}  // namespace layout
