#define NOMINMAX
#include "photos.h"

#include <windows.h>
#include <ole2.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shobjidl.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>

#pragma comment(lib, "propsys.lib")
#pragma comment(lib, "ole32.lib")

namespace {

constexpr int kMaxPhotos = 400000;
constexpr int kMaxFiles = 2000000;

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring w(static_cast<std::size_t>(n - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<std::size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

bool plausible(float lat, float lon) {
    if (!(lat == lat) || !(lon == lon)) return false;  // NaN
    if (lat < -90.f || lat > 90.f || lon < -180.f || lon > 180.f) return false;
    // Phone software writes exactly 0,0 when the fix is missing ("Null Island").
    if (lat == 0.f && lon == 0.f) return false;
    return true;
}

std::uint32_t ymdToUnix(int year, int month, int day, int hour, int min, int sec) {
    if (year < 1970 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) return 0;
    if (hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 60) return 0;
    std::tm tm{};
    tm.tm_year = year - 1900;
    tm.tm_mon = month - 1;
    tm.tm_mday = day;
    tm.tm_hour = hour;
    tm.tm_min = min;
    tm.tm_sec = sec;
    std::time_t u = _mkgmtime(&tm);
    if (u <= 0) return 0;
    return static_cast<std::uint32_t>(u);
}

// EXIF dates have no timezone. They are stored as UTC so two files agree with each other;
// the clock on the wall may differ by the camera's offset.
bool parseExifDate(const char* text, std::uint32_t& time) {
    int year = 0, month = 0, day = 0, hour = 0, min = 0, sec = 0;
    const char* s = text;
    auto read = [&](int& out) {
        if (*s < '0' || *s > '9') return false;
        int v = 0;
        while (*s >= '0' && *s <= '9') {
            v = v * 10 + (*s - '0');
            ++s;
        }
        out = v;
        return true;
    };
    auto eat = [&](char c) {
        if (*s != c) return false;
        ++s;
        return true;
    };
    if (!read(year) || !eat(':') || !read(month) || !eat(':') || !read(day) || !eat(' ')) return false;
    if (!read(hour) || !eat(':') || !read(min) || !eat(':') || !read(sec)) return false;
    std::uint32_t u = ymdToUnix(year, month, day, hour, min, sec);
    if (!u) return false;
    time = u;
    return true;
}

struct Tiff {
    const std::uint8_t* p = nullptr;
    std::size_t n = 0;
    bool le = true;
    bool ok = true;

    std::uint16_t u16(std::size_t o) {
        if (o + 2 > n) {
            ok = false;
            return 0;
        }
        if (le) return std::uint16_t(p[o] | (p[o + 1] << 8));
        return std::uint16_t((p[o] << 8) | p[o + 1]);
    }
    std::uint32_t u32(std::size_t o) {
        if (o + 4 > n) {
            ok = false;
            return 0;
        }
        if (le)
            return std::uint32_t(p[o] | (p[o + 1] << 8) | (p[o + 2] << 16) | (p[o + 3] << 24));
        return std::uint32_t((p[o] << 24) | (p[o + 1] << 16) | (p[o + 2] << 8) | p[o + 3]);
    }
};

int typeSize(int type) {
    switch (type) {
        case 1: return 1;  // BYTE
        case 2: return 1;  // ASCII
        case 3: return 2;  // SHORT
        case 4: return 4;  // LONG
        case 5: return 8;  // RATIONAL
        default: return 0;
    }
}

// Value bytes live in the entry when they fit in four bytes; otherwise the entry holds an offset
// from the start of the TIFF header.
std::size_t valueAt(Tiff& t, std::size_t entry, int type, std::uint32_t count) {
    int sz = typeSize(type);
    if (sz <= 0) {
        t.ok = false;
        return 0;
    }
    if (sz * static_cast<std::size_t>(count) <= 4) return entry + 8;
    return t.u32(entry + 8);
}

double rational(Tiff& t, std::size_t at) {
    std::uint32_t num = t.u32(at);
    std::uint32_t den = t.u32(at + 4);
    if (!t.ok || den == 0) return 0.0;
    return double(num) / double(den);
}

bool readDateAt(Tiff& t, std::size_t at, std::uint32_t count, std::uint32_t& time) {
    if (count < 19 || at + 19 > t.n) return false;
    char buf[20];
    std::memcpy(buf, t.p + at, 19);
    buf[19] = 0;
    return parseExifDate(buf, time);
}

struct GpsFields {
    bool hasLat = false;
    bool hasLon = false;
    double lat = 0;
    double lon = 0;
    char latRef = 'N';
    char lonRef = 'E';
    std::uint32_t time = 0;
    bool hasTime = false;
};

void readIfd(Tiff& t, std::uint32_t off, GpsFields& g, int which) {
    // which: 0 = IFD0, 1 = Exif, 2 = GPS
    if (!t.ok || off + 2 > t.n) {
        t.ok = false;
        return;
    }
    std::uint16_t count = t.u16(off);
    if (count > 512 || off + 2 + static_cast<std::size_t>(count) * 12 > t.n) {
        t.ok = false;
        return;
    }
    std::uint32_t gpsPtr = 0;
    std::uint32_t exifPtr = 0;
    for (int e = 0; e < count && t.ok; ++e) {
        std::size_t ent = off + 2 + static_cast<std::size_t>(e) * 12;
        std::uint16_t tag = t.u16(ent);
        std::uint16_t type = t.u16(ent + 2);
        std::uint32_t cnt = t.u32(ent + 4);
        // Most IFD entries (maker notes, thumbnails) are irrelevant. Touching their offsets
        // would reject the file, so only the tags below are decoded.
        if (which == 0 && tag == 0x8769 && type == 4 && cnt == 1) {
            exifPtr = t.u32(ent + 8);
        } else if (which == 0 && tag == 0x8825 && type == 4 && cnt == 1) {
            gpsPtr = t.u32(ent + 8);
        } else if (which == 0 && tag == 0x0132) {
            std::uint32_t tm = 0;
            if (readDateAt(t, valueAt(t, ent, type, cnt), cnt, tm)) {
                g.time = tm;
                g.hasTime = true;
            }
        } else if (which == 1 && tag == 0x9003) {
            std::uint32_t tm = 0;
            if (readDateAt(t, valueAt(t, ent, type, cnt), cnt, tm)) {
                g.time = tm;
                g.hasTime = true;
            }
        } else if (which == 2 && (tag == 1 || tag == 3) && typeSize(type) > 0) {
            std::size_t at = valueAt(t, ent, type, cnt);
            if (t.ok && at < t.n) {
                if (tag == 1) g.latRef = char(t.p[at]);
                else g.lonRef = char(t.p[at]);
            }
        } else if (which == 2 && tag == 2 && type == 5 && cnt >= 3) {
            std::size_t at = valueAt(t, ent, type, cnt);
            g.lat = rational(t, at) + rational(t, at + 8) / 60.0 + rational(t, at + 16) / 3600.0;
            g.hasLat = t.ok;
        } else if (which == 2 && tag == 4 && type == 5 && cnt >= 3) {
            std::size_t at = valueAt(t, ent, type, cnt);
            g.lon = rational(t, at) + rational(t, at + 8) / 60.0 + rational(t, at + 16) / 3600.0;
            g.hasLon = t.ok;
        }
    }
    if (which == 0 && t.ok) {
        if (exifPtr) readIfd(t, exifPtr, g, 1);
        if (gpsPtr) readIfd(t, gpsPtr, g, 2);
    }
}

bool parseTiff(const std::uint8_t* data, std::size_t n, float& lat, float& lon, std::uint32_t& time) {
    if (n < 8) return false;
    Tiff t;
    t.p = data;
    t.n = n;
    if (data[0] == 'I' && data[1] == 'I') t.le = true;
    else if (data[0] == 'M' && data[1] == 'M') t.le = false;
    else return false;
    if (t.u16(2) != 42) return false;
    GpsFields g;
    readIfd(t, t.u32(4), g, 0);
    if (!t.ok || !g.hasLat || !g.hasLon) return false;
    if (g.latRef == 'S' || g.latRef == 's') g.lat = -g.lat;
    if (g.lonRef == 'W' || g.lonRef == 'w') g.lon = -g.lon;
    float flat = float(g.lat);
    float flon = float(g.lon);
    if (!plausible(flat, flon)) return false;
    lat = flat;
    lon = flon;
    time = g.hasTime ? g.time : 0;
    return true;
}

// Walk JPEG markers and parse the EXIF APP1 segment. The compressed image itself is skipped.
bool parseJpeg(const std::uint8_t* data, std::size_t n, float& lat, float& lon, std::uint32_t& time) {
    if (n < 4 || data[0] != 0xFF || data[1] != 0xD8) return false;
    std::size_t i = 2;
    while (i + 4 <= n) {
        if (data[i] != 0xFF) return false;
        while (i < n && data[i] == 0xFF) ++i;
        if (i >= n) return false;
        std::uint8_t marker = data[i++];
        if (marker == 0xD9 || marker == 0xDA) return false;             // EOI, or start of entropy data
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        if (i + 2 > n) return false;
        unsigned seglen = (unsigned(data[i]) << 8) | data[i + 1];
        if (seglen < 2 || i + seglen > n) return false;
        if (marker == 0xE1 && seglen >= 8 && std::memcmp(data + i + 2, "Exif\0\0", 6) == 0) {
            return parseTiff(data + i + 8, seglen - 8, lat, lon, time);
        }
        i += seglen;
    }
    return false;
}

bool readJpegFile(FILE* f, float& lat, float& lon, std::uint32_t& time) {
    unsigned char soh[2];
    if (std::fread(soh, 1, 2, f) != 2 || soh[0] != 0xFF || soh[1] != 0xD8) return false;
    while (true) {
        int b = std::fgetc(f);
        if (b == EOF) return false;
        while (b == 0xFF) {
            b = std::fgetc(f);
            if (b == EOF) return false;
        }
        unsigned marker = unsigned(b);
        if (marker == 0xD9 || marker == 0xDA) return false;
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) continue;
        int hi = std::fgetc(f);
        int lo = std::fgetc(f);
        if (hi == EOF || lo == EOF) return false;
        int seglen = (hi << 8) | lo;
        if (seglen < 2) return false;
        int payload = seglen - 2;
        if (marker == 0xE1 && payload >= 6 && payload <= 16 * 1024 * 1024) {
            std::vector<std::uint8_t> seg(static_cast<std::size_t>(payload));
            if (std::fread(seg.data(), 1, seg.size(), f) != seg.size()) return false;
            if (std::memcmp(seg.data(), "Exif\0\0", 6) == 0)
                return parseTiff(seg.data() + 6, seg.size() - 6, lat, lon, time);
            continue;
        }
        if (_fseeki64(f, payload, SEEK_CUR) != 0) return false;
    }
}

std::uint32_t fileTimeToUnix(const FILETIME& ft) {
    ULARGE_INTEGER u;
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    const unsigned long long kEpoch = 11644473600ULL;
    unsigned long long sec = u.QuadPart / 10000000ULL;
    if (sec < kEpoch || sec - kEpoch > 0xFFFFFFFFULL) return 0;
    return static_cast<std::uint32_t>(sec - kEpoch);
}

float propDouble(IPropertyStore* store, REFPROPERTYKEY key, bool& ok) {
    PROPVARIANT var;
    PropVariantInit(&var);
    float v = 0.f;
    ok = false;
    if (SUCCEEDED(store->GetValue(key, &var))) {
        if (var.vt == VT_R8) {
            v = float(var.dblVal);
            ok = true;
        } else if (var.vt == VT_R4) {
            v = var.fltVal;
            ok = true;
        } else if (var.vt == VT_LPWSTR && var.pwszVal) {
            wchar_t* end = nullptr;
            double d = std::wcstod(var.pwszVal, &end);
            if (end != var.pwszVal) {
                v = float(d);
                ok = true;
            }
        }
    }
    PropVariantClear(&var);
    return v;
}

bool readPropertyGps(const std::wstring& path, float& lat, float& lon, std::uint32_t& time) {
    IPropertyStore* store = nullptr;
    HRESULT hr = SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_BESTEFFORT, IID_PPV_ARGS(&store));
    if (FAILED(hr) || !store) return false;
    bool latOk = false, lonOk = false;
    float plat = propDouble(store, PKEY_GPS_Latitude, latOk);
    float plon = propDouble(store, PKEY_GPS_Longitude, lonOk);
    std::uint32_t pt = 0;
    PROPVARIANT var;
    PropVariantInit(&var);
    if (SUCCEEDED(store->GetValue(PKEY_Photo_DateTaken, &var)) && var.vt == VT_FILETIME)
        pt = fileTimeToUnix(var.filetime);
    PropVariantClear(&var);
    store->Release();
    if (!latOk || !lonOk || !plausible(plat, plon)) return false;
    lat = plat;
    lon = plon;
    time = pt;
    return true;
}

bool wantExtension(const std::wstring& ext) {
    return ext == L".jpg" || ext == L".jpeg" || ext == L".jpe" || ext == L".tif" || ext == L".tiff" ||
           ext == L".png" || ext == L".heic" || ext == L".heif" || ext == L".webp" || ext == L".dng" ||
           ext == L".cr2" || ext == L".nef" || ext == L".arw";
}

bool readPhotoFile(const std::filesystem::path& path, float& lat, float& lon, std::uint32_t& time) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char magic[4] = {};
    std::size_t got = std::fread(magic, 1, 4, f);
    bool jpeg = got >= 2 && magic[0] == 0xFF && magic[1] == 0xD8;
    bool tiff = got >= 2 && ((magic[0] == 'I' && magic[1] == 'I') || (magic[0] == 'M' && magic[1] == 'M'));
    bool found = false;
    if (jpeg) {
        std::rewind(f);
        found = readJpegFile(f, lat, lon, time);
    } else if (tiff) {
        // GPS IFD offsets in phone TIFFs and DNGs sit near the front of the file.
        std::vector<std::uint8_t> buf(12 * 1024 * 1024);
        std::rewind(f);
        std::size_t n = std::fread(buf.data(), 1, buf.size(), f);
        found = parseTiff(buf.data(), n, lat, lon, time);
    }
    std::fclose(f);
    if (found) return true;
    if (jpeg || tiff) return false;
    return readPropertyGps(path.wstring(), lat, lon, time);
}

void addVisit(std::vector<PhotoRec>& out, const char* place, float lat, float lon, std::uint32_t start,
              int photos) {
    // Sit on the centre of a 0.5 degree cell (the default slider). A city whose coordinate
    // lands on a cell edge would otherwise split into two stops.
    auto centre = [](float v, float origin) {
        constexpr float cell = 0.5f;
        float k = std::floor((v - origin) / cell);
        return origin + (k + 0.5f) * cell;
    };
    lat = centre(lat, -90.f);
    lon = centre(lon, -180.f);
    for (int i = 0; i < photos; ++i) {
        float jx = ((i * 47) % 100) / 100.f - 0.5f;
        float jy = ((i * 29) % 100) / 100.f - 0.5f;
        PhotoRec rec;
        rec.lat = lat + jy * 0.08f;
        rec.lon = lon + jx * 0.08f;
        rec.time = start + static_cast<std::uint32_t>(i) * 3600u;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "demo/%s/%02d.jpg", place, i + 1);
        rec.path = buf;
        out.push_back(rec);
    }
}

void appendU16(std::vector<std::uint8_t>& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
}
void appendU32(std::vector<std::uint8_t>& b, unsigned v) {
    b.push_back(static_cast<std::uint8_t>(v & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFF));
    b.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFF));
}

}  // namespace

bool parsePhotoBytes(const std::uint8_t* data, std::size_t size, float& lat, float& lon, std::uint32_t& time) {
    if (!data || size < 8) return false;
    if (data[0] == 0xFF && data[1] == 0xD8) return parseJpeg(data, size, lat, lon, time);
    if ((data[0] == 'I' && data[1] == 'I') || (data[0] == 'M' && data[1] == 'M'))
        return parseTiff(data, size, lat, lon, time);
    return false;
}

std::vector<PhotoRec> makeDemoPhotos() {
    std::vector<PhotoRec> out;
    struct Stop {
        const char* name;
        float lat, lon;
        int year, month, day;
        int count;
    };
    // Two visits to a few cities, spread over the globe so the first frame is a world map.
    const Stop stops[] = {
        {"Lisbon-2022-03", 38.72f, -9.14f, 2022, 3, 12, 16},
        {"Lisbon-2024-08", 38.74f, -9.16f, 2024, 8, 3, 22},
        {"Paris-2023-05", 48.86f, 2.35f, 2023, 5, 18, 28},
        {"Rome-2023-05", 41.90f, 12.50f, 2023, 5, 21, 18},
        {"Reykjavik-2021-11", 64.15f, -21.94f, 2021, 11, 2, 12},
        {"NewYork-2022-12", 40.75f, -73.99f, 2022, 12, 28, 20},
        {"MexicoCity-2019-02", 19.43f, -99.13f, 2019, 2, 9, 14},
        {"Rio-2020-01", -22.97f, -43.18f, 2020, 1, 15, 15},
        {"Cairo-2024-01", 30.04f, 31.24f, 2024, 1, 8, 11},
        {"Nairobi-2018-07", -1.29f, 36.82f, 2018, 7, 20, 13},
        {"CapeTown-2023-09", -33.92f, 18.42f, 2023, 9, 4, 17},
        {"Kyoto-2022-04", 35.01f, 135.77f, 2022, 4, 6, 24},
        {"Sydney-2021-01", -33.87f, 151.21f, 2021, 1, 26, 19},
    };
    for (const Stop& s : stops) {
        std::uint32_t t = ymdToUnix(s.year, s.month, s.day, 9, 0, 0);
        addVisit(out, s.name, s.lat, s.lon, t, s.count);
    }
    return out;
}

bool exifSelfTest() {
    // Little-endian TIFF: DateTime plus a GPS IFD for 48 deg 30 min N, 12 deg 15 min W.
    std::vector<std::uint8_t> tiff;
    auto& b = tiff;
    b.insert(b.end(), {'I', 'I', 0x2A, 0x00});
    appendU32(b, 8);  // IFD0
    appendU16(b, 2);  // two entries
    // DateTime 0x0132, ASCII, 20 bytes, at offset 38.
    appendU16(b, 0x0132);
    appendU16(b, 2);
    appendU32(b, 20);
    appendU32(b, 38);
    // GPSInfo 0x8825, LONG, points at offset 58.
    appendU16(b, 0x8825);
    appendU16(b, 4);
    appendU32(b, 1);
    appendU32(b, 58);
    appendU32(b, 0);  // next IFD
    const char date[] = "2020:01:02 03:04:05";
    b.insert(b.end(), date, date + 19);
    b.push_back(0);
    // GPS IFD, 4 entries, at offset 58.
    appendU16(b, 4);
    appendU16(b, 1);
    appendU16(b, 2);
    appendU32(b, 2);
    b.insert(b.end(), {'N', 0, 0, 0});
    appendU16(b, 2);
    appendU16(b, 5);
    appendU32(b, 3);
    appendU32(b, 112);  // latitude rationals
    appendU16(b, 3);
    appendU16(b, 2);
    appendU32(b, 2);
    b.insert(b.end(), {'W', 0, 0, 0});
    appendU16(b, 4);
    appendU16(b, 5);
    appendU32(b, 3);
    appendU32(b, 136);  // longitude rationals
    appendU32(b, 0);
    auto rats = [&](unsigned a, unsigned bnum) {
        appendU32(b, a);
        appendU32(b, 1);
        appendU32(b, bnum);
        appendU32(b, 1);
        appendU32(b, 0);
        appendU32(b, 1);
    };
    rats(48, 30);
    rats(12, 15);

    std::vector<std::uint8_t> jpeg;
    jpeg.insert(jpeg.end(), {0xFF, 0xD8, 0xFF, 0xE1});
    unsigned seglen = static_cast<unsigned>(2 + 6 + tiff.size());
    jpeg.push_back(static_cast<std::uint8_t>(seglen >> 8));
    jpeg.push_back(static_cast<std::uint8_t>(seglen & 0xFF));
    jpeg.insert(jpeg.end(), {'E', 'x', 'i', 'f', 0, 0});
    jpeg.insert(jpeg.end(), tiff.begin(), tiff.end());
    jpeg.insert(jpeg.end(), {0xFF, 0xD9});

    float lat = 0, lon = 0;
    std::uint32_t time = 0;
    if (!parsePhotoBytes(jpeg.data(), jpeg.size(), lat, lon, time)) {
        std::fprintf(stderr, "exif self-test: parser rejected the sample JPEG\n");
        return false;
    }
    std::uint32_t expect = ymdToUnix(2020, 1, 2, 3, 4, 5);
    if (std::fabs(lat - 48.5f) > 1e-3f || std::fabs(lon - (-12.25f)) > 1e-3f || time != expect) {
        std::fprintf(stderr, "exif self-test: got %.5f, %.5f time %u, expected 48.5, -12.25, %u\n", lat, lon,
                     time, expect);
        return false;
    }
    if (!parsePhotoBytes(tiff.data(), tiff.size(), lat, lon, time)) {
        std::fprintf(stderr, "exif self-test: parser rejected the bare TIFF\n");
        return false;
    }
    return true;
}

FolderScan::~FolderScan() { stop(); }

void FolderScan::stop() {
    cancel_.store(true);
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mu_);
    photos_.clear();
    ready_ = false;
    running_.store(false);
}

void FolderScan::start(std::wstring folder) {
    stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        photos_.clear();
        ready_ = false;
        error_.clear();
        folder_ = wideToUtf8(folder);
    }
    cancel_.store(false);
    seen_.store(0);
    withGps_.store(0);
    capped_.store(false);
    running_.store(true);
    thread_ = std::thread([this, folder = std::move(folder)]() { run(folder); });
}

bool FolderScan::take(std::vector<PhotoRec>& photos) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!ready_) return false;
    photos.swap(photos_);
    photos_.clear();
    ready_ = false;
    return true;
}

std::string FolderScan::folder() const {
    std::lock_guard<std::mutex> lock(mu_);
    return folder_;
}

std::string FolderScan::error() const {
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void FolderScan::run(std::wstring folder) {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool ownCom = com == S_OK;
    std::vector<PhotoRec> found;
    std::string error;
    bool capped = false;
    try {
        std::error_code ec;
        auto options = std::filesystem::directory_options::skip_permission_denied;
        std::filesystem::recursive_directory_iterator it(folder, options, ec), end;
        for (; it != end; it.increment(ec)) {
            if (cancel_.load()) break;
            if (ec) {
                ec.clear();
                continue;
            }
            std::error_code fec;
            if (!it->is_regular_file(fec) || fec) continue;
            std::wstring ext = it->path().extension().wstring();
            for (wchar_t& c : ext) c = wchar_t(towlower(c));
            if (!wantExtension(ext)) continue;
            int seen = seen_.fetch_add(1) + 1;
            if (seen > kMaxFiles || int(found.size()) >= kMaxPhotos) {
                capped = true;
                break;
            }
            float lat = 0, lon = 0;
            std::uint32_t time = 0;
            if (!readPhotoFile(it->path(), lat, lon, time)) continue;
            PhotoRec rec;
            rec.lat = lat;
            rec.lon = lon;
            rec.time = time;
            rec.path = wideToUtf8(it->path().wstring());
            found.push_back(rec);
            withGps_.store(int(found.size()));
        }
    } catch (const std::exception& ex) {
        error = ex.what();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!cancel_.load()) {
            photos_.swap(found);
            error_ = std::move(error);
            ready_ = true;
            capped_.store(capped);
        }
        running_.store(false);
    }
    if (ownCom) CoUninitialize();
}
