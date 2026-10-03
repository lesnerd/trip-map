#pragma once
#include <cuda_runtime.h>
#include <cstdint>
#include <vector>

#include "photos.h"

// lon/lat of the view centre, and framebuffer pixels per degree of longitude.
struct MapCamera {
    float lon = 0.f;
    float lat = 0.f;
    float zoom = 4.f;
};

// One visit: photos that landed in the same map cell and were not split by a time gap.
// `begin` indexes TripMap::order(), which holds `count` photo indices for this visit.
struct ClusterInfo {
    int count;
    float lat;
    float lon;
    unsigned tMin;
    unsigned tMax;
    int begin;
};

// Photo locations live on the device. Clustering is a radix sort by map cell, then a scan
// that turns time gaps into visit ids. The window texture is written by CUDA directly.
class TripMap {
public:
    TripMap();
    ~TripMap();
    TripMap(const TripMap&) = delete;
    TripMap& operator=(const TripMap&) = delete;

    void setPhotos(const std::vector<PhotoRec>& photos);
    void cluster(float cellDegrees, unsigned gapSeconds);
    void render(int width, int height, const MapCamera& cam, int selectedCluster, int selectedPhoto);

    unsigned glTexture() const { return tex_; }
    const std::vector<ClusterInfo>& clusters() const { return clusters_; }
    const std::vector<int>& order() const { return order_; }
    float clusterMilliseconds() const { return clusterMs_; }
    int count() const { return count_; }

private:
    void releaseTexture();
    void ensureCapacity(int n);

    int count_ = 0;
    int capacity_ = 0;
    float cellDeg_ = 0.5f;
    float clusterMs_ = 0.f;

    float* lat_ = nullptr;
    float* lon_ = nullptr;
    unsigned* time_ = nullptr;
    unsigned long long* keys_ = nullptr;
    unsigned long long* keysSorted_ = nullptr;
    int* index_ = nullptr;
    int* indexSorted_ = nullptr;
    int* flags_ = nullptr;
    int* clusterOf_ = nullptr;
    int* clusterStart_ = nullptr;
    ClusterInfo* clusterDev_ = nullptr;
    int* countDev_ = nullptr;
    void* temp_ = nullptr;
    std::size_t tempBytes_ = 0;

    unsigned char* land_ = nullptr;
    cudaEvent_t ev0_ = nullptr;
    cudaEvent_t ev1_ = nullptr;

    std::vector<ClusterInfo> clusters_;
    std::vector<int> order_;

    unsigned tex_ = 0;
    int texW_ = 0;
    int texH_ = 0;
    cudaGraphicsResource_t resource_ = nullptr;
    cudaSurfaceObject_t surf_ = 0;
};
