#include "settings_ui.h"

#include "ui.h"

namespace rd {

namespace {

constexpr double kW = 540, kPad = 24, kTop = 64, kRowH = 44, kLabelW = 190;
constexpr double kCtlX = kPad + kLabelW, kCtlW = kW - kPad - kCtlX;
constexpr double kShadow = 26;   // room around the card for its shadow

enum Row { RConvert, RTools, RActions, RHotkey, RSave, RFolder, RJpeg, RWebp, RDpi, RMeta, RTheme, RStartup, RExplorer, RCount };

const wchar_t* kLabels[RCount] = { L"Convert wheel key", L"Tools wheel key", L"Actions wheel key", L"Hotkey for selected files",
                                   L"Save results", L"Folder", L"JPEG quality", L"WebP quality", L"PDF pages as pictures",
                                   L"Photo info (EXIF)", L"Look", L"Start with Windows", L"Right-click in Explorer" };

double H() { return kTop + RCount * kRowH + 22; }
const int kTriggerKeys[3] = { VK_SHIFT, VK_CONTROL, VK_MENU };
const int kDpis[3] = { 150, 300, 600 };

std::vector<const wchar_t*> Choices(int row)
{
    switch (row) {
        case RConvert: case RTools: case RActions: return { L"Shift", L"Ctrl", L"Alt" };
        case RSave: return { L"Next to the file", L"In one folder" };
        case RDpi: return { L"150 DPI", L"300 DPI", L"600 DPI" };
        case RMeta: return { L"Keep", L"Remove" };
        case RTheme: return { L"As Windows", L"Light", L"Dark" };
        case RStartup: case RExplorer: return { L"Off", L"On" };
        default: return {};
    }
}

wstring KeyName(UINT vk)
{
    wchar_t name[64] = L"";
    UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LONG lp = LONG(sc << 16);
    if (vk >= VK_PRIOR && vk <= VK_HELP) lp |= 1 << 24;   // the navigation block is "extended"
    if (!GetKeyNameTextW(lp, name, 64) || !name[0]) swprintf(name, 64, L"Key %u", vk);
    return name;
}

wstring HotkeyText(const Settings& s)
{
    wstring t;
    if (s.hotkeyMods & MOD_CONTROL) t += L"Ctrl + ";
    if (s.hotkeyMods & MOD_ALT) t += L"Alt + ";
    if (s.hotkeyMods & MOD_SHIFT) t += L"Shift + ";
    if (s.hotkeyMods & MOD_WIN) t += L"Win + ";
    return t + KeyName(UINT(s.hotkeyKey));
}

}  // namespace

SettingsWindow::SettingsWindow(Settings& s) : s_(s) {}

SettingsWindow::~SettingsWindow()
{
    if (hwnd_) DestroyWindow(hwnd_);
}

void SettingsWindow::Show()
{
    if (!hwnd_) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = WndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(wc.hInstance, MAKEINTRESOURCEW(1));
        wc.lpszClassName = L"RodaSettings";
        RegisterClassW(&wc);
        // An ordinary window in the taskbar (it can be found again), drawn by Roda itself.
        hwnd_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_APPWINDOW, wc.lpszClassName, L"Roda settings", WS_POPUP | WS_SYSMENU | WS_MINIMIZEBOX,
                                0, 0, 1, 1, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) return;
    }
    POINT at = CursorPos();
    scale_ = ScaleAt(at);
    surf_.SetScale(scale_);
    Mon mon = MonitorOf(at);
    int w = int(std::lround((kW + 2 * kShadow) * scale_)), h = int(std::lround((H() + 2 * kShadow) * scale_));
    int x = mon.work.left + (mon.work.right - mon.work.left - w) / 2, y = std::max<int>(mon.work.top, mon.work.top + (mon.work.bottom - mon.work.top - h) / 2);
    if (!shown_) {
        frame_ = { x, y, x + w, y + h };
        surf_.Resize(w, h);
        if (ReducedMotion()) fade_.Set(1); else { fade_.Set(0); fade_.Ease(1, 0.15, Curve::EaseOut); }
        SetWindowPos(hwnd_, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
    }
    shown_ = true;
    Draw();
    SetForegroundWindow(hwnd_);
    Log(L"settings: opened");
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, LONG_PTR(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams));
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (self && self->hwnd_ == h) return self->Handle(m, w, l);
    return DefWindowProcW(h, m, w, l);
}

SettingsWindow::Hit SettingsWindow::HitTest(double x, double y) const
{
    Hit h;
    x -= kShadow;
    y -= kShadow;
    if (x > kW - 48 && x < kW - 12 && y > 12 && y < 48) { h.row = 100; return h; }   // the close cross
    if (y < kTop || x < kCtlX || x > kCtlX + kCtlW) return h;
    int row = int((y - kTop) / kRowH);
    if (row < 0 || row >= RCount) return h;
    double cy = y - kTop - row * kRowH;
    if (cy < 6 || cy > kRowH - 6) return h;
    h.row = row;
    auto ch = Choices(row);
    if (!ch.empty()) h.choice = int((x - kCtlX) / (kCtlW / ch.size()));
    else if (row == RFolder) h.choice = x > kCtlX + kCtlW - 96 ? 0 : -1;
    else h.choice = 0;
    return h;
}

void SettingsWindow::Slide(int row, double x)
{
    double f = std::clamp((x - kShadow - kCtlX) / (kCtlW - 52), 0.0, 1.0);
    int v = 50 + int(std::lround(f * 50));
    (row == RJpeg ? s_.jpegQuality : s_.webpQuality) = v;
}

void SettingsWindow::PickFolder()
{
    Com<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(dlg.Put())))) return;
    DWORD o = 0;
    dlg->GetOptions(&o);
    dlg->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Save Roda's results in");
    BeginModal();
    HRESULT hr = dlg->Show(hwnd_);
    EndModal();
    Com<IShellItem> item;
    PWSTR p = nullptr;
    if (SUCCEEDED(hr) && SUCCEEDED(dlg->GetResult(item.Put())) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &p)) && p) {
        s_.outputFolder = p;
        Log(L"settings: results go to %s", p);
    }
    CoTaskMemFree(p);
}

void SettingsWindow::Click(const Hit& h, double x)
{
    if (h.row == 100) { ShowWindow(hwnd_, SW_HIDE); shown_ = false; surf_.Release(); return; }
    if (h.row < 0) return;
    int c = h.choice;
    int* keys[3] = { &s_.keyConvert, &s_.keyTools, &s_.keyActions };
    switch (h.row) {
        case RConvert: case RTools: case RActions: {
            // Each wheel has its own key: picking one another wheel uses swaps them.
            int row = h.row - RConvert, want = kTriggerKeys[std::clamp(c, 0, 2)];
            for (int i = 0; i < 3; i++) if (i != row && *keys[i] == want) *keys[i] = *keys[row];
            *keys[row] = want;
            if (onKeysChanged) onKeysChanged();
            break;
        }
        case RHotkey: capturing_ = true; break;
        case RSave:
            if (c == 0) s_.outputFolder.clear();
            else if (s_.outputFolder.empty()) PickFolder();
            break;
        case RFolder: if (c == 0) PickFolder(); break;
        case RJpeg: case RWebp: dragging_ = h.row; Slide(h.row, x); SetCapture(hwnd_); break;
        case RDpi: s_.pdfDpi = kDpis[std::clamp(c, 0, 2)]; break;
        case RMeta: s_.keepMetadata = c == 0; break;
        case RTheme: s_.theme = std::clamp(c, 0, 2); SetThemeOverride(s_.theme); if (onThemeChanged) onThemeChanged(); break;
        case RStartup: if (setStartup) setStartup(c == 1); break;
        case RExplorer: if (setExplorerMenu) setExplorerMenu(c == 1); break;
    }
    s_.Save();
    dirty_ = true;
    RequestFrame();
}

void SettingsWindow::Capture(UINT vk)
{
    if (vk == VK_ESCAPE) { capturing_ = false; return; }
    if (vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN) return;   // wait for the real key
    int mods = 0;
    if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CONTROL;
    if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    if ((GetKeyState(VK_LWIN) | GetKeyState(VK_RWIN)) & 0x8000) mods |= MOD_WIN;
    // A hotkey needs Ctrl, Alt or Win: a plain letter would be taken from every app.
    if (!(mods & (MOD_CONTROL | MOD_ALT | MOD_WIN))) { MessageBeep(MB_OK); return; }
    s_.hotkeyMods = mods;
    s_.hotkeyKey = int(vk);
    capturing_ = false;
    s_.Save();
    if (onKeysChanged) onKeysChanged();
    Log(L"settings: hotkey is now %s", HotkeyText(s_).c_str());
}

LRESULT SettingsWindow::Handle(UINT m, WPARAM w, LPARAM l)
{
    auto local = [&](LPARAM lp) { return std::pair<double, double>{ GET_X_LPARAM(lp) / scale_, GET_Y_LPARAM(lp) / scale_ }; };
    switch (m) {
        case WM_NCHITTEST: {
            // The empty parts of the card move the window.
            POINT p{ GET_X_LPARAM(l), GET_Y_LPARAM(l) };
            ScreenToClient(hwnd_, &p);
            Hit h = HitTest(p.x / scale_, p.y / scale_);
            double y = p.y / scale_ - kShadow;
            return h.row < 0 && y < kTop ? HTCAPTION : HTCLIENT;
        }
        case WM_LBUTTONDOWN: { auto [x, y] = local(l); Click(HitTest(x, y), x); return 0; }
        case WM_MOUSEMOVE: {
            auto [x, y] = local(l);
            if (dragging_ >= 0) { Slide(dragging_, x); dirty_ = true; RequestFrame(); return 0; }
            Hit h = HitTest(x, y);
            if (h.row != hover_.row || h.choice != hover_.choice) { hover_ = h; dirty_ = true; RequestFrame(); }
            return 0;
        }
        case WM_LBUTTONUP:
            if (dragging_ >= 0) { dragging_ = -1; ReleaseCapture(); s_.Save(); }
            return 0;
        case WM_SETCURSOR:
            SetCursor(LoadCursorW(nullptr, hover_.row >= 0 ? IDC_HAND : IDC_ARROW));
            return TRUE;
        case WM_KEYDOWN: case WM_SYSKEYDOWN:
            if (capturing_) { Capture(UINT(w)); dirty_ = true; RequestFrame(); return 0; }
            if (w == VK_ESCAPE) { ShowWindow(hwnd_, SW_HIDE); shown_ = false; surf_.Release(); }
            return 0;
        case WM_SYSCHAR: if (capturing_) return 0; break;   // no beep for Alt+key while picking the hotkey
        case WM_ACTIVATE: if (LOWORD(w) == WA_INACTIVE && capturing_) { capturing_ = false; dirty_ = true; RequestFrame(); } return 0;
        case WM_CLOSE: ShowWindow(hwnd_, SW_HIDE); shown_ = false; surf_.Release(); return 0;
        case WM_MOVE: GetWindowRect(hwnd_, &frame_); return 0;
        case WM_DPICHANGED: return 0;
    }
    return DefWindowProcW(hwnd_, m, w, l);
}

void SettingsWindow::Draw()
{
    dirty_ = false;
    if (!shown_) return;
    if (!surf_.ready()) surf_.Resize(frame_.right - frame_.left, frame_.bottom - frame_.top);
    ID2D1DCRenderTarget* rt = surf_.Begin();
    if (!rt) return;
    const Theme& th = CurrentTheme();
    float L = float(kShadow), T = float(kShadow), W = float(kW), Ht = float(H());
    surf_.DrawShadow(RectF(L, T, W, Ht), 16, 22, 0, 8, th.shadow);
    rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(L, T, W, Ht), 16, 16), surf_.Solid(th.glass | 0xFF000000));
    rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(L + 0.5, T + 0.5, W - 1, Ht - 1), 15.5f, 15.5f), surf_.Solid(th.edge), 1);
    surf_.DrawText(L"Roda settings", 16, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(L + kPad, T + 20, 300, 24), th.text);
    // Close.
    D2D1_POINT_2F cc = D2D1::Point2F(L + W - 30, T + 30);
    if (hover_.row == 100) rt->FillEllipse(D2D1::Ellipse(cc, 13, 13), surf_.Solid(th.button));
    rt->DrawLine(D2D1::Point2F(cc.x - 5, cc.y - 5), D2D1::Point2F(cc.x + 5, cc.y + 5), surf_.Solid(th.text), 1.4f);
    rt->DrawLine(D2D1::Point2F(cc.x - 5, cc.y + 5), D2D1::Point2F(cc.x + 5, cc.y - 5), surf_.Solid(th.text), 1.4f);

    int* keys[3] = { &s_.keyConvert, &s_.keyTools, &s_.keyActions };
    bool startup = startupOn && startupOn();
    for (int row = 0; row < RCount; row++) {
        float y = T + float(kTop + row * kRowH);
        if (row > 0) rt->DrawLine(D2D1::Point2F(L + kPad, y), D2D1::Point2F(L + W - kPad, y), surf_.Solid(th.sep), 1);
        bool disabled = row == RFolder && s_.outputFolder.empty();
        surf_.DrawText(kLabels[row], 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(L + kPad, y + 14, kLabelW, 20), disabled ? th.dim : th.text);
        float cx = L + float(kCtlX), cw = float(kCtlW), cy = y + 8, ch = float(kRowH - 16);
        auto choices = Choices(row);
        if (!choices.empty()) {
            int selected = 0;
            switch (row) {
                case RConvert: case RTools: case RActions:
                    selected = -1;   // another key, set in settings.json: none of the three
                    for (int i = 0; i < 3; i++) if (*keys[row - RConvert] == kTriggerKeys[i]) selected = i;
                    break;
                case RSave: selected = s_.outputFolder.empty() ? 0 : 1; break;
                case RDpi: for (int i = 0; i < 3; i++) if (s_.pdfDpi == kDpis[i]) selected = i; break;
                case RMeta: selected = s_.keepMetadata ? 0 : 1; break;
                case RTheme: selected = s_.theme; break;
                case RStartup: selected = startup ? 1 : 0; break;
                case RExplorer: selected = explorerMenuOn && explorerMenuOn() ? 1 : 0; break;
            }
            float segW = cw / float(choices.size());
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(cx, cy, cw, ch), 8, 8), surf_.Solid(th.button));
            for (size_t i = 0; i < choices.size(); i++) {
                float sx = cx + segW * float(i);
                bool on = int(i) == selected, hot = hover_.row == row && hover_.choice == int(i);
                if (on) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(sx + 2, cy + 2, segW - 4, ch - 4), 6, 6), surf_.Solid(th.accent));
                else if (hot) rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(sx + 2, cy + 2, segW - 4, ch - 4), 6, 6), surf_.Solid(WithAlpha(th.accent, 0.15)));
                float tw = surf_.MeasureText(choices[i], 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
                surf_.DrawText(choices[i], 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(sx + (segW - tw) / 2, cy + ch / 2 - 8.5f, tw + 4, 18), on ? th.accentText : th.text);
            }
        } else if (row == RHotkey) {
            bool hot = hover_.row == row;
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(cx, cy, cw, ch), 8, 8), surf_.Solid(th.button));
            if (capturing_ || hot) rt->DrawRoundedRectangle(D2D1::RoundedRect(RectF(cx + 0.75, cy + 0.75, cw - 1.5, ch - 1.5), 7.5f, 7.5f), surf_.Solid(th.accent), 1.5f);
            wstring t = capturing_ ? L"Press the keys now (Esc: keep the old ones)" : HotkeyText(s_);
            surf_.DrawText(t.c_str(), 12.5f, DWRITE_FONT_WEIGHT_NORMAL, RectF(cx + 12, cy + ch / 2 - 9, cw - 20, 20), capturing_ ? th.accent : th.text);
        } else if (row == RFolder) {
            wstring path = s_.outputFolder.empty() ? L"(next to each file)" : Ellipsize(surf_, s_.outputFolder, 12, DWRITE_FONT_WEIGHT_NORMAL, cw - 108);
            surf_.DrawText(path.c_str(), 12, DWRITE_FONT_WEIGHT_NORMAL, RectF(cx, cy + ch / 2 - 8.5f, cw - 104, 18), th.dim);
            bool hot = hover_.row == row && hover_.choice == 0;
            rt->FillRoundedRectangle(D2D1::RoundedRect(RectF(cx + cw - 92, cy, 92, ch), 8, 8), surf_.Solid(hot ? WithAlpha(th.accent, 0.18) : th.button));
            float tw = surf_.MeasureText(L"Choose…", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
            surf_.DrawText(L"Choose…", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(cx + cw - 46 - tw / 2, cy + ch / 2 - 8.5f, tw + 4, 18), th.text);
        } else if (row == RJpeg || row == RWebp) {
            int v = row == RJpeg ? s_.jpegQuality : s_.webpQuality;
            float x0 = cx, x1 = cx + cw - 52, my = cy + ch / 2;
            float kx = x0 + (x1 - x0) * float(std::clamp(v - 50, 0, 50)) / 50.0f;
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, my - 2, x1, my + 2), 2, 2), surf_.Solid(th.sep));
            rt->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x0, my - 2, kx, my + 2), 2, 2), surf_.Solid(th.accent));
            rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, my), 8, 8), surf_.Solid(th.accent));
            wstring num = std::to_wstring(v);
            surf_.DrawText(num.c_str(), 12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, RectF(x1 + 16, my - 9, 40, 20), th.text);
        }
    }
    surf_.End();
    surf_.Present(hwnd_, frame_.left, frame_.top, BYTE(std::clamp(fade_.value, 0.0, 1.0) * 255));
    if (fade_.Active()) dirty_ = true;
}

}  // namespace rd
