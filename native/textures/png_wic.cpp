// Stranded Together textures: PNG read / write through WIC (png_wic.hpp).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objidl.h>
#include <wincodec.h>

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
        char buf[96];
        std::snprintf(buf, sizeof(buf), "%s failed (0x%08lX)", what, static_cast<unsigned long>(hr));
        *error = buf;
    }
    return false;
}

bool Factory(Com<IWICImagingFactory>& f, std::string* error) {
    const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f.p));
    return SUCCEEDED(hr) || Fail(error, "WIC factory (is COM initialised on this thread?)", hr);
}

}  // namespace

bool EncodePng(const Image& img, PngLayout layout, std::vector<std::uint8_t>& out, std::string* error) {
    out.clear();
    if (img.w == 0 || img.h == 0 || img.rgba.size() != static_cast<std::size_t>(img.w) * img.h * 4u) {
        if (error) *error = "empty image";
        return false;
    }
    Com<IWICImagingFactory> f;
    if (!Factory(f, error)) return false;
    Com<IStream> stream;
    HRESULT hr = CreateStreamOnHGlobal(nullptr, TRUE, &stream);
    if (FAILED(hr)) return Fail(error, "CreateStreamOnHGlobal", hr);
    Com<IWICBitmapEncoder> enc;
    hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc);
    if (FAILED(hr)) return Fail(error, "PNG encoder", hr);
    hr = enc->Initialize(stream.p, WICBitmapEncoderNoCache);
    if (FAILED(hr)) return Fail(error, "encoder Initialize", hr);
    Com<IWICBitmapFrameEncode> frame;
    Com<IPropertyBag2> props;
    hr = enc->CreateNewFrame(&frame, &props);
    if (FAILED(hr)) return Fail(error, "CreateNewFrame", hr);
    if (props) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"FilterOption");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_UI1;
        v.bVal = WICPngFilterNone;
        props->Write(1, &opt, &v);   // best effort: an older WIC keeps its default filter
    }
    hr = frame->Initialize(props.p);
    if (FAILED(hr)) return Fail(error, "frame Initialize", hr);
    hr = frame->SetSize(img.w, img.h);
    if (FAILED(hr)) return Fail(error, "SetSize", hr);
    WICPixelFormatGUID want = layout == PngLayout::gray8 ? GUID_WICPixelFormat8bppGray
                              : layout == PngLayout::rgb24 ? GUID_WICPixelFormat24bppBGR
                                                           : GUID_WICPixelFormat32bppBGRA;
    WICPixelFormatGUID got = want;
    hr = frame->SetPixelFormat(&got);
    if (FAILED(hr) || got != want) return Fail(error, "SetPixelFormat", hr);
    const UINT bpp = layout == PngLayout::gray8 ? 1u : layout == PngLayout::rgb24 ? 3u : 4u;
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

bool DecodePng(const std::uint8_t* data, std::size_t size, Image& out, std::string* error) {
    if (!data || size < 8 || size > 0x7FFFFFFFu) {
        if (error) *error = "not a PNG";
        return false;
    }
    Com<IWICImagingFactory> f;
    if (!Factory(f, error)) return false;
    Com<IWICStream> stream;
    HRESULT hr = f->CreateStream(&stream);
    if (FAILED(hr)) return Fail(error, "CreateStream", hr);
    hr = stream->InitializeFromMemory(const_cast<BYTE*>(data), static_cast<DWORD>(size));
    if (FAILED(hr)) return Fail(error, "InitializeFromMemory", hr);
    Com<IWICBitmapDecoder> dec;
    hr = f->CreateDecoderFromStream(stream.p, nullptr, WICDecodeMetadataCacheOnDemand, &dec);
    if (FAILED(hr)) return Fail(error, "not a readable picture", hr);
    GUID container{};
    if (SUCCEEDED(dec->GetContainerFormat(&container)) && container != GUID_ContainerFormatPng) {
        if (error) *error = "not a PNG (another picture format)";
        return false;
    }
    Com<IWICBitmapFrameDecode> frame;
    hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) return Fail(error, "GetFrame", hr);
    UINT w = 0, h = 0;
    hr = frame->GetSize(&w, &h);
    if (FAILED(hr) || w == 0 || h == 0 || w > 16384 || h > 16384) return Fail(error, "picture size", hr);
    Com<IWICBitmapSource> conv;
    hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppRGBA, frame.p, &conv);
    if (FAILED(hr)) return Fail(error, "convert to RGBA", hr);
    out.Resize(w, h);
    hr = conv->CopyPixels(nullptr, w * 4u, static_cast<UINT>(out.rgba.size()), out.rgba.data());
    if (FAILED(hr)) return Fail(error, "CopyPixels", hr);
    return true;
}

bool ReadPngFile(const std::wstring& path, Image& out, std::string* error) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open the file";
        return false;
    }
    LARGE_INTEGER sz{};
    std::vector<std::uint8_t> bytes;
    bool ok = GetFileSizeEx(file, &sz) && sz.QuadPart > 0 && sz.QuadPart < (512ll << 20);
    if (ok) {
        bytes.resize(static_cast<std::size_t>(sz.QuadPart));
        DWORD got = 0;
        ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &got, nullptr) && got == bytes.size();
    }
    CloseHandle(file);
    if (!ok) {
        if (error) *error = "cannot read the file";
        return false;
    }
    return DecodePng(bytes.data(), bytes.size(), out, error);
}

bool PngSize(const std::uint8_t* data, std::size_t size, std::uint32_t& w, std::uint32_t& h) {
    static const std::uint8_t kSig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    if (size < 24 || std::memcmp(data, kSig, 8) != 0 || std::memcmp(data + 12, "IHDR", 4) != 0) return false;
    auto be = [&](std::size_t o) {
        return (static_cast<std::uint32_t>(data[o]) << 24) | (static_cast<std::uint32_t>(data[o + 1]) << 16) |
               (static_cast<std::uint32_t>(data[o + 2]) << 8) | data[o + 3];
    };
    w = be(16);
    h = be(20);
    return w > 0 && h > 0;
}

}  // namespace st::tex
