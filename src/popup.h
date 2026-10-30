// The small options popup some tools need before they run: Compress (a quality slider), Size
// (width and height), Trim (start and end), Remove pages (the pages, to click away), Crop (a rectangle on
// a preview) and Merge (the files' order). Mouse first; keys work once it has focus:
// arrows, digits, Tab, Enter applies, Esc cancels.
#pragma once
#include "anim.h"
#include "gfx.h"
#include "registry.h"

#include <map>
#include <set>

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
    /// Remove pages: the PDF's pages side by side to click away; applies with "pages" ("2, 5-7").
    /// Pictures of the pages are asked for as they scroll into view: onNeedPage(token, page), answered
    /// by SetPage (token says which showing it was for; a late answer to an old one is dropped).
    void ShowPages(const wstring& title, POINT at);
    std::function<void(int token, int page)> onNeedPage;
    void SetPage(int token, int page, const wstring& png, int pages);
    int token() const { return token_; }
    void Hide();
    bool shown() const { return shown_; }

    /// Called with the chosen options (q / w, h / crop), or not at all when cancelled.
    std::function<void(const Json& opts)> onApply;

    bool dirty() const { return dirty_; }
    void Draw();

private:
    enum class Part { None, Slider, Width, Height, Apply, Cancel, CropMove, CropN, CropS, CropE, CropW, CropNE, CropNW, CropSE, CropSW, Aspect, Row, Preset,
                      Page, Strip, ScrollBar };
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
    // Remove pages: the strip of pages, DIPs.
    double PageW() const;                    // one page's slot (the first page's shape)
    double MaxScroll() const;
    void ScrollBy(double dx);                // glides there (wheel, keys); blends with a glide already going
    void ScrollTo(double x);
    void ShowPage(int page);                 // scrolled so the page is in view
    void AskVisible();                       // pictures for the pages in view and just past
    void TogglePage(int page);

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
    int pages_ = 0;                          // Remove pages: how many (0 until the first picture says)
    std::map<int, Mips> thumbs_;             // pictures of the pages that have arrived
    std::set<int> asked_, removed_;          // 0-based
    double scroll_ = 0, pageAspect_ = 0.707; // width over height of the first page
    int focusPage_ = 0, hoverPage_ = -1, pressPage_ = -1;
    bool moved_ = false;                     // the press became a drag (a scroll), not a click
    bool pageKeys_ = false;                  // the arrows are in use: show which page has them
    Anim glide_;                             // the strip's scroll on its way somewhere (wheel, keys, a fling)
    double flingV_ = 0, lastX_ = 0, lastT_ = 0;   // a drag's speed (DIPs a second), for the slide after letting go
    int token_ = 0;
    Part drag_ = Part::None, hover_ = Part::None;
    double dragX_ = 0, dragY_ = 0, startX_ = 0, startY_ = 0, startW_ = 0, startH_ = 0;
};

}  // namespace rd
