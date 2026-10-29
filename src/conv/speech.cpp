// Text read aloud into a sound file, with the voice Windows already has (SAPI, offline): used for
// PDF to MP3. The voice speaks into a WAV file in the worker's temp folder; FFmpeg makes the MP3.
#include "conv.h"

#include <initguid.h>   // defines CLSID_SpVoice and CLSID_SpStream from sapi.h here
#include <sapi.h>

namespace rd {

namespace {

// Declared by sapi.h, defined in a library MinGW doesn't ship.
const GUID kWaveFormatEx = { 0xC31ADBAE, 0x527F, 0x4FF5, { 0xA2, 0x30, 0xF6, 0x2B, 0xB6, 0x1F, 0xF7, 0x0C } };

/// The text in pieces of about `size` characters, cut at paragraph or sentence ends, so progress can move.
std::vector<wstring> Pieces(const wstring& text, size_t size)
{
    std::vector<wstring> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = std::min(text.size(), start + size);
        if (end < text.size()) {
            size_t cut = text.rfind(L'\n', end);
            if (cut == wstring::npos || cut <= start) cut = text.rfind(L". ", end);
            if (cut != wstring::npos && cut > start) end = cut + 1;
        }
        out.push_back(text.substr(start, end - start));
        start = end;
    }
    return out;
}

}  // namespace

bool SpeakToWav(const wstring& text, const wstring& wavPath, const Progress& p, wstring& err)
{
    Com<ISpVoice> voice;
    if (FAILED(CoCreateInstance(CLSID_SpVoice, nullptr, CLSCTX_ALL, IID_ISpVoice, reinterpret_cast<void**>(voice.Put())))) {
        err = L"Windows has no voice to read with on this PC.";
        return false;
    }
    Com<ISpStream> stream;
    if (FAILED(CoCreateInstance(CLSID_SpStream, nullptr, CLSCTX_ALL, IID_ISpStream, reinterpret_cast<void**>(stream.Put())))) {
        err = L"Windows' speech could not start.";
        return false;
    }
    // 22 kHz mono: plenty for a voice, and a small file.
    WAVEFORMATEX wf{ WAVE_FORMAT_PCM, 1, 22050, 22050 * 2, 2, 16, 0 };
    if (FAILED(stream->BindToFile(wavPath.c_str(), SPFM_CREATE_ALWAYS, &kWaveFormatEx, &wf, 0)) || FAILED(voice->SetOutput(stream.Get(), TRUE))) {
        err = L"Could not write the sound file.";
        return false;
    }
    auto pieces = Pieces(text, 2000);
    size_t done = 0;
    for (auto& piece : pieces) {
        // Not XML: a "<" in the text is read as it is.
        if (FAILED(voice->Speak(piece.c_str(), SPF_IS_NOT_XML, nullptr))) { stream->Close(); err = L"Windows' voice stopped partway."; return false; }
        done += piece.size();
        p.Set(0.9 * double(done) / double(text.size()));
    }
    stream->Close();
    return true;
}

}  // namespace rd
