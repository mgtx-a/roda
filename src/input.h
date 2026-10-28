// Noticing a file drag and the trigger keys, without any keyboard or mouse hook.
//
// Raw mouse input says when the button goes down and the pointer moves. When a drag may
// have started, a tiny invisible drop target (the probe) sits under the pointer: if an
// OLE drag carrying files is under way, the drag source tells the probe so. Only then are
// the trigger keys read, and only until the button goes up.
#pragma once
#include "common.h"
#include "wheel.h"

namespace rd {

class ProbeTarget;

class Trigger {
public:
    Trigger(const Settings& settings, Wheel& wheel);
    ~Trigger();
    bool Create(HINSTANCE inst);

    /// Raw input on/off (the tray's Pause).
    void Enable(bool on);
    bool enabled() const { return enabled_; }

    void OnRawInput(HRAWINPUT h);     // from WM_INPUT on the message window
    bool NeedsTick() const;           // the keys are polled only during a file drag
    double NextTickIn() const;
    void Tick();

    // Called by the probe's drop target.
    void OnProbeEnter(IDataObject* data);

private:
    enum class State { Idle, Pressed, Probing, FileDrag, Ignored };

    void ButtonDown(POINT p);
    void ButtonUp();
    void Moved(POINT p);
    void ShowProbe(POINT p);
    void HideProbe();
    void CheckKeys();
    bool PrimaryDown() const;
    static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l);

    const Settings& settings_;
    Wheel& wheel_;
    HWND probe_ = nullptr;
    ProbeTarget* target_ = nullptr;
    bool enabled_ = false;
    State state_ = State::Idle;
    POINT down_{};
    double probeSince = 0, nextTick_ = 0;
    std::vector<wstring> paths_;
    bool keyWasDown_[3]{};            // Convert, Tools, Actions: as last seen
};

}  // namespace rd
