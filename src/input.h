// Noticing a file drag and the trigger keys, without any keyboard or mouse hook.
//
// Raw mouse input says when the button goes down and the pointer moves. When a drag may
// have started, Roda looks at which window holds the mouse capture: during an OLE drag it
// is OLE's own hidden window. Only then does a tiny invisible drop target (the probe) sit
// under the pointer, and the drag source tells it whether files are being dragged. Only
// then are the trigger keys read, and only until the button goes up.
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
    enum class State { Idle, Pressed, Watching, Probing, FileDrag, Ignored };

    void ButtonDown(POINT p);
    void ButtonUp();
    void Moved(POINT p);
    void ShowProbe(POINT p);
    void HideProbe();
    void CheckKeys();
    void CheckCapture();
    void GiveUp(const wchar_t* why);
    void CountCpu();
    bool PrimaryDown() const;
    static LRESULT CALLBACK ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l);

    const Settings& settings_;
    Wheel& wheel_;
    HWND probe_ = nullptr;
    ProbeTarget* target_ = nullptr;
    bool enabled_ = false;
    State state_ = State::Idle;
    POINT down_{};
    double dragSince_ = 0, nextTick_ = 0;
    DWORD srcThread_ = 0;             // the thread that owns the window the drag started on
    HWND lastCapture_ = nullptr;      // as last logged, so each change is logged once
    int captureLogs_ = 0;
    double cpuSince_ = -1;            // a minute of mouse movement, for the CPU log
    uint64_t cpuAtStart_ = 0;
    int rawEvents_ = 0;
    std::vector<wstring> paths_;
    bool keyWasDown_[3]{};            // Convert, Tools, Actions: as last seen
    bool spaceWasDown_ = false, tabWasDown_ = false;
};

}  // namespace rd
