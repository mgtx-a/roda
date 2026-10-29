// The small options popup some tools need before they run: Compress (a quality slider), Size
// (width and height), Trim (start and end), Remove pages (which pages), Crop (a rectangle on a preview)
// and Merge (the files' order). Mouse first; keys work once it has focus:
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
    /// Size: one-click sizes above the fields; each applies with "op", its own tool. Without fields, only these.
    struct Preset { wstring label, op; };
    void SetPresets(std::vector<Preset> presets, bool fields);
    /// Before Show: the main button's words ("Resize 3 pictures") and where results go. Cleared on Hide.
    void SetSummary(const wstring& apply, const wstring& where);
    /// Merge: where the result goes when file `first` (an index into the names) comes first. Cleared on Hide.
    std::function<wstring(size_t first)> whereFor;
    /// Merge: the files in a list to put in order; applies with "order", the new order as indexes ("2,0,1").
    void ShowOrder(const wstring& title, const std::vector<wstring>& names, POINT at);
    void Hide();
    bool shown() const { return shown_; }

    /// Called with the chosen options (q / w, h / crop), or not at all when cancelled.
    std::function<void(const Json& opts)> onApply;

    bool dirty() const { return dirty_; }
    void Draw();

private:
    enum class Part { None, Slider, Width, Height, Apply, Cancel, CropMove, CropN, CropS, CropE, CropW, CropNE, CropNW, CropSE, CropSW, Aspect, Row, Preset };
    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    Part HitTest(double x, double y, int* aspect) const;
    void Apply();
    void ApplyPreset(int i);
    void Drag(double x, double y);
    void SetAspect(int i);
    void MoveRow(int to);
    D2D1_RECT_F PictureRect() const;
    double CancelX() const;               // DIPs from the card's left
    double FieldsTop() const { return presets_.empty() ? 0 : 44; }
    double RowH() const { return std::clamp(360.0 / std::max<size_t>(1, names_.size()), 22.0, 34.0); }

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
    std::vector<wstring> names_;             // Merge: the files, as given
    std::vector<size_t> order_;              // Merge: indexes into names_, top to bottom
    int selected_ = 0;                       // Merge: the row picked (by click or arrows)
    std::vector<Preset> presets_;
    std::vector<std::pair<double, double>> presetX_;   // left and width of each, DIPs from the card's left
    bool sizeFields_ = true;
    wstring apply_, where_;
    double applyW_ = 92, whereH_ = 0;
    int hoverPreset_ = -1;
    int presetSel_ = -1;                     // the size picked with Tab or the arrows; -1: the fields have the keys
    Part drag_ = Part::None, hover_ = Part::None;
    double dragX_ = 0, dragY_ = 0, startX_ = 0, startY_ = 0, startW_ = 0, startH_ = 0;
};

}  // namespace rd
