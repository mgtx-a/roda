// The Sign window: a PDF's pages, one at a time, with signatures (drawn with the mouse) and pictures
// placed on them by dragging. Save hands the placements to a worker (pdf.sign / pdf.picture); the
// pages themselves are drawn by workers too (the UI process never loads PDFium).
#pragma once
#include "anim.h"
#include "gfx.h"
#include "json.h"

namespace rd {

class PlaceWindow {
public:
    PlaceWindow();
    ~PlaceWindow();

    /// Opens for `pdf`; `draw` starts with the signature pad, otherwise with the picture chooser.
    void Show(const wstring& pdf, bool draw);
    void Hide();
    bool shown() const { return shown_; }

    /// A page's picture arrived (a PNG in the temp folder), with the page count.
    void SetPage(int page, const wstring& png, int pages);
    void PageFailed(const wstring& why);

    std::function<void(int page)> onNeedPage;      // draw this page (0-based) for me
    std::function<void(const Json& opts)> onSave;  // { stamps: [ {page, x, y, w, h, image} ] }

    bool dirty() const { return dirty_; }
    void Draw();

private:
    struct Item {
        int page = 0;
        double x = 0, y = 0, w = 0, h = 0;   // fractions of the page as shown
        wstring image;                       // PNG/JPG on disk, read by the worker
        Mips pic;
    };
    enum class Part { None, Close, Prev, Next, Draw, Picture, Cancel, Save, Item, Resize, Delete, Pad, PadClear, PadCancel, PadUse };

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    Part HitTest(double x, double y, int* item) const;
    void Click(Part p, int item, double x, double y);
    void Go(int page);
    void AddItem(const wstring& image, Mips pic, bool signature);
    void PickPicture();
    bool SaveSignature(wstring& png);
    void Save();
    D2D1_RECT_F PageRect() const;     // where the page is drawn (DIPs, window coordinates)
    D2D1_RECT_F PadRect() const;
    D2D1_RECT_F ItemRect(const Item& it) const;
    void Changed() { dirty_ = true; RequestFrame(); }

    HWND hwnd_ = nullptr;
    Surface surf_;
    bool shown_ = false, dirty_ = false;
    double scale_ = 1;
    RECT frame_{};
    Anim fade_;

    wstring pdf_;
    bool signMode_ = true;           // Sign (pdf.sign) or Add picture (pdf.picture)
    int page_ = 0, pages_ = 0, waitingFor_ = -1;
    Mips pageMips_;
    int pageW_ = 0, pageH_ = 0;
    wstring note_;                   // "Loading…", or why a page could not be shown
    std::vector<Item> items_;
    int selected_ = -1;
    wstring pendingPath_;            // a picture chosen before the page arrived: placed when it does
    Mips pendingPic_;

    bool drawing_ = false;           // the signature pad is open
    std::vector<std::vector<D2D1_POINT_2F>> strokes_;   // pad coordinates (DIPs from its top left)
    bool stroking_ = false;

    Part hover_ = Part::None, drag_ = Part::None;
    int dragItem_ = -1;
    double dragX_ = 0, dragY_ = 0;
    Item dragStart_;
};

}  // namespace rd
