// The PDF editor window (Sign, Add picture, Fill form): a PDF's pages one at a time, with form fields
// to fill, text to type anywhere, and signatures (drawn with the mouse) or pictures placed by
// dragging. Save hands everything to a worker (pdf.sign / pdf.picture / pdf.fill); pages and fields
// come from workers too (the UI process never loads PDFium).
#pragma once
#include "anim.h"
#include "gfx.h"
#include "json.h"

namespace rd {

class PlaceWindow {
public:
    enum class Mode { Sign, Picture, Fill };

    PlaceWindow();
    ~PlaceWindow();

    /// Opens for `pdf`: Sign starts with the signature pad, Picture with the chooser, Fill with the fields.
    void Show(const wstring& pdf, Mode mode);
    void Hide();
    bool shown() const { return shown_; }

    /// A page's picture arrived (a PNG in the temp folder), with the page count and its height in points.
    void SetPage(int page, const wstring& png, int pages, double heightPt);
    void PageFailed(const wstring& why);
    /// The form fields of every page arrived ("fields" from prep.fields).
    void SetFields(const Json& fields);

    std::function<void(int page)> onNeedPage;      // draw this page (0-based) for me
    std::function<void()> onNeedFields;            // read the form fields for me
    std::function<void(const Json& opts)> onSave;  // { stamps: [...], texts: [...], fields: [...] }

    bool dirty() const { return dirty_; }
    void Draw();

private:
    struct Item {                    // something added: a picture, or typed text
        bool text = false;
        int page = 0;
        double x = 0, y = 0, w = 0, h = 0;   // fractions of the page as shown (text: w, h follow the words)
        wstring image;               // picture: PNG/JPG on disk, read by the worker
        Mips pic;
        wstring words;               // text
        double size = 12;            // text: points
    };
    struct Field {                   // a form field of the PDF
        int page = 0, index = 0, type = 0;   // type: FPDF_FORMFIELD_* (2 check, 3 radio, 4 combo, 5 list, 6 text)
        double x = 0, y = 0, w = 0, h = 0;
        wstring name, label, exportValue, value;
        bool checked = false, multiline = false, changed = false;
        std::vector<wstring> options;
        int choice = -1;             // the chosen option (lists)
    };
    enum class Part { None, Close, Prev, Next, Text, Sign, Picture, Cancel, Save, Item, Resize, Delete, Field, Option,
                      Pad, PadClear, PadCancel, PadUse };

    static LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l);
    LRESULT Handle(UINT m, WPARAM w, LPARAM l);
    Part HitTest(double x, double y, int* which) const;
    void Click(Part p, int which, double x, double y);
    void Go(int page);
    void AddItem(Item it);
    void AddPicture(const wstring& image, Mips pic, bool signature);
    void AddText();
    void PickPicture();
    bool SaveSignature(wstring& png);
    void Save();
    void StopEditing();
    void Type(const wstring& s);
    void Backspace();
    void ClickField(int f);
    void NextField(int from);
    D2D1_RECT_F PageRect() const;     // where the page is drawn (DIPs, window coordinates)
    D2D1_RECT_F PadRect() const;
    D2D1_RECT_F ItemRect(const Item& it) const;
    D2D1_RECT_F FieldRect(const Field& f) const;
    D2D1_RECT_F OptionsRect() const;  // the open list of a combo/list field
    double PtToDip() const;           // one point of the page, in DIPs on screen
    wstring& EditedText();
    bool EditingMultiline() const;
    void Changed() { dirty_ = true; RequestFrame(); }

    HWND hwnd_ = nullptr;
    Surface surf_;
    bool shown_ = false, dirty_ = false;
    double scale_ = 1;
    RECT frame_{};
    Anim fade_;

    wstring pdf_;
    Mode mode_ = Mode::Sign;
    int page_ = 0, pages_ = 0, waitingFor_ = -1;
    Mips pageMips_;
    int pageW_ = 0, pageH_ = 0;
    double pagePt_ = 0;              // the page's height in points
    wstring note_;                   // "Loading…", a hint, or why a page could not be shown
    std::vector<Item> items_;
    std::vector<Field> fields_;
    bool fieldsKnown_ = false;
    int selected_ = -1;              // an item
    int editItem_ = -1, editField_ = -1;   // typing into a text item or a text field
    int openList_ = -1;              // a combo/list field showing its options
    wstring pendingPath_;            // a picture chosen before the page arrived: placed when it does
    Mips pendingPic_;
    double caretBlink_ = 0;

    bool drawing_ = false;           // the signature pad is open
    std::vector<std::vector<D2D1_POINT_2F>> strokes_;   // pad coordinates (DIPs from its top left)
    bool stroking_ = false;

    Part hover_ = Part::None, drag_ = Part::None;
    int hoverWhich_ = -1, dragItem_ = -1;
    double dragX_ = 0, dragY_ = 0;
    bool dragMoved_ = false;
    Item dragStart_;
};

}  // namespace rd
