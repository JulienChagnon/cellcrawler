// Xbox controller via XInput (loaded at runtime so no import library is needed).
#pragma once
#include "mapping.h"

namespace pc {

class Gamepad {
public:
    Gamepad();
    bool available() const { return getState_ != nullptr; }
    // Reads the first connected controller. Cheap enough to call at 1 kHz.
    PadState poll();

private:
    using GetStateFn = unsigned long(__stdcall*)(unsigned long, void*);
    GetStateFn getState_ = nullptr;
    int index_ = 0;
    bool connected_ = false;
    unsigned long long lastProbeUs_ = 0;
};

}  // namespace pc
