#ifndef RINHA_INDEX_H
#define RINHA_INDEX_H

#include <stddef.h>
#include <stdint.h>

// Indice IVF int16 (v3): AABB por cluster + vetores em chunks pair-interleaved
// (8 lanes) para o kernel madd. mmap read-only.
typedef struct {
    int n;
    int k;
    int total_chunks;
    const float *centroids;        // k*14
    const int16_t *aabb_min;       // k*14
    const int16_t *aabb_max;       // k*14
    const int32_t *offsets;        // k+1 (em vetores)
    const int32_t *chunk_offsets;  // k+1 (em chunks)
    const uint8_t *labels;         // n
    const int16_t *chunks;         // total_chunks*112
    int max_lanes;                 // maior cluster arredondado a multiplo de 8 (dim do scratch)
    void *map;
    size_t map_len;
} index_t;

int index_load(const char *path, index_t *ix);

#endif
