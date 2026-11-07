// Host -> device line protocol (ASCII, '\n' terminated, human-typeable).
//
//   F <seq> <mode> <bar> <p0> <p1> <p2> <p3>
//       mode : A = normal, S = starry night (idle screensaver)
//       bar  : '-' for no sessions, else one char per session, left to right:
//              W working, Y waiting for human, I idle, D done (closed), E error.
//              A trailing '+' means there were more sessions than pixels.
//       pN   : <value>:<unit>:<colour>
//              value  0..99, '-' shows "--", '_' blank
//              unit   n none, s seconds, m minutes, h hours (1/2/3 dots)
//              colour c cyan, y yellow, r red, g green, w white, m magenta,
//                     o orange, b blue, x rainbow, k off
//   ?             identify  -> "AGENTIND v3 fw=<ver> w=40 h=6 leds=240"
//   B <0..255>    global brightness (default 38 = 15 %)
//   L <mA>        current budget for the whole matrix
//   T [seconds]   layout test pattern
//   Z             reboot into the USB bootloader (for flashing)
//
// Only '?' and errors produce output, so a host that never reads can't stall
// the device.
#pragma once
#include "display_model.hpp"

namespace proto {

constexpr const char* kFirmwareVersion = "1.1.0";

// Parses an "F ..." line (without the leading 'F'). Returns false on error.
bool parse_frame(const char* args, disp::Model& out);

}  // namespace proto
