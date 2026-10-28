// A small PDF writer: one image per page, page sized to the picture. JPEGs go in as they are
// (DCTDecode, no quality loss); everything else is compressed losslessly (Flate), with transparency
// as a soft mask.
#pragma once
#include "conv.h"

namespace rd {

class PdfWriter {
public:
    /// `jpeg`: the source file's bytes when it is a JPEG, so it can be embedded untouched.
    /// `jpegQuality`: used when a lossy source has to be re-encoded.
    void AddImage(const Image& img, const std::vector<uint8_t>* jpeg, int jpegQuality);
    bool Finish(std::vector<uint8_t>& out, wstring& err);

private:
    struct Page {
        double wPt = 0, hPt = 0;
        int pxW = 0, pxH = 0;
        std::string filter, colour;     // "DCTDecode"/"FlateDecode", "DeviceRGB"/"DeviceGray"
        std::vector<uint8_t> data, mask;  // mask: Flate-compressed alpha, or empty
    };
    std::vector<Page> pages_;
    wstring error_;
};

}  // namespace rd
