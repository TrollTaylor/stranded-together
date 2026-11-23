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
// Decodes any PNG (grey, palette, 16-bit, with or without alpha) to RGBA8. Another format (a JPEG named .png): false.
bool DecodePng(const std::uint8_t* data, std::size_t size, Image& out, std::string* error = nullptr);
bool ReadPngFile(const std::wstring& path, Image& out, std::string* error = nullptr);
// Only the size from the header (no pixel decode). False when it is not a PNG.
bool PngSize(const std::uint8_t* data, std::size_t size, std::uint32_t& w, std::uint32_t& h);

// ---- Any picture, by its content (never by its file name) ----------------------------------------------------
// The pictures the mod and "Check my edits" read: an image tool may save a JPEG (or another format) under the .png name
// the folders use. Only these, recognised by their first bytes; each is opened with the container's own WIC decoder
// (Microsoft's preferred), never by probing every codec installed. WebP needs Windows' WebP codec (built in on current
// Windows 10 / 11; refused with a sentence where it is missing). GIF: its first frame.
enum class PictureFormat { unknown, png, jpeg, bmp, gif, tiff, webp };
const char* PictureFormatName(PictureFormat f);   // "PNG", "JPEG", "BMP", "GIF", "TIFF", "WebP", "unknown"
PictureFormat SniffPicture(const std::uint8_t* data, std::size_t size);
// Width and height from the header, no pixel decoded (PNG: its IHDR; the others: the WIC frame's GetSize, which needs COM
// on this thread). False + a sentence when it is none of the formats above or its header does not read.
bool PictureSize(const std::uint8_t* data, std::size_t size, std::uint32_t& w, std::uint32_t& h, PictureFormat* format = nullptr,
                 std::string* error = nullptr);
// Decodes to RGBA8 (grey, palette, 16-bit, CMYK... converted; no alpha = 255). A side over max_w / max_h (0: 16384) is
// refused from the header, before any pixel is decoded.
bool DecodePicture(const std::uint8_t* data, std::size_t size, Image& out, std::uint32_t max_w = 0, std::uint32_t max_h = 0,
                   PictureFormat* format = nullptr, std::string* error = nullptr);
bool ReadPictureFile(const std::wstring& path, Image& out, PictureFormat* format = nullptr, std::string* error = nullptr);
// Tests and tools: `img` as RGB in another container (png, jpeg at quality 90, bmp, tiff). GIF / WebP: false.
bool EncodePicture(const Image& img, PictureFormat format, std::vector<std::uint8_t>& out, std::string* error = nullptr);

}  // namespace st::tex
