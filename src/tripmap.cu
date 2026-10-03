#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>
#include <cuda_gl_interop.h>

#include <cub/device/device_radix_sort.cuh>
#include <cub/device/device_scan.cuh>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "cuda_check.h"
#include "landmask.h"
#include "tripmap.h"

// ---------------------------------------------------------------------------
// One visit = one run of photos that share a map cell and have no large time gap.
//
//   key[i]     = (cell << 32) | unix time          one thread per photo
//   sort keys                             CUB radix sort, so a cell is one contiguous run
//   flag[i]    = 1 when a new visit starts         cell changed, or the gap exceeds the slider
//   scan flags                            exclusive sum, which is the visit id
//   summarise                             one thread per visit: count, centroid, time span
// ---------------------------------------------------------------------------

static_assert(sizeof(ClusterInfo) == 24, "ClusterInfo is copied between host and device");

__device__ __forceinline__ float3 lerp3(float3 a, float3 b, float t) {
    return make_float3(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t);
}

__device__ __forceinline__ float clamp01(float x) { return fminf(fmaxf(x, 0.f), 1.f); }

__device__ __forceinline__ uchar4 toRGBA(float3 c) {
    return make_uchar4((unsigned char)(clamp01(c.x) * 255.f), (unsigned char)(clamp01(c.y) * 255.f),
                       (unsigned char)(clamp01(c.z) * 255.f), 255);
}

__device__ __forceinline__ float landBit(const unsigned char* mask, int ix, int iy) {
    ix = min(max(ix, 0), kLandW - 1);
    iy = min(max(iy, 0), kLandH - 1);
    int i = iy * kLandW + ix;
    return float((mask[i >> 3] >> (7 - (i & 7))) & 1);
}

__device__ __forceinline__ float landSample(const unsigned char* mask, float lon, float lat) {
    float x = (lon + 180.f) / 360.f * kLandW - 0.5f;
    float y = (90.f - lat) / 180.f * kLandH - 0.5f;
    int x0 = (int)floorf(x);
    int y0 = (int)floorf(y);
    float tx = x - floorf(x);
    float ty = y - floorf(y);
    return (1.f - tx) * (1.f - ty) * landBit(mask, x0, y0) + tx * (1.f - ty) * landBit(mask, x0 + 1, y0) +
           (1.f - tx) * ty * landBit(mask, x0, y0 + 1) + tx * ty * landBit(mask, x0 + 1, y0 + 1);
}

__global__ void computeKeysKernel(const float* lat, const float* lon, const unsigned* time, int n,
                                  float cellDeg, int nLon, int nLat, unsigned long long* keys, int* index) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    int cx = (int)floorf((lon[i] + 180.f) / cellDeg);
    int cy = (int)floorf((lat[i] + 90.f) / cellDeg);
    cx = min(max(cx, 0), nLon - 1);
    cy = min(max(cy, 0), nLat - 1);
    // Cell in the high half so a radix sort groups a place together and orders it by time.
    unsigned cell = unsigned(cy * nLon + cx);
    keys[i] = (unsigned long long)cell << 32 | time[i];
    index[i] = i;
}

__global__ void visitBreaksKernel(const unsigned long long* keys, int n, unsigned gap, int* flags) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    if (i == 0) {
        flags[0] = 1;
        return;
    }
    unsigned long long key = keys[i];
    unsigned long long prev = keys[i - 1];
    unsigned cell = unsigned(key >> 32);
    unsigned prevCell = unsigned(prev >> 32);
    unsigned t = unsigned(key);
    unsigned p = unsigned(prev);
    int split = cell != prevCell;
    if (!split) {
        // Undated photos (time 0) sort first and form their own visit, rather than gluing onto
        // whatever dated shot happens to share the cell.
        if ((t == 0) != (p == 0)) split = 1;
        else if (t != 0 && t - p > gap) split = 1;
    }
    flags[i] = split;
}

__global__ void clusterCountKernel(const int* clusterOf, const int* flags, int n, int* out) {
    if (n <= 0) {
        *out = 0;
        return;
    }
    *out = clusterOf[n - 1] + flags[n - 1];
}

__global__ void markStartsKernel(const int* clusterOf, const int* flags, int n, int* start) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n || !flags[i]) return;
    start[clusterOf[i]] = i;
}

__global__ void summariseKernel(const int* start, int nClusters, int nPhotos, const int* sortedIndex,
                                const float* lat, const float* lon, const unsigned* time, ClusterInfo* out) {
    int c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= nClusters) return;
    int a = start[c];
    int b = (c + 1 < nClusters) ? start[c + 1] : nPhotos;
    double sumLat = 0, sumLon = 0;
    unsigned tMin = 0xFFFFFFFFu, tMax = 0;
    bool anyTime = false;
    for (int i = a; i < b; ++i) {
        int p = sortedIndex[i];
        sumLat += lat[p];
        sumLon += lon[p];
        unsigned t = time[p];
        if (t) {
            anyTime = true;
            tMin = min(tMin, t);
            tMax = max(tMax, t);
        }
    }
    int count = b - a;
    ClusterInfo info;
    info.count = count;
    info.lat = float(sumLat / count);
    info.lon = float(sumLon / count);
    info.tMin = anyTime ? tMin : 0;
    info.tMax = anyTime ? tMax : 0;
    info.begin = a;
    out[c] = info;
}

__global__ void drawMapKernel(cudaSurfaceObject_t surf, int width, int height, MapCamera cam,
                              const unsigned char* land) {
    int x = blockIdx.x * blockDim.x + threadIdx.x;
    int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    float lon = cam.lon + (x - 0.5f * width) / cam.zoom;
    float lat = cam.lat - (y - 0.5f * height) / cam.zoom;
    float3 c;
    if (lon < -180.f || lon > 180.f || lat < -90.f || lat > 90.f) {
        c = make_float3(0.03f, 0.035f, 0.045f);
    } else {
        float ground = landSample(land, lon, lat);
        float3 ocean = make_float3(0.07f, 0.11f, 0.16f);
        float3 earth = make_float3(0.20f, 0.24f, 0.20f);
        c = lerp3(ocean, earth, ground);

        float degPerPx = 1.f / cam.zoom;
        float step = degPerPx > 0.25f ? 30.f : degPerPx > 0.08f ? 10.f : degPerPx > 0.02f ? 1.f : 0.25f;
        float lx = fabsf(lon / step - rintf(lon / step)) * step;
        float ly = fabsf(lat / step - rintf(lat / step)) * step;
        if (lx < degPerPx * 0.8f || ly < degPerPx * 0.8f) c = lerp3(c, make_float3(0.45f, 0.5f, 0.55f), 0.35f);
    }
    surf2Dwrite(toRGBA(c), surf, x * (int)sizeof(uchar4), y);
}

__device__ float3 visitColor(unsigned t, unsigned newest) {
    float3 fresh = make_float3(1.0f, 0.58f, 0.18f);
    float3 old = make_float3(0.45f, 0.62f, 0.72f);
    if (!t || !newest) return fresh;
    float years = float(newest - t) / (3600.f * 24.f * 365.f);
    return lerp3(fresh, old, clamp01(years / 6.f));
}

__global__ void drawVisitsKernel(cudaSurfaceObject_t surf, int width, int height, MapCamera cam, float cellDeg,
                                 const ClusterInfo* clusters, int nClusters, int selected, unsigned newest) {
    int c = blockIdx.x * blockDim.x + threadIdx.x;
    if (c >= nClusters) return;
    ClusterInfo v = clusters[c];
    float sx = 0.5f * width + (v.lon - cam.lon) * cam.zoom;
    float sy = 0.5f * height - (v.lat - cam.lat) * cam.zoom;
    float radius = fminf(fmaxf(5.f, 2.2f * log2f(float(v.count) + 1.f)), 22.f);
    radius = fmaxf(radius, cellDeg * cam.zoom * 0.45f);
    radius = fminf(radius, 72.f);
    int r = min((int)ceilf(radius) + 2, 80);
    if (sx < -r || sy < -r || sx > width + r || sy > height + r) return;

    // Zoomed out, a visit is a disc. Zoomed in, the individual photos take over and the disc
    // becomes a ring so it does not hide them.
    bool ring = cam.zoom * cellDeg > 28.f;
    float3 fill = visitColor(v.tMax, newest);
    bool hot = c == selected;
    for (int oy = -r; oy <= r; ++oy) {
        for (int ox = -r; ox <= r; ++ox) {
            int px = (int)sx + ox;
            int py = (int)sy + oy;
            if (px < 0 || py < 0 || px >= width || py >= height) continue;
            float d = sqrtf(float(ox * ox + oy * oy));
            if (d > radius + (hot ? 3.f : 0.f)) continue;
            bool outline = d > radius - 2.f;
            if (ring && !outline && !hot) continue;
            if (ring && !outline && hot && d < radius - 5.f) continue;
            float3 col = outline || hot ? (hot ? make_float3(1.f, 1.f, 1.f) : fill) : fill;
            if (outline && !hot) col = make_float3(col.x * 0.45f, col.y * 0.45f, col.z * 0.45f);
            surf2Dwrite(toRGBA(col), surf, px * (int)sizeof(uchar4), py);
        }
    }
}

__global__ void drawPhotosKernel(cudaSurfaceObject_t surf, int width, int height, MapCamera cam, const float* lat,
                                 const float* lon, const unsigned* time, int n, int selected, unsigned newest) {
    int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;
    float sx = 0.5f * width + (lon[i] - cam.lon) * cam.zoom;
    float sy = 0.5f * height - (lat[i] - cam.lat) * cam.zoom;
    float radius = (i == selected) ? 6.f : 2.5f;
    int r = (int)ceilf(radius) + 1;
    if (sx < -r || sy < -r || sx > width + r || sy > height + r) return;
    float3 fill = (i == selected) ? make_float3(1.f, 1.f, 1.f) : visitColor(time[i], newest);
    for (int oy = -r; oy <= r; ++oy) {
        for (int ox = -r; ox <= r; ++ox) {
            int px = (int)sx + ox;
            int py = (int)sy + oy;
            if (px < 0 || py < 0 || px >= width || py >= height) continue;
            float d = sqrtf(float(ox * ox + oy * oy));
            if (d > radius) continue;
            surf2Dwrite(toRGBA(fill), surf, px * (int)sizeof(uchar4), py);
        }
    }
}

namespace {

void freeCuda(void*& p) {
    if (p) {
        cudaFree(p);
        p = nullptr;
    }
}

template <typename T>
void freeCuda(T*& p) {
    if (p) {
        cudaFree(p);
        p = nullptr;
    }
}

}  // namespace

TripMap::TripMap() {
    CUDA_CHECK(cudaMalloc(&land_, kLandMaskBytes));
    CUDA_CHECK(cudaMemcpy(land_, kLandMask, kLandMaskBytes, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaEventCreate(&ev0_));
    CUDA_CHECK(cudaEventCreate(&ev1_));
}

TripMap::~TripMap() {
    releaseTexture();
    freeCuda(lat_);
    freeCuda(lon_);
    freeCuda(time_);
    freeCuda(keys_);
    freeCuda(keysSorted_);
    freeCuda(index_);
    freeCuda(indexSorted_);
    freeCuda(flags_);
    freeCuda(clusterOf_);
    freeCuda(clusterStart_);
    freeCuda(clusterDev_);
    freeCuda(countDev_);
    freeCuda(temp_);
    freeCuda(land_);
    if (ev0_) cudaEventDestroy(ev0_);
    if (ev1_) cudaEventDestroy(ev1_);
}

void TripMap::ensureCapacity(int n) {
    if (n <= capacity_) return;
    freeCuda(lat_);
    freeCuda(lon_);
    freeCuda(time_);
    freeCuda(keys_);
    freeCuda(keysSorted_);
    freeCuda(index_);
    freeCuda(indexSorted_);
    freeCuda(flags_);
    freeCuda(clusterOf_);
    freeCuda(clusterStart_);
    freeCuda(clusterDev_);
    capacity_ = n;
    CUDA_CHECK(cudaMalloc(&lat_, sizeof(float) * n));
    CUDA_CHECK(cudaMalloc(&lon_, sizeof(float) * n));
    CUDA_CHECK(cudaMalloc(&time_, sizeof(unsigned) * n));
    CUDA_CHECK(cudaMalloc(&keys_, sizeof(unsigned long long) * n));
    CUDA_CHECK(cudaMalloc(&keysSorted_, sizeof(unsigned long long) * n));
    CUDA_CHECK(cudaMalloc(&index_, sizeof(int) * n));
    CUDA_CHECK(cudaMalloc(&indexSorted_, sizeof(int) * n));
    CUDA_CHECK(cudaMalloc(&flags_, sizeof(int) * n));
    CUDA_CHECK(cudaMalloc(&clusterOf_, sizeof(int) * n));
    CUDA_CHECK(cudaMalloc(&clusterStart_, sizeof(int) * n));
    CUDA_CHECK(cudaMalloc(&clusterDev_, sizeof(ClusterInfo) * n));
    if (!countDev_) CUDA_CHECK(cudaMalloc(&countDev_, sizeof(int)));
}

void TripMap::setPhotos(const std::vector<PhotoRec>& photos) {
    count_ = int(photos.size());
    clusters_.clear();
    order_.clear();
    clusterMs_ = 0.f;
    if (count_ <= 0) return;
    ensureCapacity(count_);
    std::vector<float> lat(count_), lon(count_);
    std::vector<unsigned> time(count_);
    for (int i = 0; i < count_; ++i) {
        lat[i] = photos[i].lat;
        lon[i] = photos[i].lon;
        time[i] = photos[i].time;
    }
    CUDA_CHECK(cudaMemcpy(lat_, lat.data(), sizeof(float) * count_, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(lon_, lon.data(), sizeof(float) * count_, cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemcpy(time_, time.data(), sizeof(unsigned) * count_, cudaMemcpyHostToDevice));
}

void TripMap::cluster(float cellDegrees, unsigned gapSeconds) {
    clusters_.clear();
    order_.clear();
    clusterMs_ = 0.f;
    if (count_ <= 0) return;

    cellDeg_ = std::clamp(cellDegrees, 0.02f, 5.f);
    int nLon = std::max(1, int(std::ceil(360.0 / double(cellDeg_))));
    int nLat = std::max(1, int(std::ceil(180.0 / double(cellDeg_))));

    const int block = 256;
    CUDA_CHECK(cudaEventRecord(ev0_));

    computeKeysKernel<<<blocksFor(count_, block), block>>>(lat_, lon_, time_, count_, cellDeg_, nLon, nLat, keys_,
                                                           index_);
    CUDA_CHECK_LAUNCH();

    std::size_t sortBytes = 0, scanBytes = 0;
    CUDA_CHECK(cub::DeviceRadixSort::SortPairs(nullptr, sortBytes, keys_, keysSorted_, index_, indexSorted_, count_, 0,
                                               64));
    CUDA_CHECK(cub::DeviceScan::ExclusiveSum(nullptr, scanBytes, flags_, clusterOf_, count_));
    std::size_t need = std::max(sortBytes, scanBytes);
    if (need > tempBytes_) {
        freeCuda(temp_);
        tempBytes_ = need;
        CUDA_CHECK(cudaMalloc(&temp_, tempBytes_));
    }
    CUDA_CHECK(cub::DeviceRadixSort::SortPairs(temp_, tempBytes_, keys_, keysSorted_, index_, indexSorted_, count_, 0,
                                               64));
    visitBreaksKernel<<<blocksFor(count_, block), block>>>(keysSorted_, count_, gapSeconds, flags_);
    CUDA_CHECK_LAUNCH();
    CUDA_CHECK(cub::DeviceScan::ExclusiveSum(temp_, tempBytes_, flags_, clusterOf_, count_));
    clusterCountKernel<<<1, 1>>>(clusterOf_, flags_, count_, countDev_);
    CUDA_CHECK_LAUNCH();

    int nClusters = 0;
    CUDA_CHECK(cudaMemcpy(&nClusters, countDev_, sizeof(int), cudaMemcpyDeviceToHost));
    if (nClusters < 0 || nClusters > count_) nClusters = 0;

    if (nClusters > 0) {
        markStartsKernel<<<blocksFor(count_, block), block>>>(clusterOf_, flags_, count_, clusterStart_);
        CUDA_CHECK_LAUNCH();
        summariseKernel<<<blocksFor(nClusters, block), block>>>(clusterStart_, nClusters, count_, indexSorted_, lat_,
                                                                lon_, time_, clusterDev_);
        CUDA_CHECK_LAUNCH();
        clusters_.resize(nClusters);
        CUDA_CHECK(cudaMemcpy(clusters_.data(), clusterDev_, sizeof(ClusterInfo) * nClusters, cudaMemcpyDeviceToHost));
    }
    order_.resize(count_);
    CUDA_CHECK(cudaMemcpy(order_.data(), indexSorted_, sizeof(int) * count_, cudaMemcpyDeviceToHost));

    CUDA_CHECK(cudaEventRecord(ev1_));
    CUDA_CHECK(cudaEventSynchronize(ev1_));
    CUDA_CHECK(cudaEventElapsedTime(&clusterMs_, ev0_, ev1_));
}

void TripMap::releaseTexture() {
    if (surf_) {
        cudaDestroySurfaceObject(surf_);
        surf_ = 0;
    }
    if (resource_) {
        cudaGraphicsUnregisterResource(resource_);
        resource_ = nullptr;
    }
    if (tex_) {
        glDeleteTextures(1, &tex_);
        tex_ = 0;
    }
    texW_ = texH_ = 0;
}

void TripMap::render(int width, int height, const MapCamera& cam, int selectedCluster, int selectedPhoto) {
    if (width <= 0 || height <= 0) return;
    if (width != texW_ || height != texH_) {
        releaseTexture();
        texW_ = width;
        texH_ = height;
        glGenTextures(1, &tex_);
        glBindTexture(GL_TEXTURE_2D, tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glBindTexture(GL_TEXTURE_2D, 0);
        CUDA_CHECK(cudaGraphicsGLRegisterImage(&resource_, tex_, GL_TEXTURE_2D,
                                               cudaGraphicsRegisterFlagsSurfaceLoadStore));
    }

    CUDA_CHECK(cudaGraphicsMapResources(1, &resource_));
    cudaArray_t array = nullptr;
    CUDA_CHECK(cudaGraphicsSubResourceGetMappedArray(&array, resource_, 0, 0));
    cudaResourceDesc desc{};
    desc.resType = cudaResourceTypeArray;
    desc.res.array.array = array;
    CUDA_CHECK(cudaCreateSurfaceObject(&surf_, &desc));

    dim3 block(16, 16);
    dim3 grid((width + block.x - 1) / block.x, (height + block.y - 1) / block.y);
    drawMapKernel<<<grid, block>>>(surf_, width, height, cam, land_);
    CUDA_CHECK_LAUNCH();

    unsigned newest = 0;
    for (const ClusterInfo& c : clusters_) newest = std::max(newest, c.tMax);

    if (!clusters_.empty() && clusterDev_) {
        const int b = 64;
        int nClusters = int(clusters_.size());
        drawVisitsKernel<<<blocksFor(nClusters, b), b>>>(surf_, width, height, cam, cellDeg_, clusterDev_, nClusters,
                                                        -1, newest);
        CUDA_CHECK_LAUNCH();
        // Redraw the selection last so a neighbouring visit cannot paint over the highlight.
        if (selectedCluster >= 0 && selectedCluster < nClusters) {
            drawVisitsKernel<<<1, 1>>>(surf_, width, height, cam, cellDeg_, clusterDev_ + selectedCluster, 1, 0, newest);
            CUDA_CHECK_LAUNCH();
        }
    }
    // Individual shots are only readable once a degree covers a few dozen pixels.
    if (count_ > 0 && cam.zoom >= 40.f) {
        drawPhotosKernel<<<blocksFor(count_, 128), 128>>>(surf_, width, height, cam, lat_, lon_, time_, count_,
                                                          selectedPhoto, newest);
        CUDA_CHECK_LAUNCH();
    }

    CUDA_CHECK(cudaDestroySurfaceObject(surf_));
    surf_ = 0;
    CUDA_CHECK(cudaGraphicsUnmapResources(1, &resource_));
}
