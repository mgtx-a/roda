// The small options popup some tools need before they run: Compress (a quality slider), Size
// (width and height) and Crop (a rectangle on a preview). Mouse first; keys work once it has focus:
// arrows, digits, Tab, Enter applies, Esc cancels.
#pragma once
#include "anim.h"
#include "gfx.h"
#include "registry.h"

namespace rd {

class OptionsPopup {
public:
    OptionsPopup();
    ~OptionsPopup();
    bool Create(HINSTANCE inst);

    /// Opens centred on `at`. For Crop, `preview` is a PNG of the picture and origW/H its real size.
    void Show(Popup kind, const wstring& title, POINT at, int quality, const wstring& preview = L"", int origW = 0, int origH = 0);
    void Hide();
    bool shown() const { return shown_; }

    /// Called with the chosen options (q / w, h / crop), or not at all when cancelled.
    std::function<void(const Json& opts)> onApply;

    bool dirty() const { return dirty_; }
    void Draw();

private:
    enum class Part { None, Slider, Width, Height, Apply, Cancel, CropMove, CropN, CropS, CropE, CropW, CropNE, CropNW, CropSE, CropSW, Aspect };
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    Part HitTest(double x, double y, int* aspect) const;
    void Apply();
    void Drag(double x, double y);
    void SetAspect(int i);
    D2D1_RECT_F PictureRect() const;

    HWND hwnd_ = nullptr;
    Surface surf_;
    Popup kind_ = Popup::None;
    wstring title_;
    bool shown_ = false, dirty_ = false;
    double scale_ = 1, w_ = 0, h_ = 0;      // DIPs
    RECT frame_{};
    Anim fade_;

    int quality_ = 70;
    wstring width_, height_;
    int field_ = 0;                          // 0 width, 1 height
    Mips pic_;
    int origW_ = 0, origH_ = 0;
    double cx_ = 0, cy_ = 0, cw_ = 1, ch_ = 1;   // crop, 0..1 of the picture
    int aspect_ = 0;                         // 0 free, 1 1:1, 2 4:3, 3 16:9
    Part drag_ = Part::None, hover_ = Part::None;
    double dragX_ = 0, dragY_ = 0, startX_ = 0, startY_ = 0, startW_ = 0, startH_ = 0;
};

}  // namespace rd
