// Which mode runs, across a restart of the Cardputer: the USB host can only be
// let go of by restarting, so "try again" after a connection restarts into
// the step it came from.
#pragma once
#include <stdint.h>

namespace modes {

enum Mode : uint32_t { GUIDED = 1, ADVANCED = 2, GUIDED_SWITCH_ON = 3 };

[[noreturn]] void restart_into(Mode m);
// What restart_into asked for; GUIDED after a power-on.
Mode at_boot();

}  // namespace modes
