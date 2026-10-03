#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define GLFW_EXPOSE_NATIVE_WIN32
#include <windows.h>
#include <GL/gl.h>
#include <shellapi.h>
#include <shobjidl.h>

#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <numeric>
#include <string>
#include <vector>

#include "cuda_check.h"
#include "landmask.h"
#include "tripmap.h"

namespace {

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

std::string formatDay(unsigned t) {
    if (!t) return "undated";
    std::time_t tt = t;
    std::tm tm{};
    if (gmtime_s(&tm, &tt) != 0) return "undated";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
    return buf;
}

std::string formatSpan(unsigned a, unsigned b) {
    if (!a && !b) return "undated";
    if (!a) a = b;
    if (!b) b = a;
    std::string da = formatDay(a);
    std::string db = formatDay(b);
    if (da == db) return da;
    return da + "  to  " + db;
}

std::string shortPath(const std::string& path) {
    auto slash = path.find_last_of("/\\");
    if (slash == std::string::npos) return path;
    auto prev = path.find_last_of("/\\", slash == 0 ? 0 : slash - 1);
    if (prev == std::string::npos) return path.substr(slash + 1);
    return path.substr(prev + 1);
}

bool fileExists(const std::string& utf8) {
    std::error_code ec;
    return std::filesystem::is_regular_file(std::filesystem::path(utf8ToWide(utf8)), ec);
}

void openPath(const std::string& utf8) {
    std::wstring w = utf8ToWide(utf8);
    if (w.empty()) return;
    ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void revealPath(const std::string& utf8) {
    std::wstring w = utf8ToWide(utf8);
    if (w.empty()) return;
    std::wstring args = L"/select,\"" + w + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

std::wstring pickFolder(GLFWwindow* window) {
    IFileOpenDialog* dlg = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg))))
        return {};
    DWORD opt = 0;
    dlg->GetOptions(&opt);
    dlg->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dlg->SetTitle(L"Folder of pictures");
    std::wstring result;
    if (SUCCEEDED(dlg->Show(glfwGetWin32Window(window)))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dlg->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dlg->Release();
    return result;
}

struct App {
    MapCamera cam;
    float uiScale = 1.f;
    float cellDeg = 0.5f;
    int gapHours = 48;
    int selectedCluster = -1;
    int selectedPhoto = -1;
    bool hoverValid = false;
    float hoverLon = 0.f;
    float hoverLat = 0.f;
    std::vector<int> listOrder;
    std::vector<int> clusterOfPhoto;
    int fbW = 1;
    int fbH = 1;
};

void clampCamera(MapCamera& cam, int w, int h) {
    float minZoom = 0.9f * std::min(w / 360.f, h / 180.f);
    cam.zoom = std::clamp(cam.zoom, std::max(minZoom, 0.4f), 20000.f);
    float halfLon = 0.5f * w / cam.zoom;
    float halfLat = 0.5f * h / cam.zoom;
    if (halfLon < 180.f) cam.lon = std::clamp(cam.lon, -180.f + halfLon, 180.f - halfLon);
    else cam.lon = 0.f;
    if (halfLat < 90.f) cam.lat = std::clamp(cam.lat, -90.f + halfLat, 90.f - halfLat);
    else cam.lat = 0.f;
}

void fitWorld(MapCamera& cam, int w, int h) {
    cam.lon = 0.f;
    cam.lat = 0.f;
    cam.zoom = 0.92f * std::min(w / 360.f, h / 180.f);
    clampCamera(cam, w, h);
}

void fitPhotos(MapCamera& cam, const std::vector<PhotoRec>& photos, int w, int h) {
    if (photos.empty()) {
        fitWorld(cam, w, h);
        return;
    }
    float minLon = 180.f, maxLon = -180.f, minLat = 90.f, maxLat = -90.f;
    for (const PhotoRec& p : photos) {
        minLon = std::min(minLon, p.lon);
        maxLon = std::max(maxLon, p.lon);
        minLat = std::min(minLat, p.lat);
        maxLat = std::max(maxLat, p.lat);
    }
    if (maxLon - minLon > 180.f || maxLat - minLat > 120.f) {
        fitWorld(cam, w, h);
        return;
    }
    cam.lon = 0.5f * (minLon + maxLon);
    cam.lat = 0.5f * (minLat + maxLat);
    float spanLon = std::max((maxLon - minLon) * 1.35f, 0.4f);
    float spanLat = std::max((maxLat - minLat) * 1.35f, 0.4f);
    cam.zoom = std::min(w / spanLon, h / spanLat);
    clampCamera(cam, w, h);
}

void frameCluster(MapCamera& cam, const ClusterInfo& c, float cellDeg, int w, int h) {
    cam.lon = c.lon;
    cam.lat = c.lat;
    float span = std::max(cellDeg * 8.f, 0.35f);
    cam.zoom = 0.72f * std::min(w, h) / span;
    clampCamera(cam, w, h);
}

void rebuildIndex(App& app, const TripMap& map, int photoCount) {
    const auto& clusters = map.clusters();
    app.listOrder.resize(clusters.size());
    std::iota(app.listOrder.begin(), app.listOrder.end(), 0);
    std::sort(app.listOrder.begin(), app.listOrder.end(), [&](int a, int b) {
        unsigned ta = clusters[a].tMax ? clusters[a].tMax : 0;
        unsigned tb = clusters[b].tMax ? clusters[b].tMax : 0;
        if ((ta == 0) != (tb == 0)) return ta != 0;  // dated visits first
        if (ta != tb) return ta > tb;
        return a < b;
    });
    app.clusterOfPhoto.assign(photoCount, -1);
    const auto& order = map.order();
    for (int ci = 0; ci < int(clusters.size()); ++ci) {
        const ClusterInfo& c = clusters[ci];
        for (int k = 0; k < c.count; ++k) {
            int p = order[c.begin + k];
            if (p >= 0 && p < photoCount) app.clusterOfPhoto[p] = ci;
        }
    }
    app.selectedCluster = -1;
    app.selectedPhoto = -1;
}

void screenToMap(const App& app, float mouseX, float mouseY, float scaleX, float scaleY, float& lon, float& lat) {
    float mx = mouseX * scaleX - 0.5f * app.fbW;
    float my = mouseY * scaleY - 0.5f * app.fbH;
    lon = app.cam.lon + mx / app.cam.zoom;
    lat = app.cam.lat - my / app.cam.zoom;
}

void handleMouse(App& app, const std::vector<PhotoRec>& photos) {
    ImGuiIO& io = ImGui::GetIO();
    app.hoverValid = false;
    if (!std::isfinite(io.MousePos.x) || !std::isfinite(io.MousePos.y)) return;
    float sx = io.DisplayFramebufferScale.x;
    float sy = io.DisplayFramebufferScale.y;
    float lon = 0, lat = 0;
    screenToMap(app, io.MousePos.x, io.MousePos.y, sx, sy, lon, lat);
    if (!io.WantCaptureMouse && lon >= -180.f && lon <= 180.f && lat >= -90.f && lat <= 90.f) {
        app.hoverValid = true;
        app.hoverLon = lon;
        app.hoverLat = lat;
    }
    if (io.WantCaptureMouse) return;

    float mx = io.MousePos.x * sx - 0.5f * app.fbW;
    float my = io.MousePos.y * sy - 0.5f * app.fbH;
    if (ImGui::IsMouseDragging(ImGuiMouseButton_Left) || ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        app.cam.lon -= io.MouseDelta.x * sx / app.cam.zoom;
        app.cam.lat += io.MouseDelta.y * sy / app.cam.zoom;
    }
    if (io.MouseWheel != 0.f) {
        float lonUnder = app.cam.lon + mx / app.cam.zoom;
        float latUnder = app.cam.lat - my / app.cam.zoom;
        app.cam.zoom *= std::pow(1.12f, io.MouseWheel);
        app.cam.lon = lonUnder - mx / app.cam.zoom;
        app.cam.lat = latUnder + my / app.cam.zoom;
    }
    clampCamera(app.cam, app.fbW, app.fbH);

    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || io.MouseDragMaxDistanceSqr[0] >= 16.f) return;

    auto pick = [&](float plon, float plat) {
        float dx = (plon - lon) * app.cam.zoom;
        float dy = (plat - lat) * app.cam.zoom;
        return dx * dx + dy * dy;
    };

    app.selectedPhoto = -1;
    if (app.cam.zoom >= 40.f) {
        float best = 12.f * 12.f * app.uiScale * app.uiScale;
        int found = -1;
        for (int i = 0; i < int(photos.size()); ++i) {
            float d = pick(photos[i].lon, photos[i].lat);
            if (d < best) {
                best = d;
                found = i;
            }
        }
        if (found >= 0) {
            app.selectedPhoto = found;
            app.selectedCluster = found < int(app.clusterOfPhoto.size()) ? app.clusterOfPhoto[found] : -1;
        }
    }
}

// The visit pick is separate so it can see the cluster list without dragging the whole map type in.
void pickVisit(App& app, const TripMap& map) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureMouse) return;
    if (!ImGui::IsMouseReleased(ImGuiMouseButton_Left) || io.MouseDragMaxDistanceSqr[0] >= 16.f) return;
    if (app.selectedPhoto >= 0) return;  // a photo click already chose the visit

    float sx = io.DisplayFramebufferScale.x;
    float sy = io.DisplayFramebufferScale.y;
    float lon = 0, lat = 0;
    screenToMap(app, io.MousePos.x, io.MousePos.y, sx, sy, lon, lat);
    float best = 1.0e30f;
    int found = -1;
    const auto& clusters = map.clusters();
    for (int i = 0; i < int(clusters.size()); ++i) {
        float dx = (clusters[i].lon - lon) * app.cam.zoom;
        float dy = (clusters[i].lat - lat) * app.cam.zoom;
        float d = dx * dx + dy * dy;
        // The drawn disc grows with the photo count, so a fat stop is easier to hit.
        float rad = std::min(std::max(5.f, 2.2f * std::log2(float(clusters[i].count) + 1.f)), 22.f) + 4.f;
        if (d <= rad * rad && d < best) {
            best = d;
            found = i;
        }
    }
    app.selectedCluster = found;
    app.selectedPhoto = -1;
}

void drawPanel(App& app, TripMap& map, FolderScan& scan, std::vector<PhotoRec>& photos, GLFWwindow* window) {
    const float s = app.uiScale;
    ImGui::SetNextWindowPos(ImVec2(12.f * s, 12.f * s), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(420.f * s, 640.f * s), ImGuiCond_FirstUseEver);
    ImGui::Begin("Trip Map");

    if (ImGui::Button("Open folder")) {
        std::wstring folder = pickFolder(window);
        if (!folder.empty()) scan.start(std::move(folder));
    }
    ImGui::SameLine();
    if (ImGui::Button("Load demo")) {
        scan.stop();
        photos = makeDemoPhotos();
        map.setPhotos(photos);
        map.cluster(app.cellDeg, unsigned(app.gapHours) * 3600u);
        rebuildIndex(app, map, int(photos.size()));
        fitPhotos(app.cam, photos, app.fbW, app.fbH);
    }
    ImGui::SameLine();
    if (ImGui::Button("Fit")) fitPhotos(app.cam, photos, app.fbW, app.fbH);

    std::string folder = scan.folder();
    if (!folder.empty()) ImGui::TextWrapped("%s", folder.c_str());
    if (scan.running())
        ImGui::Text("Looking through pictures...  %d files, %d with a location", scan.seen(), scan.withGps());
    std::string err = scan.error();
    if (!err.empty()) ImGui::TextColored(ImVec4(1.f, 0.45f, 0.4f, 1.f), "%s", err.c_str());
    if (scan.capped())
        ImGui::TextWrapped("Stopped early: the folder is larger than this view holds (400,000 located pictures).");

    ImGui::Separator();
    ImGui::Text("%d pictures with a location", int(photos.size()));
    ImGui::Text("%d stops    clustered in %.2f ms", int(map.clusters().size()), map.clusterMilliseconds());
    ImGui::Text("Drag to move, scroll to zoom, click a stop.");

    bool changed = false;
    changed |= ImGui::SliderFloat("Cell size", &app.cellDeg, 0.05f, 2.f, "%.2f deg");
    ImGui::Text("About %.0f km across. Photos in one cell can share a stop.", app.cellDeg * 111.f);
    changed |= ImGui::SliderInt("Split after", &app.gapHours, 6, 24 * 14, "%d hours");
    if (changed && !photos.empty()) {
        map.cluster(app.cellDeg, unsigned(app.gapHours) * 3600u);
        rebuildIndex(app, map, int(photos.size()));
    }

    if (app.hoverValid) ImGui::Text("Cursor  %.3f, %.3f", app.hoverLat, app.hoverLon);
    else ImGui::TextUnformatted("Cursor  -");

    ImGui::Separator();
    const auto& clusters = map.clusters();
    float avail = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("stops", ImVec2(0, std::max(80.f, avail * 0.42f)), ImGuiChildFlags_Borders);
    for (int slot = 0; slot < int(app.listOrder.size()); ++slot) {
        int ci = app.listOrder[slot];
        const ClusterInfo& c = clusters[ci];
        char row[160];
        std::snprintf(row, sizeof(row), "%s    %d %s", formatSpan(c.tMin, c.tMax).c_str(), c.count,
                      c.count == 1 ? "photo" : "photos");
        ImGui::PushID(ci);
        if (ImGui::Selectable(row, ci == app.selectedCluster)) {
            app.selectedCluster = ci;
            app.selectedPhoto = -1;
            frameCluster(app.cam, c, app.cellDeg, app.fbW, app.fbH);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%.4f, %.4f", c.lat, c.lon);
        ImGui::PopID();
    }
    ImGui::EndChild();

    ImGui::BeginChild("photos", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (app.selectedCluster >= 0 && app.selectedCluster < int(clusters.size())) {
        const ClusterInfo& c = clusters[app.selectedCluster];
        const auto& order = map.order();
        for (int k = 0; k < c.count; ++k) {
            int pi = order[c.begin + k];
            if (pi < 0 || pi >= int(photos.size())) continue;
            const PhotoRec& rec = photos[pi];
            ImGui::PushID(pi);
            bool on = pi == app.selectedPhoto;
            if (ImGui::Selectable(shortPath(rec.path).c_str(), on)) app.selectedPhoto = pi;
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && fileExists(rec.path))
                openPath(rec.path);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", rec.path.c_str());
            ImGui::PopID();
        }
        ImGui::Separator();
        const PhotoRec* shown = nullptr;
        if (app.selectedPhoto >= 0 && app.selectedPhoto < int(photos.size())) shown = &photos[app.selectedPhoto];
        else if (c.count > 0) {
            int pi = order[c.begin];
            if (pi >= 0 && pi < int(photos.size())) shown = &photos[pi];
        }
        if (shown && fileExists(shown->path)) {
            if (ImGui::Button("Open")) openPath(shown->path);
            ImGui::SameLine();
            if (ImGui::Button("Show in Explorer")) revealPath(shown->path);
        } else if (shown) {
            ImGui::TextWrapped("This entry is not a file on disk.");
        }
    } else {
        ImGui::TextWrapped("Click a stop to list its pictures. Double-click a file to open it.");
    }
    ImGui::EndChild();

    ImGui::End();
}

void drawLabel(const App& app, const TripMap& map) {
    if (app.selectedCluster < 0 || app.selectedCluster >= int(map.clusters().size())) return;
    const ClusterInfo& c = map.clusters()[app.selectedCluster];
    ImGuiIO& io = ImGui::GetIO();
    float sx = io.DisplayFramebufferScale.x;
    float sy = io.DisplayFramebufferScale.y;
    float px = ((c.lon - app.cam.lon) * app.cam.zoom + 0.5f * app.fbW) / sx;
    float py = ((app.cam.lat - c.lat) * app.cam.zoom + 0.5f * app.fbH) / sy;
    if (px < -40.f || py < -40.f || px > io.DisplaySize.x + 40.f || py > io.DisplaySize.y + 40.f) return;
    std::string text = formatSpan(c.tMin, c.tMax) + "   " + std::to_string(c.count);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 at(px + 14.f, py - 8.f);
    dl->AddText(ImVec2(at.x + 1.f, at.y + 1.f), IM_COL32(0, 0, 0, 220), text.c_str());
    dl->AddText(at, IM_COL32(255, 244, 220, 240), text.c_str());
}

bool landAt(float lon, float lat) {
    int x = int((lon + 180.f) / 360.f * kLandW);
    int y = int((90.f - lat) / 180.f * kLandH);
    x = std::clamp(x, 0, kLandW - 1);
    y = std::clamp(y, 0, kLandH - 1);
    int i = y * kLandW + x;
    return (kLandMask[i >> 3] >> (7 - (i & 7))) & 1;
}

bool landSelfTest() {
    struct Sample {
        float lat, lon;
        bool land;
        const char* name;
    };
    const Sample samples[] = {
        {48.86f, 2.35f, true, "Paris"},
        {40.75f, -73.99f, true, "New York"},
        {-33.87f, 151.21f, true, "Sydney"},
        {0.f, -30.f, false, "Atlantic"},
        {0.f, -160.f, false, "Pacific"},
    };
    for (const Sample& s : samples) {
        if (landAt(s.lon, s.lat) != s.land) {
            std::fprintf(stderr, "land self-test: %s (%.2f, %.2f) expected %s\n", s.name, s.lat, s.lon,
                         s.land ? "land" : "water");
            return false;
        }
    }
    return true;
}

bool clusterSelfTest() {
    std::vector<PhotoRec> photos;
    auto add = [&](float lat, float lon, unsigned t0, int n, unsigned step) {
        for (int i = 0; i < n; ++i) {
            PhotoRec r;
            r.lat = lat;
            r.lon = lon;
            r.time = t0 + unsigned(i) * step;
            r.path = "self-test";
            photos.push_back(r);
        }
    };
    const unsigned base = 1600000000u;
    add(10.5f, 20.5f, base, 8, 3600u);
    add(10.5f, 20.5f, base + 100u * 86400u, 8, 3600u);
    add(-33.5f, 150.5f, base, 5, 1800u);

    TripMap map;
    map.setPhotos(photos);
    map.cluster(1.f, 48u * 3600u);
    const auto& clusters = map.clusters();
    auto matches = [&](float lat, float lon, int count, unsigned t0) {
        for (const ClusterInfo& c : clusters) {
            if (c.count == count && std::fabs(c.lat - lat) < 0.01f && std::fabs(c.lon - lon) < 0.01f && c.tMin >= t0 &&
                c.tMin < t0 + 86400u)
                return true;
        }
        return false;
    };
    bool ok = clusters.size() == 3 && matches(10.5f, 20.5f, 8, base) &&
              matches(10.5f, 20.5f, 8, base + 100u * 86400u) && matches(-33.5f, 150.5f, 5, base);
    if (!ok) {
        std::fprintf(stderr, "cluster self-test: expected 3 visits, got %d\n", int(clusters.size()));
        for (const ClusterInfo& c : clusters)
            std::fprintf(stderr, "  n=%d  %.3f, %.3f  t=%u..%u\n", c.count, c.lat, c.lon, c.tMin, c.tMax);
        return false;
    }
    std::printf("cluster self-test: 3 visits in %.2f ms\n", map.clusterMilliseconds());
    return true;
}

int runSelfTest() {
    if (!exifSelfTest()) return 1;
    std::printf("exif self-test: 48.5 N, 12.25 W\n");
    if (!landSelfTest()) return 1;
    std::printf("land self-test: coastlines match\n");
    if (!clusterSelfTest()) return 1;
    std::printf("self-test ok\n");
    return 0;
}

void usage() {
    std::fprintf(stderr, "tripmap [--demo] [folder]\n");
    std::fprintf(stderr, "tripmap --self-test\n");
}

}  // namespace

int main() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool demo = false;
    bool selfTest = false;
    std::wstring folder;
    for (int i = 1; i < argc; ++i) {
        if (std::wcscmp(argv[i], L"--self-test") == 0) selfTest = true;
        else if (std::wcscmp(argv[i], L"--demo") == 0) demo = true;
        else if (std::wcscmp(argv[i], L"--help") == 0 || std::wcscmp(argv[i], L"-h") == 0) {
            usage();
            LocalFree(argv);
            return 0;
        } else if (argv[i][0] != L'-') {
            folder = argv[i];
        } else {
            std::fwprintf(stderr, L"Unknown option %s\n", argv[i]);
            usage();
            LocalFree(argv);
            return 2;
        }
    }
    LocalFree(argv);
    if (selfTest) return runSelfTest();

    HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ownCom = com == S_OK;

    if (!glfwInit()) {
        std::fprintf(stderr, "Failed to initialise GLFW\n");
        if (ownCom) CoUninitialize();
        return 1;
    }
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_FALSE);
    int workX = 0, workY = 0, workW = 1600, workH = 1000;
    glfwGetMonitorWorkarea(glfwGetPrimaryMonitor(), &workX, &workY, &workW, &workH);
    GLFWwindow* window = glfwCreateWindow(workW * 85 / 100, workH * 85 / 100, "Trip Map", nullptr, nullptr);
    if (!window) {
        std::fprintf(stderr, "Failed to create window\n");
        glfwTerminate();
        if (ownCom) CoUninitialize();
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    float contentScale = 1.f, unused = 1.f;
    glfwGetWindowContentScale(window, &contentScale, &unused);
    contentScale = std::max(1.f, contentScale);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::StyleColorsDark();
    ImGui::GetStyle().ScaleAllSizes(contentScale);
    ImFontConfig fontCfg;
    fontCfg.SizePixels = std::round(16.f * contentScale);
    if (!ImGui::GetIO().Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\segoeui.ttf", std::round(16.f * contentScale)))
        ImGui::GetIO().Fonts->AddFontDefault(&fontCfg);
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    cudaDeviceProp prop{};
    CUDA_CHECK(cudaGetDeviceProperties(&prop, 0));
    std::printf("GPU: %s (sm_%d%d)\n", prop.name, prop.major, prop.minor);

    {
        TripMap map;
        FolderScan scan;
        std::vector<PhotoRec> photos;
        App app;
        app.uiScale = contentScale;
        glfwGetFramebufferSize(window, &app.fbW, &app.fbH);
        fitWorld(app.cam, app.fbW, app.fbH);

        if (!folder.empty()) scan.start(std::move(folder));
        else if (demo) {
            photos = makeDemoPhotos();
            map.setPhotos(photos);
            map.cluster(app.cellDeg, unsigned(app.gapHours) * 3600u);
            rebuildIndex(app, map, int(photos.size()));
            fitPhotos(app.cam, photos, app.fbW, app.fbH);
        }

        while (!glfwWindowShouldClose(window)) {
            glfwPollEvents();
            glfwGetFramebufferSize(window, &app.fbW, &app.fbH);
            if (app.fbW == 0 || app.fbH == 0) {
                glfwWaitEvents();
                continue;
            }

            if (scan.take(photos)) {
                map.setPhotos(photos);
                map.cluster(app.cellDeg, unsigned(app.gapHours) * 3600u);
                rebuildIndex(app, map, int(photos.size()));
                fitPhotos(app.cam, photos, app.fbW, app.fbH);
                std::printf("Loaded %d pictures with a location, %d stops\n", int(photos.size()),
                            int(map.clusters().size()));
            }

            ImGui_ImplOpenGL3_NewFrame();
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            ImGuiIO& io = ImGui::GetIO();

            if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F))
                fitPhotos(app.cam, photos, app.fbW, app.fbH);
            if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                app.selectedCluster = -1;
                app.selectedPhoto = -1;
            }

            handleMouse(app, photos);
            pickVisit(app, map);
            // The panel can change the selection and the camera; draw the map after it so both show
            // up on this frame.
            drawPanel(app, map, scan, photos, window);
            map.render(app.fbW, app.fbH, app.cam, app.selectedCluster, app.selectedPhoto);

            if (map.glTexture())
                ImGui::GetBackgroundDrawList()->AddImage((ImTextureID)(intptr_t)map.glTexture(), ImVec2(0, 0),
                                                         io.DisplaySize);
            drawLabel(app, map);

            ImGui::Render();
            glViewport(0, 0, app.fbW, app.fbH);
            glClearColor(0.03f, 0.035f, 0.045f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT);
            ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
            glfwSwapBuffers(window);
        }
        scan.stop();
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
    if (ownCom) CoUninitialize();
    return 0;
}
