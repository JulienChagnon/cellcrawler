#include "gamepad.h"

#include <windows.h>
#include <xinput.h>

#include "util.h"

namespace pc {

Gamepad::Gamepad() {
    for (const char* dll : {"xinput1_4.dll", "xinput1_3.dll", "xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryA(dll)) {
            getState_ = reinterpret_cast<GetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
            if (getState_) break;
        }
    }
}

PadState Gamepad::poll() {
    PadState p;
    if (!getState_) return p;
    // XInputGetState on an empty slot can take milliseconds, so only probe 4x per second.
    const uint64_t now = nowUs();
    if (!connected_ && now - lastProbeUs_ < 250000) return p;
    lastProbeUs_ = now;
    XINPUT_STATE st{};
    if (getState_(DWORD(index_), &st) != ERROR_SUCCESS) {
        // Controller gone or on another slot: probe the next slot next time.
        connected_ = false;
        index_ = (index_ + 1) % XUSER_MAX_COUNT;
        return p;
    }
    connected_ = true;
    const XINPUT_GAMEPAD& g = st.Gamepad;
    p.connected = true;
    p.lx = g.sThumbLX >= 0 ? g.sThumbLX / 32767.0f : g.sThumbLX / 32768.0f;
    p.lt = g.bLeftTrigger / 255.0f;
    p.rt = g.bRightTrigger / 255.0f;
    p.buttons = g.wButtons;
    return p;
}

}  // namespace pc
