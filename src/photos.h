#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// One picture that carried a GPS fix. `time` is the EXIF timestamp interpreted as UTC,
// or 0 when the file has a location but no date. `path` is UTF-8.
struct PhotoRec {
    float lat = 0.f;
    float lon = 0.f;
    std::uint32_t time = 0;
    std::string path;
};

// JPEG APP1 EXIF, or a TIFF header. Returns false when the bytes have no GPS fix.
bool parsePhotoBytes(const std::uint8_t* data, std::size_t size, float& lat, float& lon, std::uint32_t& time);

// A few dozen real cities, two visits each, so the map is usable before any folder is opened.
std::vector<PhotoRec> makeDemoPhotos();

// Builds a tiny JPEG with a known GPS fix and checks the parser against it.
bool exifSelfTest();

// Walks a folder on a background thread. JPEG and TIFF are read from the EXIF header only;
// HEIC, PNG, WEBP and camera RAW go through the Windows property handler.
class FolderScan {
public:
    FolderScan() = default;
    ~FolderScan();
    FolderScan(const FolderScan&) = delete;
    FolderScan& operator=(const FolderScan&) = delete;

    void start(std::wstring folder);
    void stop();

    // Hands the finished list to the caller once. Further calls return false until the next scan.
    bool take(std::vector<PhotoRec>& photos);

    bool running() const { return running_.load(); }
    bool capped() const { return capped_.load(); }
    int seen() const { return seen_.load(); }
    int withGps() const { return withGps_.load(); }
    std::string folder() const;
    std::string error() const;

private:
    void run(std::wstring folder);

    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> capped_{false};
    std::atomic<int> seen_{0};
    std::atomic<int> withGps_{0};

    mutable std::mutex mu_;
    std::vector<PhotoRec> photos_;
    std::string folder_;
    std::string error_;
    bool ready_ = false;
};
