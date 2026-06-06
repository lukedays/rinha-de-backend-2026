#ifndef RINHA_INDEX_H
#define RINHA_INDEX_H

#include <stddef.h>
#include <stdint.h>

// No da KD-tree (layout identico ao gravado pelo build_index): 16 bytes.
// interno: a=filho esq, b=filho dir; leaf: a=chunk_start, b=count, c=vec_start.
typedef struct { uint8_t leaf, dim; int16_t split; int32_t a, b, c; int16_t bmin[16], bmax[16]; } kdnode;

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

    // Filtro grosseiro de 2 niveis (construido em memoria no load): agrupa os k
    // clusters em n_super super-clusters. super_min/max e o AABB-uniao dos
    // membros (lower bound admissivel p/ todos eles) -> a busca varre ~n_super
    // super-AABBs + os membros dos promissores, em vez dos k AABBs por query.
    int n_super;
    int16_t *super_min;            // n_super*16 (padded; dims 14,15 = 0)
    int16_t *super_max;            // n_super*16
    int32_t *super_off;            // n_super+1 (indices em super_mem)
    int32_t *super_mem;            // k ids de cluster, agrupados por super

    // KD-tree (formato "RNH4"): quando is_kd=1, a busca usa a arvore em vez da IVF.
    int is_kd;
    int n_nodes;
    int kd_root;
    const kdnode *nodes;           // n_nodes (mmap)
} index_t;

int index_load(const char *path, index_t *ix);

#endif
