// Stranded Together textures: PNG through the Windows Imaging Component (no third-party code). The caller's thread must
// have COM initialised (CoInitializeEx; the extractor and the tests use COINIT_MULTITHREADED on their own thread).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "tex_codec.hpp"

namespace st::tex {

enum class PngLayout {
    gray8,   // the image's R channel as 8-bit grey
    rgb24,   // R, G, B (alpha dropped)
    rgba32,  // R, G, B, A
};
// Encodes `img` as a PNG in memory (no filter: fast, about the size of a deflated image).
bool EncodePng(const Image& img, PngLayout layout, std::vector<std::uint8_t>& out, std::string* error = nullptr);
// Decodes any PNG (grey, palette, 16-bit, with or without alpha) to RGBA8.
bool DecodePng(const std::uint8_t* data, std::size_t size, Image& out, std::string* error = nullptr);
bool ReadPngFile(const std::wstring& path, Image& out, std::string* error = nullptr);
// Only the size from the header (no pixel decode). False when it is not a PNG.
bool PngSize(const std::uint8_t* data, std::size_t size, std::uint32_t& w, std::uint32_t& h);

}  // namespace st::tex
