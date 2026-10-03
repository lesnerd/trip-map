#pragma once
#include <cstdio>
#include <cstdlib>
#include <cuda_runtime.h>

#define CUDA_CHECK(call)                                                        \
    do {                                                                        \
        cudaError_t err_ = (call);                                              \
        if (err_ != cudaSuccess) {                                              \
            std::fprintf(stderr, "CUDA error %s at %s:%d: %s\n",                \
                         cudaGetErrorName(err_), __FILE__, __LINE__,            \
                         cudaGetErrorString(err_));                             \
            std::exit(EXIT_FAILURE);                                            \
        }                                                                       \
    } while (0)

// Kernel launches don't return an error. In debug builds, also wait, so a fault
// is reported on the launch that caused it.
#ifdef NDEBUG
#define CUDA_CHECK_LAUNCH() CUDA_CHECK(cudaGetLastError())
#else
#define CUDA_CHECK_LAUNCH()                  \
    do {                                     \
        CUDA_CHECK(cudaGetLastError());      \
        CUDA_CHECK(cudaDeviceSynchronize()); \
    } while (0)
#endif

inline unsigned blocksFor(int n, int blockSize) {
    return n <= 0 ? 0u : unsigned((n + blockSize - 1) / blockSize);
}
