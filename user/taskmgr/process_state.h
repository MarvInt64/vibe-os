#pragma once

#include "../vexui.h"

/* Mirrors the kernel's process state constants (kernel/src/process.h).
 * Kept as a scoped enum so callers cannot accidentally mix it with raw ints. */
enum class ProcessState : unsigned char {
    Empty     = 0,
    Ready     = 1,
    Running   = 2,
    Sleeping  = 3,
    Waiting   = 4,
    Exited    = 5,
    WaitingIO = 6,
};

/* Returns a short uppercase label for display in the State column. */
inline const char *state_name(ProcessState s) {
    switch (s) {
        case ProcessState::Running:   return "RUNNING";
        case ProcessState::Ready:     return "READY";
        case ProcessState::Sleeping:  return "SLEEP";
        case ProcessState::Waiting:   return "WAIT";
        case ProcessState::WaitingIO: return "IOWAIT";
        case ProcessState::Exited:    return "EXIT";
        default:                      return "EMPTY";
    }
}

/* Returns the theme colour associated with a given process state. */
inline vui_u32 state_color(ProcessState s) {
    switch (s) {
        case ProcessState::Running:   return VUI_ACCENT;
        case ProcessState::Ready:     return VUI_OK;
        case ProcessState::Sleeping:  return VUI_WARN;
        case ProcessState::Waiting:
        case ProcessState::WaitingIO: return 0x006bcdf0u;
        case ProcessState::Exited:    return VUI_DANGER;
        default:                      return VUI_TEXT_DIM;
    }
}

/* Returns true for states that should be shown as an active table row. */
inline bool state_is_active(ProcessState s) {
    return s != ProcessState::Empty && s != ProcessState::Exited;
}
