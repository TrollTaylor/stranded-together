// Stranded Together textures: PNG (and any picture the mod reads) through WIC (png_wic.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

#include <cstdio>
#include <cstring>

#include "png_wic.hpp"

namespace st::tex {
namespace {

template <typename T>
struct Com {
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

bool Fail(std::string* error, const char* what, HRESULT hr) {
    if (error) {
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        *error = buf;
    }
    return false;
}

bool Say(std::string* error, const std::string& what) {
    if (error) *error = what;
    return false;
}

bool Factory(Com<IWICImagingFactory>& f, std::string* error) {
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.p));
    return SUCCEEDED(hr) || Fail(error, "WIC factory (is COM initialised on this thread?)", hr);
}

// WebP's container GUID (in wincodec.h from the 1809 SDK on; spelled out for older SDKs)
const GUID kContainerWebp = {0xe094b0e2, 0x67f2, 0x45b3, {0xb0, 0xea, 0x11, 0x53, 0x37, 0xca, 0x7c, 0xf3}};

GUID ContainerOf(PictureFormat f) {
    switch (f) {
        case PictureFormat::png: return GUID_ContainerFormatPng;
        case PictureFormat::jpeg: return GUID_ContainerFormatJpeg;
        case PictureFormat::bmp: return GUID_ContainerFormatBmp;
        case PictureFormat::gif: return GUID_ContainerFormatGif;
        case PictureFormat::tiff: return GUID_ContainerFormatTiff;
        case PictureFormat::webp: return kContainerWebp;
        default: return GUID_NULL;
    }
}

const char kNotAPicture[] = "not a picture the mod reads (PNG, JPEG, BMP, TIFF, GIF or WebP)";

// The first frame of a picture SniffPicture knows, opened with its own container's decoder (never by asking every codec
// installed to probe the bytes). `stream` and `dec` keep the frame alive; `data` must outlive them (read in place).
bool OpenFrame(IWICImagingFactory* f, const std::uint8_t* data, std::size_t size, PictureFormat fmt, Com<IWICStream>& stream,
               Com<IWICBitmapDecoder>& dec, Com<IWICBitmapFrameDecode>& frame, std::string* error) {
    if (fmt == PictureFormat::unknown || size > 0x7FFFFFFFu) return Say(error, kNotAPicture);
    const GUID container = ContainerOf(fmt);
    HRESULT hr = f->CreateStream(&stream);
    if (FAILED(hr)) return Fail(error, "CreateStream", hr);
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (FAILED(hr)) return Fail(error, "InitializeFromMemory", hr);
    hr = f->CreateDecoder(container, &GUID_VendorMicrosoft, &dec);
    if (FAILED(hr)) {
        if (fmt == PictureFormat::webp)
            return Say(error, "a WebP picture, but this Windows has no WebP reader (the WebP Image Extension): save it as PNG");
        return Fail(error, "picture decoder", hr);
    }
    hr = dec->Initialize(stream.p, WICDecodeMetadataCacheOnDemand);
    if (FAILED(hr)) return Fail(error, (std::string("reading the ") + PictureFormatName(fmt) + " header").c_str(), hr);
    GUID got{};
    if (FAILED(dec->GetContainerFormat(&got)) || got != container) return Say(error, kNotAPicture);
    UINT frames = 0;
    hr = dec->GetFrameCount(&frames);
    if (FAILED(hr) || frames == 0) return Fail(error, "no picture in the file", FAILED(hr) ? hr : E_FAIL);
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) return Fail(error, "GetFrame", hr);
    return true;
}

// One encoder for EncodePng and EncodePicture: `bpp` 1 (grey: R), 3 (BGR) or 4 (BGRA) from `img`'s RGBA.
bool Encode(const Image& img, const GUID& container, const WICPixelFormatGUID& want, UINT bpp, std::vector<std::uint8_t>& out,
            std::string* error) {
    out.clear();
    if (img.w == 0 || img.h == 0 || img.rgba.size() != static_cast<std::size_t>(img.w) * img.h * 4u) return Say(error, "empty image");
    Com<IWICImagingFactory> f;
    if (!Factory(f, error)) return false;
    Com<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) return Fail(error, "CreateStreamOnHGlobal", hr);
    Com<IWICBitmapEncoder> enc;
    hr = f->CreateEncoder(container, nullptr, &enc);
    if (FAILED(hr)) return Fail(error, "picture encoder", hr);
    hr = enc->Initialize(stream.p, WICBitmapEncoderNoCache);
    if (FAILED(hr)) return Fail(error, "encoder Initialize", hr);
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    hr = enc->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return Fail(error, "CreateNewFrame", hr);
    if (props) {  // best effort: an older WIC keeps its defaults
        PROPBAG2 opt{};
        VARIANT v;
        VariantInit(&v);
        if (container == GUID_ContainerFormatPng) {
            opt.pstrName = const_cast<LPOLESTR>(L"FilterOption");
            v.vt = VT_UI1;
            v.bVal = WICPngFilterNone;
            props->Write(1, &opt, &v);
        } else if (container == GUID_ContainerFormatJpeg) {
            opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
            v.vt = VT_R4;
            v.fltVal = 0.9f;
            props->Write(1, &opt, &v);
        }
    }
    hr = frame->Initialize(props.p);
    if (FAILED(hr)) return Fail(error, "frame Initialize", hr);
    hr = frame->SetSize(img.w, img.h);
    if (FAILED(hr)) return Fail(error, "SetSize", hr);
    WICPixelFormatGUID got = want;
    hr = frame->SetPixelFormat(&got);
    if (FAILED(hr) || got != want) return Fail(error, "SetPixelFormat", hr);
    const UINT stride = img.w * bpp;
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(stride) * img.h);
    const std::size_t n = static_cast<std::size_t>(img.w) * img.h;
    for (std::size_t i = 0; i < n; ++i) {
        const std::uint8_t* s = &img.rgba[i * 4];
        std::uint8_t* d = &buf[i * bpp];
        if (bpp == 1) {
            d[0] = s[0];
        } else {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            if (bpp == 4) d[3] = s[3];
        }
    }
    hr = frame->WritePixels(img.h, stride, static_cast<UINT>(buf.size()), buf.data());
    if (FAILED(hr)) return Fail(error, "WritePixels", hr);
    hr = frame->Commit();
    if (FAILED(hr)) return Fail(error, "frame Commit", hr);
    hr = enc->Commit();
    if (FAILED(hr)) return Fail(error, "encoder Commit", hr);
    STATSTG st{};
    hr = stream->Stat(&st, STATFLAG_NONAME);
    if (FAILED(hr)) return Fail(error, "Stat", hr);
    HGLOBAL mem = nullptr;
    hr = GetHGlobalFromStream(stream.p, &mem);
    if (FAILED(hr)) return Fail(error, "GetHGlobalFromStream", hr);
    const void* p = GlobalLock(mem);
    if (!p) return Fail(error, "GlobalLock", E_FAIL);
    out.assign(static_cast<const std::uint8_t*>(p), static_cast<const std::uint8_t*>(p) + st.cbSize.QuadPart);
    GlobalUnlock(mem);
    return true;
}

bool ReadFileBytes(const std::wstring& path, std::vector<std::uint8_t>& bytes, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return Say(error, "cannot open the file");
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(file, &sz) && sz.QuadPart > 0 && sz.QuadPart < (512ll << 20);
    if (ok) {
        bytes.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(file);
    return ok || Say(error, "cannot read the file");
}

}  // namespace

bool EncodePng(const Image& img, PngLayout layout, std::vector<std::uint8_t>& out, std::string* error) {
    const WICPixelFormatGUID want = layout == PngLayout::gray8   ? GUID_WICPixelFormat8bppGray
                                    : layout == PngLayout::rgb24 ? GUID_WICPixelFormat24bppBGR
                                                                 : GUID_WICPixelFormat32bppBGRA;
    const UINT bpp = layout == PngLayout::gray8 ? 1u : layout == PngLayout::rgb24 ? 3u : 4u;
    return Encode(img, GUID_ContainerFormatPng, want, bpp, out, error);
}

bool DecodePng(const std::uint8_t* data, std::size_t size, Image& out, std::string* error) {
    const PictureFormat fmt = SniffPicture(data, size);
    if (fmt != PictureFormat::png)
        return Say(error, fmt == PictureFormat::unknown ? std::string("not a PNG") : std::string("not a PNG (a ") + PictureFormatName(fmt) + " picture)");
    return DecodePicture(data, size, out, 0, 0, nullptr, error);
}

bool ReadPngFile(const std::wstring& path, Image& out, std::string* error) {
    std::vector<std::uint8_t> bytes;
    return ReadFileBytes(path, bytes, error) && DecodePng(bytes.data(), bytes.size(), out, error);
}

bool PngSize(const std::uint8_t* data, std::size_t size, std::uint32_t& w, std::uint32_t& h) {
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (!data || size < 24 || std::memcmp(data, kSig, 8) != 0 || std::memcmp(data + 12, "IHDR", 4) != 0) return false;
    auto be = [&](std::size_t o) {
        return (static_cast<std::uint32_t>(data[o]) << 24) | (static_cast<std::uint32_t>(data[o + 1]) << 16) |
               (static_cast<std::uint32_t>(data[o + 2]) << 8) | data[o + 3];
    };
    w = be(16);
    h = be(20);
    return w > 0 && h > 0;
}

const char* PictureFormatName(PictureFormat f) {
    switch (f) {
        case PictureFormat::png: return "PNG";
        case PictureFormat::jpeg: return "JPEG";
        case PictureFormat::bmp: return "BMP";
        case PictureFormat::gif: return "GIF";
        case PictureFormat::tiff: return "TIFF";
        case PictureFormat::webp: return "WebP";
        default: return "unknown";
    }
}

PictureFormat SniffPicture(const std::uint8_t* d, std::size_t n) {
    if (!d) return PictureFormat::unknown;
    static const std::uint8_t kPng[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    static const std::uint8_t kTiffLe[4] = {'I', 'I', 0x2A, 0x00};
    static const std::uint8_t kTiffBe[4] = {'M', 'M', 0x00, 0x2A};
    if (n >= 8 && std::memcmp(d, kPng, 8) == 0) return PictureFormat::png;
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return PictureFormat::jpeg;
    if (n >= 26 && d[0] == 'B' && d[1] == 'M') return PictureFormat::bmp;
    if (n >= 6 && (std::memcmp(d, "GIF87a", 6) == 0 || std::memcmp(d, "GIF89a", 6) == 0)) return PictureFormat::gif;
    if (n >= 8 && (std::memcmp(d, kTiffLe, 4) == 0 || std::memcmp(d, kTiffBe, 4) == 0)) return PictureFormat::tiff;
    if (n >= 12 && std::memcmp(d, "RIFF", 4) == 0 && std::memcmp(d + 8, "WEBP", 4) == 0) return PictureFormat::webp;
    return PictureFormat::unknown;
}

bool PictureSize(const std::uint8_t* data, std::size_t size, std::uint32_t& w, std::uint32_t& h, PictureFormat* format, std::string* error) {
    w = h = 0;
    const PictureFormat fmt = SniffPicture(data, size);
    if (format) *format = fmt;
    if (fmt == PictureFormat::unknown) return Say(error, kNotAPicture);
    if (fmt == PictureFormat::png) return PngSize(data, size, w, h) || Say(error, "a damaged PNG header");
    Com<IWICImagingFactory> f;
    if (!Factory(f, error)) return false;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    if (!OpenFrame(f.p, data, size, fmt, stream, dec, frame, error)) return false;
    UINT fw = 0, fh = 0;  // the header's size: no pixel is decoded
    const HRESULT hr = frame->GetSize(&fw, &fh);
    if (FAILED(hr) || fw == 0 || fh == 0) return Fail(error, "picture size", FAILED(hr) ? hr : E_FAIL);
    w = fw;
    h = fh;
    return true;
}

bool DecodePicture(const std::uint8_t* data, std::size_t size, Image& out, std::uint32_t max_w, std::uint32_t max_h, PictureFormat* format,
                   std::string* error) {
    const PictureFormat fmt = SniffPicture(data, size);
    if (format) *format = fmt;
    if (fmt == PictureFormat::unknown) return Say(error, kNotAPicture);
    if (max_w == 0 || max_w > 16384) max_w = 16384;
    if (max_h == 0 || max_h > 16384) max_h = 16384;
    Com<IWICImagingFactory> f;
    if (!Factory(f, error)) return false;
    Com<IWICStream> stream;
    Com<IWICBitmapDecoder> dec;
    Com<IWICBitmapFrameDecode> frame;
    if (!OpenFrame(f.p, data, size, fmt, stream, dec, frame, error)) return false;
    UINT w = 0, h = 0;
    HRESULT hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0) return Fail(error, "picture size", FAILED(hr) ? hr : E_FAIL);
    if (w > max_w || h > max_h) {  // refused from the header: nothing decoded, nothing allocated
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%ux%u, too big (at most %ux%u)", w, h, max_w, max_h);
        return Say(error, buf);
    }
    Com<IWICBitmapSource> conv;
    hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.p, &conv);
    if (FAILED(hr)) return Fail(error, "convert to RGBA", hr);
    out.Resize(w, h);
    hr = conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(out.rgba.size()), out.rgba.data());
    if (FAILED(hr)) return Fail(error, "CopyPixels", hr);
    return true;
}

bool ReadPictureFile(const std::wstring& path, Image& out, PictureFormat* format, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (format) *format = PictureFormat::unknown;
    return ReadFileBytes(path, bytes, error) && DecodePicture(bytes.data(), bytes.size(), out, 0, 0, format, error);
}

bool EncodePicture(const Image& img, PictureFormat format, std::vector<std::uint8_t>& out, std::string* error) {
    switch (format) {
        case PictureFormat::png:
        case PictureFormat::jpeg:
        case PictureFormat::bmp:
        case PictureFormat::tiff:
            return Encode(img, ContainerOf(format), GUID_WICPixelFormat24bppBGR, 3u, out, error);
        default:
            out.clear();
            return Say(error, std::string("cannot write ") + PictureFormatName(format));
    }
}

}  // namespace st::tex
