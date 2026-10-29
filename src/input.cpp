#include "input.h"

#include "dragdrop.h"

namespace rd {

namespace {

constexpr double kProbeWindow = 1.5;    // how long after the drag threshold a drag source may take to show itself (Explorer's ZIP folders take ~0.4 s)
constexpr double kKeyPoll = 0.016;      // the keys are read this often during a file drag, never otherwise
constexpr int kProbeSize = 48;          // pixels: big enough that a quick flick stays over it
// DoDragDrop gives the mouse capture to this hidden OLE window for the whole drag. A text
// selection, a window move or an app's own dragging keeps the capture on the app's window.
constexpr wchar_t kOleDragClass[] = L"CLIPBRDWNDCLASS";
constexpr int kMaxCaptureLogs = 4;      // per drag

uint64_t ProcessCpu100ns()
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u)) return 0;
    auto v = [](FILETIME f) { return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return v(k) + v(u);
}

}  // namespace

/// The probe's drop target: it only looks at what is being dragged, then steps aside.
class ProbeTarget : public DropTarget {
public:
    Trigger* trigger = nullptr;
    STDMETHODIMP DragEnter(IDataObject* data, DWORD, POINTL, DWORD* effect) override
    {
        if (trigger) trigger->OnProbeEnter(data);
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
    STDMETHODIMP DragOver(DWORD, POINTL, DWORD* effect) override { *effect = DROPEFFECT_NONE; return S_OK; }
    STDMETHODIMP DragLeave() override { return S_OK; }
    STDMETHODIMP Drop(IDataObject*, DWORD, POINTL, DWORD* effect) override
    {
        // Only reachable if the button went up in the instant the probe was under the pointer.
        Log(L"drag: dropped on the probe before it could step aside; nothing done");
        *effect = DROPEFFECT_NONE;
        return S_OK;
    }
};

Trigger::Trigger(const Settings& settings, Wheel& wheel) : settings_(settings), wheel_(wheel) {}

Trigger::~Trigger()
{
    Enable(false);
    if (probe_) {
        RevokeDragDrop(probe_);
        DestroyWindow(probe_);
    }
    if (target_) { target_->trigger = nullptr; target_->Release(); }
}

LRESULT CALLBACK Trigger::ProbeProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    return DefWindowProcW(h, m, w, l);
}

bool Trigger::Create(HINSTANCE inst)
{
    WNDCLASSW wc{};
    wc.lpfnWndProc = ProbeProc;
    wc.hInstance = inst;
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = L"RodaProbe";
    RegisterClassW(&wc);
    probe_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP,
                             -10000, -10000, kProbeSize, kProbeSize, nullptr, nullptr, inst, nullptr);
    if (!probe_) return false;
    // Alpha 1 of 255: invisible, but still a window the drag source finds under the pointer.
    // (Alpha 0 would make it click- and drop-through.)
    SetLayeredWindowAttributes(probe_, 0, 1, LWA_ALPHA);
    target_ = new ProbeTarget();
    target_->trigger = this;
    HRESULT hr = RegisterDragDrop(probe_, target_);
    if (FAILED(hr)) { Log(L"drag: the probe could not accept drags (0x%08lx)", hr); return false; }
    return true;
}

void Trigger::Enable(bool on)
{
    if (on == enabled_) return;
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 1;   // generic desktop
    rid.usUsage = 2;       // mouse
    rid.dwFlags = on ? RIDEV_INPUTSINK : RIDEV_REMOVE;
    rid.hwndTarget = on ? MessageWindow() : nullptr;
    if (!RegisterRawInputDevices(&rid, 1, sizeof rid)) {
        Log(L"raw mouse input could not be turned %s (error %lu)", on ? L"on" : L"off", GetLastError());
        return;
    }
    enabled_ = on;
    if (!on) { HideProbe(); state_ = State::Idle; paths_.clear(); }
}

bool Trigger::PrimaryDown() const
{
    // GetAsyncKeyState reads the physical buttons; with swapped buttons the primary one is the right.
    int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void Trigger::OnRawInput(HRAWINPUT h)
{
    RAWINPUT ri;
    UINT size = sizeof ri;
    if (GetRawInputData(h, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) || ri.header.dwType != RIM_TYPEMOUSE) return;
    USHORT f = ri.data.mouse.usButtonFlags;
    bool swap = GetSystemMetrics(SM_SWAPBUTTON) != 0;
    USHORT downFlag = swap ? RI_MOUSE_RIGHT_BUTTON_DOWN : RI_MOUSE_LEFT_BUTTON_DOWN;
    USHORT upFlag = swap ? RI_MOUSE_RIGHT_BUTTON_UP : RI_MOUSE_LEFT_BUTTON_UP;
    CountCpu();
    if (f & downFlag) ButtonDown(CursorPos());
    if (state_ == State::Pressed || state_ == State::Watching || state_ == State::Probing || state_ == State::FileDrag) Moved(CursorPos());
    if (f & upFlag) ButtonUp();
}

void Trigger::CountCpu()
{
    // Raw input arrives for every mouse report (up to 1000 a second on gaming mice), so this
    // is where Roda could cost CPU without anyone noticing. Logged once per minute of movement;
    // only the clock is read per event.
    rawEvents_++;
    double now = Now();
    if (cpuSince_ < 0) { cpuSince_ = now; cpuAtStart_ = ProcessCpu100ns(); rawEvents_ = 0; return; }
    if (now - cpuSince_ < 60) return;
    uint64_t cpu = ProcessCpu100ns();
    Log(L"cpu: %.0f ms used in the last %.0f s while the mouse moved (%d mouse reports)",
        (cpu - cpuAtStart_) / 10000.0, now - cpuSince_, rawEvents_);
    cpuSince_ = now;
    cpuAtStart_ = cpu;
    rawEvents_ = 0;
}

void Trigger::ButtonDown(POINT p)
{
    HideProbe();
    paths_.clear();
    down_ = p;
    // Roda's own drags (result cards, Float) never open the wheel.
    state_ = OwnDrag() ? State::Ignored : State::Pressed;
}

void Trigger::ButtonUp()
{
    HideProbe();
    if (state_ == State::FileDrag && wheel_.shown()) wheel_.ButtonReleased();
    state_ = State::Idle;
    paths_.clear();
}

void Trigger::Moved(POINT p)
{
    switch (state_) {
        case State::Pressed:
            if (std::abs(p.x - down_.x) > GetSystemMetrics(SM_CXDRAG) || std::abs(p.y - down_.y) > GetSystemMetrics(SM_CYDRAG)) {
                if (HWND fs = FullScreenAppAt(p)) {
                    state_ = State::Ignored;
                    Log(L"drag: a full screen app is in front (%s, %s); not watching this drag", ProcessNameOf(fs).c_str(), ClassOf(fs).c_str());
                    break;
                }
                // A drag may have started. Keys already held now (Shift+drag to move) are not a trigger press.
                int keys[3] = { settings_.keyConvert, settings_.keyTools, settings_.keyActions };
                for (int i = 0; i < 3; i++) keyWasDown_[i] = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
                spaceWasDown_ = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
                tabWasDown_ = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
                // The window the button went down on: its thread starts the drag, if anyone does.
                srcThread_ = GetWindowThreadProcessId(WindowFromPoint(down_), nullptr);
                lastCapture_ = reinterpret_cast<HWND>(-1);   // so "no capture" is logged too
                captureLogs_ = 0;
                state_ = State::Watching;
                dragSince_ = Now();
                nextTick_ = dragSince_ + kKeyPoll;
                CheckCapture();
            }
            break;
        case State::Watching:
            CheckCapture();
            break;
        case State::Probing:
            if (Now() - dragSince_ > kProbeWindow) {
                GiveUp(L"the drag source never offered anything to the probe");
            } else {
                SetWindowPos(probe_, HWND_TOPMOST, p.x - kProbeSize / 2, p.y - kProbeSize / 2, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
            }
            break;
        case State::FileDrag:
            CheckKeys();   // while the pointer moves the keys are seen at once; Tick covers a still pointer
            break;
        default: break;
    }
}

void Trigger::CheckCapture()
{
    if (Now() - dragSince_ > kProbeWindow) {
        GiveUp(L"the mouse capture never moved to an OLE drag (a selection, a window move, or an app's own dragging)");
        return;
    }
    // The drag source may not have started DoDragDrop yet when the threshold is crossed (it sees
    // the same move a moment after Roda does), so this is asked again on each move and tick.
    GUITHREADINFO gti{ sizeof gti };
    HWND cap = GetGUIThreadInfo(srcThread_, &gti) ? gti.hwndCapture : nullptr;
    if (!cap) {
        // Desktop icons and some sources run the drag on another thread than the window under
        // the pointer, so also ask about the thread in front.
        GUITHREADINFO fg{ sizeof fg };
        if (GetGUIThreadInfo(0, &fg)) cap = fg.hwndCapture;
    }
    wstring cls = ClassOf(cap);
    if (cap != lastCapture_ && captureLogs_ < kMaxCaptureLogs) {
        lastCapture_ = cap;
        captureLogs_++;
        if (cap) Log(L"drag: mouse capture is on %s (%s)%s", cls.c_str(), ProcessNameOf(cap).c_str(), (gti.flags & GUI_INMOVESIZE) ? L", moving or sizing a window" : L"");
        else Log(L"drag: no window has the mouse capture yet");
    }
    if (cls != kOleDragClass) return;
    state_ = State::Probing;
    ShowProbe(CursorPos());
}

void Trigger::GiveUp(const wchar_t* why)
{
    HideProbe();
    state_ = State::Ignored;
    Log(L"drag: not a file drag: %s", why);
}

void Trigger::ShowProbe(POINT p)
{
    SetWindowPos(probe_, HWND_TOPMOST, p.x - kProbeSize / 2, p.y - kProbeSize / 2, kProbeSize, kProbeSize, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Trigger::HideProbe()
{
    if (probe_ && IsWindowVisible(probe_))
        SetWindowPos(probe_, nullptr, -10000, -10000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
}

void Trigger::OnProbeEnter(IDataObject* data)
{
    // Step aside first, so the drag carries on to wherever the user is heading.
    HideProbe();
    if (state_ != State::Probing) return;
    double ms = (Now() - dragSince_) * 1000;
    if (!ReadDroppedPaths(data, paths_)) {
        state_ = State::Ignored;
        Log(L"drag: a drag without files (text or a link), %.0f ms after it started", ms);
        return;
    }
    state_ = State::FileDrag;
    Log(L"drag: %d file(s) being dragged, noticed %.0f ms after the drag started", int(paths_.size()), ms);
    CheckKeys();
}

bool Trigger::NeedsTick() const { return state_ == State::Watching || state_ == State::Probing || state_ == State::FileDrag; }

double Trigger::NextTickIn() const { return std::max(0.0, nextTick_ - Now()); }

void Trigger::Tick()
{
    double now = Now();
    if (now < nextTick_) return;
    nextTick_ = now + kKeyPoll;
    // A button-up we never heard about (it happened over an elevated window, say).
    if (!PrimaryDown()) { ButtonUp(); return; }
    if (state_ == State::Watching) { CheckCapture(); return; }
    if (state_ == State::Probing && now - dragSince_ > kProbeWindow) {
        GiveUp(L"the drag source never offered anything to the probe");
        return;
    }
    if (state_ == State::FileDrag) CheckKeys();
}

void Trigger::CheckKeys()
{
    int keys[3] = { settings_.keyConvert, settings_.keyTools, settings_.keyActions };
    int pressed = -1;
    for (int i = 0; i < 3; i++) {
        bool down = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
        if (down && !keyWasDown_[i]) pressed = i;
        keyWasDown_[i] = down;
    }
    if (pressed >= 0) {
        auto kind = static_cast<WheelKind>(pressed);
        if (!wheel_.open()) wheel_.Show(kind, paths_, Now());
        else wheel_.Switch(kind);
    }
    // Space opens the wheel (Convert first), Space or Tab again moves to the next one. Windows leaves
    // both alone during a drag, so a wheel that doesn't open never turns the drop into a copy, a move
    // or a shortcut, as Ctrl, Shift and Alt do.
    bool space = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0, tab = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    if ((space && !spaceWasDown_) || (tab && !tabWasDown_)) {
        if (!wheel_.open()) wheel_.Show(WheelKind::Convert, paths_, Now());
        else wheel_.Next();
    }
    spaceWasDown_ = space;
    tabWasDown_ = tab;
    if (wheel_.open() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        Log(L"wheel: Esc, cancelled");
        wheel_.Hide();
    }
}

}  // namespace rd
