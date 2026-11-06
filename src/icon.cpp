// icon.cpp - SVG veya PNG dosyasindan cok boyutlu ICO ureten komut satiri / surukle-birak araci.
//
// Kullanım:
//   icon -p "png dosyasi" [-o "ico cikti klasoru"]
//   icon -s "svg dosyasi" [-o "ico cikti klasoru"]
//   icon "dosya.png" "baska.svg" ...        (sürükle-bırak ile aynı)
//
// ICO icerigi (vectoral-icon skill kurallari):
//   * 16, 24, 32, 48, 64, 128, 256 piksel, 32 bit RGBA girisler (256 -> PNG sikistirmali)
//   * 16..128 icin ek olarak 8 bit (256 renk paletli, 1 bit AND maskeli) girisler
//   * SVG her boyutta dogrudan o piksel boyutunda render edilir; PNG kaynak yuksek kaliteli
//     (sRGB, alfa agirlikli) yeniden ornekleme ile kucultulur, buyutme yapilmaz.
//
// Derleme: bkz. build.bat / README.md   (C++17)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image_resize2.h"
#define NANOSVG_ALL_COLOR_KEYWORDS
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

namespace fs = std::filesystem;

// ----------------------------------------------------------------------------
// Ortak tipler
// ----------------------------------------------------------------------------

struct Report {
    std::vector<std::string> info, warn, err;
};

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> px;  // RGBA8, alfa duz (premultiplied degil), satir satir
};

static const int kSizes[] = {16, 24, 32, 48, 64, 128, 256};
static const int kSizeCount = (int)(sizeof(kSizes) / sizeof(kSizes[0]));

static std::string U8(const fs::path& p) {
    auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

static fs::path PathFromU8(const std::string& s) { return fs::u8path(s); }

static std::string Abs(const fs::path& p) {
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);
    return U8(ec ? p : a);
}

// ----------------------------------------------------------------------------
// Dosya G/C
// ----------------------------------------------------------------------------

static FILE* OpenFile(const fs::path& p, bool write) {
#ifdef _WIN32
    return _wfopen(p.c_str(), write ? L"wb" : L"rb");
#else
    return fopen(p.c_str(), write ? "wb" : "rb");
#endif
}

static bool ReadAll(const fs::path& p, std::vector<uint8_t>& out, std::string& why) {
    std::error_code ec;
    if (!fs::exists(p, ec)) { why = "dosya bulunamadı"; return false; }
    if (fs::is_directory(p, ec)) { why = "bu bir klasör, dosya değil"; return false; }
    uintmax_t sz = fs::file_size(p, ec);
    if (ec) { why = "dosya boyutu okunamadı"; return false; }
    if (sz == 0) { why = "dosya boş"; return false; }
    if (sz > (uintmax_t)256 * 1024 * 1024) { why = "dosya çok büyük (256 MB sınırını aşıyor)"; return false; }
    FILE* f = OpenFile(p, false);
    if (!f) { why = "dosya açılamadı (erişim izni var mı, başka program kullanıyor mu?)"; return false; }
    out.resize((size_t)sz);
    size_t got = fread(out.data(), 1, out.size(), f);
    fclose(f);
    if (got != out.size()) { why = "dosya tamamen okunamadı"; return false; }
    return true;
}

static bool WriteAll(const fs::path& p, const std::vector<uint8_t>& data, std::string& why) {
    FILE* f = OpenFile(p, true);
    if (!f) { why = "çıktı dosyası oluşturulamadı (klasörde yazma izni var mı, dosya başka programda açık mı?)"; return false; }
    size_t put = fwrite(data.data(), 1, data.size(), f);
    int cl = fclose(f);
    if (put != data.size() || cl != 0) { why = "çıktı dosyası tamamen yazılamadı (disk dolu olabilir)"; return false; }
    return true;
}

// ----------------------------------------------------------------------------
// Kaynak: PNG (kare ana gorsel + yeniden ornekleme) ve SVG (dogrudan render)
// ----------------------------------------------------------------------------

struct Source {
    virtual ~Source() {}
    virtual int MaxSize() const = 0;  // bu boyuta kadar (dahil) buyutmeden uretilebilir
    virtual Image Render(int size) = 0;
};

struct PngSource : Source {
    int s = 0;  // kare ana gorselin kenari
    std::vector<uint8_t> master;
    int MaxSize() const override { return s; }
    Image Render(int size) override {
        Image o;
        o.w = o.h = size;
        if (size == s) { o.px = master; return o; }
        o.px.assign((size_t)size * size * 4, 0);
        stbir_resize_uint8_srgb(master.data(), s, s, 0, o.px.data(), size, size, 0, STBIR_RGBA);
        return o;
    }
};

static bool LoadPng(const std::vector<uint8_t>& buf, PngSource& out, std::string& why) {
    int w = 0, h = 0, n = 0;
    unsigned char* data = stbi_load_from_memory(buf.data(), (int)buf.size(), &w, &h, &n, 4);
    if (!data) {
        const char* r = stbi_failure_reason();
        why = std::string("PNG çözülemedi") + (r ? std::string(" (") + r + ")" : "");
        return false;
    }
    if (w < 1 || h < 1 || w > 16384 || h > 16384) {
        stbi_image_free(data);
        why = "PNG boyutu desteklenmiyor (en fazla 16384 piksel)";
        return false;
    }
    int s = std::max(w, h);
    out.s = s;
    out.master.assign((size_t)s * s * 4, 0);  // seffaf kare tuval, gorsel ortalanir
    int ox = (s - w) / 2, oy = (s - h) / 2;
    for (int y = 0; y < h; y++)
        memcpy(&out.master[((size_t)(y + oy) * s + ox) * 4], data + (size_t)y * w * 4, (size_t)w * 4);
    stbi_image_free(data);
    return true;
}

struct SvgSource : Source {
    NSVGimage* img = nullptr;
    NSVGrasterizer* rast = nullptr;
    ~SvgSource() override {
        if (rast) nsvgDeleteRasterizer(rast);
        if (img) nsvgDelete(img);
    }
    int MaxSize() const override { return 1 << 30; }
    Image Render(int size) override {
        Image o;
        o.w = o.h = size;
        o.px.assign((size_t)size * size * 4, 0);
        float sw = img->width, sh = img->height;
        float S = std::max(sw, sh);
        float scale = (float)size / S;
        float tx = ((float)size - sw * scale) * 0.5f;
        float ty = ((float)size - sh * scale) * 0.5f;
        nsvgRasterize(rast, img, tx, ty, scale, o.px.data(), size, size, size * 4);
        return o;
    }
};

static bool LoadSvg(const std::vector<uint8_t>& buf, SvgSource& out, std::vector<std::string>& unsupported,
                    std::string& why) {
    size_t start = 0;
    if (buf.size() >= 3 && buf[0] == 0xEF && buf[1] == 0xBB && buf[2] == 0xBF) start = 3;  // UTF-8 BOM
    std::vector<char> text(buf.begin() + start, buf.end());
    text.push_back('\0');

    static const char* kUnsupported[] = {"<text", "<filter", "<mask", "<clipPath", "<use", "<image",
                                         "<pattern", "<style", "<foreignObject"};
    std::string hay(text.data());
    for (const char* tag : kUnsupported)
        if (hay.find(tag) != std::string::npos) unsupported.push_back(std::string(tag + 1));

    out.img = nsvgParse(text.data(), "px", 96.0f);
    if (!out.img) { why = "SVG çözülemedi (geçerli bir SVG değil)"; return false; }
    if (!(out.img->width > 0.0f) || !(out.img->height > 0.0f)) {
        why = "SVG boyutu okunamadı (width/height veya viewBox gerekli)";
        return false;
    }
    if (!out.img->shapes) { why = "SVG içinde çizilebilir şekil yok"; return false; }
    out.rast = nsvgCreateRasterizer();
    if (!out.rast) { why = "render motoru başlatılamadı"; return false; }
    return true;
}

// ----------------------------------------------------------------------------
// 8 bit paletli nicemleme (median cut + Floyd-Steinberg), indeks 0 = seffaf
// ----------------------------------------------------------------------------

struct Pal8 {
    uint8_t rgb[256][3] = {};
    int n = 1;                    // kullanilan palet girisi (0. giris seffaf icin ayrilmis)
    std::vector<uint8_t> idx;     // piksel basina palet indeksi
    std::vector<uint8_t> opaque;  // 1 = opak (alfa >= 128)
};

static void Quantize(const Image& im, Pal8& q) {
    const int w = im.w, h = im.h, N = w * h;
    q.idx.assign(N, 0);
    q.opaque.assign(N, 0);
    std::map<uint32_t, uint32_t> hist;
    for (int i = 0; i < N; i++) {
        const uint8_t* p = &im.px[(size_t)i * 4];
        if (p[3] >= 128) {
            q.opaque[i] = 1;
            hist[((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]]++;
        }
    }
    struct Col { uint8_t c[3]; uint32_t n; };
    std::vector<Col> cols;
    cols.reserve(hist.size());
    for (auto& kv : hist) {
        Col c;
        c.c[0] = (uint8_t)(kv.first >> 16);
        c.c[1] = (uint8_t)(kv.first >> 8);
        c.c[2] = (uint8_t)kv.first;
        c.n = kv.second;
        cols.push_back(c);
    }
    const size_t kMax = 255;
    std::vector<std::array<uint8_t, 3>> pal;
    const bool exact = cols.size() <= kMax;
    if (exact) {
        for (auto& c : cols) pal.push_back({c.c[0], c.c[1], c.c[2]});
    } else {
        struct Box { size_t lo, hi; };
        std::vector<Box> boxes{{0, cols.size()}};
        auto boxRange = [&](const Box& b, int& ch) {
            int mn[3] = {255, 255, 255}, mx[3] = {0, 0, 0};
            for (size_t i = b.lo; i < b.hi; i++)
                for (int k = 0; k < 3; k++) {
                    mn[k] = std::min<int>(mn[k], cols[i].c[k]);
                    mx[k] = std::max<int>(mx[k], cols[i].c[k]);
                }
            int best = -1;
            ch = 0;
            for (int k = 0; k < 3; k++)
                if (mx[k] - mn[k] > best) { best = mx[k] - mn[k]; ch = k; }
            return best;
        };
        while (boxes.size() < kMax) {
            int bi = -1, br = 0, bch = 0;
            for (size_t i = 0; i < boxes.size(); i++) {
                if (boxes[i].hi - boxes[i].lo < 2) continue;
                int ch;
                int r = boxRange(boxes[i], ch);
                if (r > br) { br = r; bi = (int)i; bch = ch; }
            }
            if (bi < 0) break;
            Box b = boxes[bi];
            std::sort(cols.begin() + b.lo, cols.begin() + b.hi,
                      [bch](const Col& a, const Col& c) { return a.c[bch] < c.c[bch]; });
            uint64_t total = 0;
            for (size_t i = b.lo; i < b.hi; i++) total += cols[i].n;
            uint64_t acc = 0;
            size_t mid = b.lo + 1;
            for (size_t i = b.lo; i + 1 < b.hi; i++) {
                acc += cols[i].n;
                mid = i + 1;
                if (acc * 2 >= total) break;
            }
            boxes[bi] = {b.lo, mid};
            boxes.push_back({mid, b.hi});
        }
        for (auto& b : boxes) {
            uint64_t sum[3] = {0, 0, 0}, tot = 0;
            for (size_t i = b.lo; i < b.hi; i++) {
                for (int k = 0; k < 3; k++) sum[k] += (uint64_t)cols[i].c[k] * cols[i].n;
                tot += cols[i].n;
            }
            if (!tot) continue;
            pal.push_back({(uint8_t)((sum[0] + tot / 2) / tot), (uint8_t)((sum[1] + tot / 2) / tot),
                           (uint8_t)((sum[2] + tot / 2) / tot)});
        }
    }
    q.n = 1 + (int)pal.size();
    q.rgb[0][0] = q.rgb[0][1] = q.rgb[0][2] = 0;
    for (size_t i = 0; i < pal.size(); i++)
        for (int k = 0; k < 3; k++) q.rgb[1 + i][k] = pal[i][k];

    auto nearest = [&](float r, float g, float b) {
        int best = 0;
        float bd = 1e30f;
        for (size_t i = 0; i < pal.size(); i++) {
            float dr = r - pal[i][0], dg = g - pal[i][1], db = b - pal[i][2];
            float d = dr * dr + dg * dg + db * db;
            if (d < bd) { bd = d; best = (int)i; }
        }
        return best;
    };

    if (exact) {
        std::unordered_map<uint32_t, int> lut;
        for (size_t i = 0; i < pal.size(); i++)
            lut[((uint32_t)pal[i][0] << 16) | ((uint32_t)pal[i][1] << 8) | pal[i][2]] = (int)i;
        for (int i = 0; i < N; i++) {
            if (!q.opaque[i]) continue;
            const uint8_t* p = &im.px[(size_t)i * 4];
            q.idx[i] = (uint8_t)(1 + lut[((uint32_t)p[0] << 16) | ((uint32_t)p[1] << 8) | p[2]]);
        }
        return;
    }
    // Floyd-Steinberg titresimi: degradelerde bant olusumunu azaltir.
    std::vector<float> err((size_t)N * 3, 0.0f);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int i = y * w + x;
            if (!q.opaque[i]) continue;
            const uint8_t* p = &im.px[(size_t)i * 4];
            float v[3];
            for (int k = 0; k < 3; k++) v[k] = std::min(255.0f, std::max(0.0f, p[k] + err[(size_t)i * 3 + k]));
            int b = nearest(v[0], v[1], v[2]);
            q.idx[i] = (uint8_t)(1 + b);
            for (int k = 0; k < 3; k++) {
                float e = v[k] - pal[b][k];
                if (x + 1 < w) err[((size_t)i + 1) * 3 + k] += e * 7.0f / 16.0f;
                if (y + 1 < h) {
                    if (x > 0) err[((size_t)i + w - 1) * 3 + k] += e * 3.0f / 16.0f;
                    err[((size_t)i + w) * 3 + k] += e * 5.0f / 16.0f;
                    if (x + 1 < w) err[((size_t)i + w + 1) * 3 + k] += e * 1.0f / 16.0f;
                }
            }
        }
    }
}

// ----------------------------------------------------------------------------
// ICO yazici
// ----------------------------------------------------------------------------

static void Put16(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back((uint8_t)v);
    b.push_back((uint8_t)(v >> 8));
}
static void Put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; i++) b.push_back((uint8_t)(v >> (8 * i)));
}

static void PutDibHeader(std::vector<uint8_t>& d, int w, int h, int bpp, uint32_t imageSize, uint32_t clrUsed) {
    Put32(d, 40);
    Put32(d, (uint32_t)w);
    Put32(d, (uint32_t)(h * 2));  // XOR + AND maskesi
    Put16(d, 1);
    Put16(d, (uint32_t)bpp);
    Put32(d, 0);  // BI_RGB
    Put32(d, imageSize);
    Put32(d, 0);
    Put32(d, 0);
    Put32(d, clrUsed);
    Put32(d, 0);
}

static std::vector<uint8_t> MakeDib32(const Image& im) {
    const int w = im.w, h = im.h;
    const int maskRow = ((w + 31) / 32) * 4;
    std::vector<uint8_t> d;
    d.reserve(40 + (size_t)w * h * 4 + (size_t)maskRow * h);
    PutDibHeader(d, w, h, 32, (uint32_t)(w * h * 4 + maskRow * h), 0);
    for (int y = h - 1; y >= 0; y--)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = &im.px[((size_t)y * w + x) * 4];
            d.push_back(p[2]);
            d.push_back(p[1]);
            d.push_back(p[0]);
            d.push_back(p[3]);
        }
    for (int y = h - 1; y >= 0; y--) {
        std::vector<uint8_t> row(maskRow, 0);
        for (int x = 0; x < w; x++)
            if (im.px[((size_t)y * w + x) * 4 + 3] == 0) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        d.insert(d.end(), row.begin(), row.end());
    }
    return d;
}

static std::vector<uint8_t> MakeDib8(const Image& im) {
    const int w = im.w, h = im.h;
    Pal8 q;
    Quantize(im, q);
    const int pixRow = (w + 3) & ~3;
    const int maskRow = ((w + 31) / 32) * 4;
    std::vector<uint8_t> d;
    PutDibHeader(d, w, h, 8, (uint32_t)(pixRow * h + maskRow * h), 256);
    for (int i = 0; i < 256; i++) {  // palet: B, G, R, 0
        if (i < q.n) {
            d.push_back(q.rgb[i][2]);
            d.push_back(q.rgb[i][1]);
            d.push_back(q.rgb[i][0]);
        } else {
            d.push_back(0); d.push_back(0); d.push_back(0);
        }
        d.push_back(0);
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = 0; x < w; x++) d.push_back(q.idx[(size_t)y * w + x]);
        for (int x = w; x < pixRow; x++) d.push_back(0);
    }
    for (int y = h - 1; y >= 0; y--) {
        std::vector<uint8_t> row(maskRow, 0);
        for (int x = 0; x < w; x++)
            if (!q.opaque[(size_t)y * w + x]) row[x >> 3] |= (uint8_t)(0x80 >> (x & 7));
        d.insert(d.end(), row.begin(), row.end());
    }
    return d;
}

static std::vector<uint8_t> MakePng(const Image& im, std::string& why) {
    stbi_write_png_compression_level = 9;
    int len = 0;
    unsigned char* p = stbi_write_png_to_mem(im.px.data(), im.w * 4, im.w, im.h, 4, &len);
    if (!p || len <= 0) { why = "PNG sıkıştırma başarısız"; return {}; }
    std::vector<uint8_t> out(p, p + len);
    STBIW_FREE(p);
    return out;
}

struct Entry {
    int size, bpp;
    std::vector<uint8_t> data;
};

static std::vector<uint8_t> BuildIco(std::vector<Entry>& ents) {
    std::stable_sort(ents.begin(), ents.end(), [](const Entry& a, const Entry& b) {
        if (a.size != b.size) return a.size < b.size;
        return a.bpp > b.bpp;  // ayni boyutta once 32 bit
    });
    std::vector<uint8_t> out;
    Put16(out, 0);
    Put16(out, 1);
    Put16(out, (uint32_t)ents.size());
    uint32_t off = 6 + 16 * (uint32_t)ents.size();
    for (auto& e : ents) {
        out.push_back((uint8_t)(e.size >= 256 ? 0 : e.size));
        out.push_back((uint8_t)(e.size >= 256 ? 0 : e.size));
        out.push_back(0);  // renk sayisi (>= 8 bit icin 0)
        out.push_back(0);
        Put16(out, 1);
        Put16(out, (uint32_t)e.bpp);
        Put32(out, (uint32_t)e.data.size());
        Put32(out, off);
        off += (uint32_t)e.data.size();
    }
    for (auto& e : ents) out.insert(out.end(), e.data.begin(), e.data.end());
    return out;
}

// ----------------------------------------------------------------------------
// Donusum
// ----------------------------------------------------------------------------

enum class Kind { Auto, Png, Svg };

struct Job {
    fs::path in;
    Kind kind;
};

static bool IsPng(const std::vector<uint8_t>& b) {
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    return b.size() >= 8 && memcmp(b.data(), sig, 8) == 0;
}

static bool LooksLikeSvg(const std::vector<uint8_t>& b) {
    size_t n = std::min<size_t>(b.size(), 65536);
    std::string head((const char*)b.data(), n);
    return head.find("<svg") != std::string::npos;
}

static bool ConvertOne(const Job& job, const fs::path& outPath, Report& rep) {
    const std::string name = U8(job.in.filename());
    std::vector<uint8_t> buf;
    std::string why;
    if (!ReadAll(job.in, buf, why)) {
        rep.err.push_back("'" + name + "': " + why + ".");
        return false;
    }
    Kind kind = job.kind;
    if (kind == Kind::Auto) {
        if (IsPng(buf)) kind = Kind::Png;
        else if (LooksLikeSvg(buf)) kind = Kind::Svg;
        else {
            rep.err.push_back("'" + name + "': desteklenmeyen dosya türü (PNG veya SVG olmalı).");
            return false;
        }
    } else if (kind == Kind::Png && !IsPng(buf)) {
        rep.err.push_back("'" + name + "': geçerli bir PNG dosyası değil (-p yalnızca PNG alır).");
        return false;
    }

    PngSource png;
    SvgSource svg;
    Source* src = nullptr;
    if (kind == Kind::Png) {
        if (!LoadPng(buf, png, why)) {
            rep.err.push_back("'" + name + "': " + why + ".");
            return false;
        }
        src = &png;
    } else {
        std::vector<std::string> unsupported;
        if (!LoadSvg(buf, svg, unsupported, why)) {
            rep.err.push_back("'" + name + "': " + why + ".");
            return false;
        }
        if (!unsupported.empty()) {
            std::string list;
            for (size_t i = 0; i < unsupported.size(); i++) list += (i ? ", " : "") + unsupported[i];
            rep.warn.push_back("'" + name + "': desteklenmeyen SVG öğeleri bulundu (" + list +
                               "); bunlar çizilmeyebilir. Metni yola (path) çevirin.");
        }
        src = &svg;
    }

    const int maxSize = src->MaxSize();
    if (maxSize < kSizes[0]) {
        rep.err.push_back("'" + name + "': görsel çok küçük (" + std::to_string(maxSize) + " piksel); en az " +
                          std::to_string(kSizes[0]) + " piksel olmalı.");
        return false;
    }

    std::vector<Entry> ents;
    std::vector<int> skipped;
    for (int i = 0; i < kSizeCount; i++) {
        int sz = kSizes[i];
        if (sz > maxSize) { skipped.push_back(sz); continue; }  // buyutme yapilmaz
        Image im = src->Render(sz);
        Entry e32;
        e32.size = sz;
        e32.bpp = 32;
        if (sz >= 256) {
            e32.data = MakePng(im, why);
            if (e32.data.empty()) {
                rep.err.push_back("'" + name + "': " + why + ".");
                return false;
            }
        } else {
            e32.data = MakeDib32(im);
        }
        ents.push_back(std::move(e32));
        if (sz < 256) {
            Entry e8;
            e8.size = sz;
            e8.bpp = 8;
            e8.data = MakeDib8(im);
            ents.push_back(std::move(e8));
        }
    }
    if (!skipped.empty()) {
        std::string list;
        for (size_t i = 0; i < skipped.size(); i++) list += (i ? ", " : "") + std::to_string(skipped[i]);
        rep.warn.push_back("'" + name + "': kaynak " + std::to_string(maxSize) + " piksel olduğu için " + list +
                           " piksel boyutları atlandı (büyütme yapılmaz).");
    }

    std::vector<uint8_t> ico = BuildIco(ents);
    if (!WriteAll(outPath, ico, why)) {
        rep.err.push_back("'" + U8(outPath) + "': " + why + ".");
        return false;
    }
    rep.info.push_back("Oluşturuldu: " + Abs(outPath) + "  (" + std::to_string(ents.size()) + " giriş)");
    return true;
}

// ----------------------------------------------------------------------------
// Komut satiri
// ----------------------------------------------------------------------------

static std::string Usage() {
    return
        "icon - SVG veya PNG dosyasından çok boyutlu ICO üretir\n"
        "\n"
        "Kullanım:\n"
        "  icon -p \"png dosyası\" [-o \"ico çıktı klasörü\"]\n"
        "  icon -s \"svg dosyası\" [-o \"ico çıktı klasörü\"]\n"
        "  icon \"dosya.png\" [\"baska.svg\" ...]      (sürükle-bırak ile aynı)\n"
        "\n"
        "Çıktı klasörü verilmezse ICO, girdi dosyasının bulunduğu klasöre yazılır.\n"
        "Çıktı dosyası: <girdi adı>.ico (varsa üzerine yazılır)\n"
        "ICO içeriği: 16, 24, 32, 48, 64, 128, 256 piksel 32 bit RGBA; 16-128 için ek olarak 8 bit paletli.\n";
}

int RunCli(const std::vector<std::string>& args, Report& rep) {
    std::vector<Job> jobs;
    std::string outDirArg;
    bool haveOut = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help" || a == "/?" || a == "-?") {
            rep.info.push_back(Usage());
            return 0;
        }
        if (a == "-p" || a == "-s" || a == "-o") {
            if (i + 1 >= args.size()) {
                rep.err.push_back("'" + a + "' seçeneği için değer verilmedi.");
                return 1;
            }
            const std::string& v = args[++i];
            if (a == "-o") {
                if (haveOut) {
                    rep.err.push_back("-o seçeneği birden fazla kez verildi.");
                    return 1;
                }
                haveOut = true;
                outDirArg = v;
            } else {
                jobs.push_back({PathFromU8(v), a == "-p" ? Kind::Png : Kind::Svg});
            }
        } else if (a.size() > 1 && a[0] == '-') {
            rep.err.push_back("Bilinmeyen seçenek: '" + a + "'. Yardım için: icon -h");
            return 1;
        } else {
            jobs.push_back({PathFromU8(a), Kind::Auto});
        }
    }
    if (jobs.empty()) {
        rep.err.push_back("Girdi dosyası belirtilmedi. Yardım için: icon -h");
        return 1;
    }

    fs::path outDir;
    if (haveOut) {
        outDir = PathFromU8(outDirArg);
        std::error_code ec;
        if (fs::exists(outDir, ec) && !fs::is_directory(outDir, ec)) {
            rep.err.push_back("Çıktı yolu bir klasör değil: '" + outDirArg + "'.");
            return 1;
        }
        if (!fs::exists(outDir, ec)) {
            fs::create_directories(outDir, ec);
            if (ec) {
                rep.err.push_back("Çıktı klasörü oluşturulamadı: '" + outDirArg + "' (" + ec.message() + ").");
                return 1;
            }
        }
    }

    int failed = 0;
    std::set<std::string> used;
    for (auto& job : jobs) {
        fs::path dir = haveOut ? outDir : job.in.parent_path();
        if (dir.empty()) dir = ".";
        fs::path stem = job.in.stem();
        if (stem.empty()) {
            rep.err.push_back("'" + U8(job.in) + "': geçerli bir dosya adı değil.");
            failed++;
            continue;
        }
        fs::path outPath = dir / (stem.native() + fs::path(".ico").native());
        std::string key = Abs(outPath);
#ifdef _WIN32
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)std::tolower(c); });
#endif
        if (!used.insert(key).second) {
            rep.err.push_back("'" + U8(job.in.filename()) + "': aynı çıktı adı (" + U8(outPath.filename()) +
                              ") bu işlemde zaten kullanıldı; atlandı.");
            failed++;
            continue;
        }
        if (!ConvertOne(job, outPath, rep)) failed++;
    }
    return failed ? 1 : 0;
}

// ----------------------------------------------------------------------------
// Kabuk: Windows (arayuzsuz GUI alt sistemi) / test icin Linux main
// ----------------------------------------------------------------------------

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

static std::wstring ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

static std::string ToUtf8(const wchar_t* w) {
    int len = (int)wcslen(w);
    if (!len) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, len, nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, len, &s[0], n, nullptr, nullptr);
    return s;
}

static std::wstring Crlf(const std::wstring& in) {
    std::wstring o;
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == L'\n' && (i == 0 || in[i - 1] != L'\r')) o += L'\r';
        o += in[i];
    }
    return o;
}

// Cikti yonlendirilmisse (icon ... > log.txt veya boru) dogrudan oraya UTF-8 yazar.
// Degilse ust surec bir konsolsa (cmd, PowerShell) ona baglanip yazar. GUI alt sistemli
// programi baslatan cmd beklemeden istemi geri getirdigi icin, konsola yazdiktan sonra
// bir Enter gondererek istemin yeniden cizilmesini saglariz.
static bool StdoutRedirected(HANDLE& h) {
    h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return false;
    DWORD t = GetFileType(h);
    return t == FILE_TYPE_DISK || t == FILE_TYPE_PIPE;
}

static void ConsolePrint(const std::string& utf8) {
    HANDLE so = nullptr;
    if (StdoutRedirected(so)) {
        DWORD n = 0;
        WriteFile(so, utf8.data(), (DWORD)utf8.size(), &n, nullptr);
        return;
    }
    HANDLE out = CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_WRITE | FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0,
                             nullptr);
    if (out == INVALID_HANDLE_VALUE) return;
    std::wstring t = Crlf(L"\n" + ToWide(utf8));
    DWORD n = 0;
    WriteConsoleW(out, t.c_str(), (DWORD)t.size(), &n, nullptr);
    CloseHandle(out);

    HANDLE in = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            OPEN_EXISTING, 0, nullptr);
    if (in != INVALID_HANDLE_VALUE) {
        INPUT_RECORD r[2] = {};
        for (int i = 0; i < 2; i++) {
            r[i].EventType = KEY_EVENT;
            r[i].Event.KeyEvent.bKeyDown = (i == 0);
            r[i].Event.KeyEvent.wRepeatCount = 1;
            r[i].Event.KeyEvent.wVirtualKeyCode = VK_RETURN;
            r[i].Event.KeyEvent.wVirtualScanCode = 0x1C;
            r[i].Event.KeyEvent.uChar.UnicodeChar = L'\r';
        }
        DWORD w = 0;
        WriteConsoleInputW(in, r, 2, &w);
        CloseHandle(in);
    }
}

static void ShowBox(const std::wstring& text, UINT icon) {
    MessageBoxW(nullptr, text.c_str(), L"icon", MB_OK | icon | MB_SETFOREGROUND);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    int argc = 0;
    LPWSTR* argvw = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argvw) {
        for (int i = 1; i < argc; i++) args.push_back(ToUtf8(argvw[i]));
        LocalFree(argvw);
    }
    HANDLE redirectProbe = nullptr;
    const bool console = (AttachConsole(ATTACH_PARENT_PROCESS) != 0) || StdoutRedirected(redirectProbe);

    if (args.empty()) {
        if (console) {
            ConsolePrint(Usage());
            return 0;
        }
        ShowBox(L"Bu program çift tıklayarak çalışmaz.\n\n"
                L"Komut satırından (cmd) kullanın:\n"
                L"  icon -p \"png dosyası\" -o \"çıktı klasörü\"\n"
                L"  icon -s \"svg dosyası\" -o \"çıktı klasörü\"\n\n"
                L"veya bir PNG / SVG dosyasını icon.exe üzerine sürükleyip bırakın.",
                MB_ICONINFORMATION);
        return 1;
    }

    Report rep;
    int code = RunCli(args, rep);

    if (console) {
        std::string all;
        for (auto& s : rep.err) all += "Hata: " + s + "\n";
        for (auto& s : rep.warn) all += "Uyarı: " + s + "\n";
        for (auto& s : rep.info) all += s + (s.empty() || s.back() != '\n' ? "\n" : "");
        if (!all.empty()) ConsolePrint(all);
    } else if (!rep.err.empty() || !rep.warn.empty()) {
        std::string msg;
        if (!rep.err.empty()) {
            msg += "Hata:\n";
            for (auto& s : rep.err) msg += "  - " + s + "\n";
        }
        if (!rep.warn.empty()) {
            if (!msg.empty()) msg += "\n";
            msg += "Uyarı:\n";
            for (auto& s : rep.warn) msg += "  - " + s + "\n";
        }
        ShowBox(ToWide(msg), rep.err.empty() ? MB_ICONWARNING : MB_ICONERROR);
    }
    return code;
}

#else  // Linux/macOS: yalnizca gelistirme sirasinda test icin

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        fputs(Usage().c_str(), stdout);
        return 1;
    }
    Report rep;
    int code = RunCli(args, rep);
    for (auto& s : rep.err) fprintf(stderr, "Hata: %s\n", s.c_str());
    for (auto& s : rep.warn) fprintf(stderr, "Uyari: %s\n", s.c_str());
    for (auto& s : rep.info) fprintf(stdout, "%s\n", s.c_str());
    return code;
}

#endif
