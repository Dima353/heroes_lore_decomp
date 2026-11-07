#pragma once
// Switch rumble through libnx: the devkitPro SDL2 joystick driver reports no rumble and its
// calls do nothing, so the port drives the Joy-Con / Pro Controller vibration devices itself.
#include <switch.h>

#include <string>

class SwitchRumble {
public:
    // Starts a pulse (0..1 amplitudes) on handheld and player 1 pads; returns a log line.
    std::string pulse(float low, float high, unsigned ms, unsigned now) {
        std::string info;
        const HidVibrationValue value{low, 160.0f, high, 320.0f};
        for (auto& pad : pads_) {
            if (!prepare(pad)) continue;
            HidVibrationValue values[2]{value, value};
            const Result rc = hidSendVibrationValues(pad.handles, values, pad.count);
            info += std::string(info.empty() ? "" : ", ") + pad.name + (R_SUCCEEDED(rc) ? " ok" : " rc=" + std::to_string(rc));
        }
        until_ = now + ms;
        active_ = true;
        return info.empty() ? std::string("no pad style") : info;
    }
    // Ends the pulse once its time is up (call every frame).
    void update(unsigned now) {
        if (!active_ || int(now - until_) < 0) return;
        active_ = false;
        const HidVibrationValue stop{0.0f, 160.0f, 0.0f, 320.0f};
        for (auto& pad : pads_) {
            if (!pad.count) continue;
            HidVibrationValue values[2]{stop, stop};
            hidSendVibrationValues(pad.handles, values, pad.count);
        }
    }

private:
    struct Pad {
        HidNpadIdType id;
        const char* name;
        u32 style{};
        s32 count{};
        HidVibrationDeviceHandle handles[2]{};
    };
    Pad pads_[2]{{HidNpadIdType_Handheld, "handheld"}, {HidNpadIdType_No1, "player1"}};
    unsigned until_{};
    bool active_{};

    static bool prepare(Pad& pad) {
        const u32 styles = hidGetNpadStyleSet(pad.id);
        HidNpadStyleTag style{};
        s32 count = 1;
        if (styles & HidNpadStyleTag_NpadHandheld) {style = HidNpadStyleTag_NpadHandheld; count = 2;}
        else if (styles & HidNpadStyleTag_NpadJoyDual) {style = HidNpadStyleTag_NpadJoyDual; count = 2;}
        else if (styles & HidNpadStyleTag_NpadFullKey) style = HidNpadStyleTag_NpadFullKey;
        else if (styles & HidNpadStyleTag_NpadJoyLeft) style = HidNpadStyleTag_NpadJoyLeft;
        else if (styles & HidNpadStyleTag_NpadJoyRight) style = HidNpadStyleTag_NpadJoyRight;
        else {pad.count = 0; pad.style = 0; return false;}
        if (pad.style != u32(style) || !pad.count) {
            if (R_FAILED(hidInitializeVibrationDevices(pad.handles, count, pad.id, style))) {pad.count = 0; return false;}
            pad.style = u32(style);
            pad.count = count;
        }
        return true;
    }
};
