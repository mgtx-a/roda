#include "input.h"

#include "dragdrop.h"

namespace rd {

namespace {

constexpr double kProbeWindow = 0.4;    // how long the probe waits for a drag source to show itself
constexpr double kKeyPoll = 0.016;      // the keys are read this often during a file drag, never otherwise
constexpr int kProbeSize = 48;          // pixels: big enough that a quick flick stays over it

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
    if (f & downFlag) ButtonDown(CursorPos());
    if (state_ == State::Pressed || state_ == State::Probing || state_ == State::FileDrag) Moved(CursorPos());
    if (f & upFlag) ButtonUp();
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
                // A drag may have started. Keys already held now (Shift+drag to move) are not a trigger press.
                int keys[3] = { settings_.keyConvert, settings_.keyTools, settings_.keyActions };
                for (int i = 0; i < 3; i++) keyWasDown_[i] = (GetAsyncKeyState(keys[i]) & 0x8000) != 0;
                state_ = State::Probing;
                probeSince = Now();
                nextTick_ = probeSince + kKeyPoll;
                ShowProbe(p);
            }
            break;
        case State::Probing:
            if (Now() - probeSince > kProbeWindow) {
                HideProbe();
                state_ = State::Ignored;
                Log(L"drag: no file drag (a selection, a window move, or an app's own dragging)");
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
    double ms = (Now() - probeSince) * 1000;
    if (!ReadDroppedPaths(data, paths_)) {
        state_ = State::Ignored;
        Log(L"drag: a drag without files (text or a link), %.0f ms after it started", ms);
        return;
    }
    state_ = State::FileDrag;
    Log(L"drag: %d file(s) being dragged, noticed %.0f ms after the drag started", int(paths_.size()), ms);
    CheckKeys();
}

bool Trigger::NeedsTick() const { return state_ == State::Probing || state_ == State::FileDrag; }

double Trigger::NextTickIn() const { return std::max(0.0, nextTick_ - Now()); }

void Trigger::Tick()
{
    double now = Now();
    if (now < nextTick_) return;
    nextTick_ = now + kKeyPoll;
    // A button-up we never heard about (it happened over an elevated window, say).
    if (!PrimaryDown()) { ButtonUp(); return; }
    if (state_ == State::Probing && now - probeSince > kProbeWindow) {
        HideProbe();
        state_ = State::Ignored;
        Log(L"drag: no file drag (a selection, a window move, or an app's own dragging)");
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
    if (wheel_.open() && (GetAsyncKeyState(VK_ESCAPE) & 0x8000)) {
        Log(L"wheel: Esc, cancelled");
        wheel_.Hide();
    }
}

}  // namespace rd
