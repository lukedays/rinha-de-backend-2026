// Microbench da busca: latencia (p50/p99) e clusters/vetores visitados.
#include "index.h"
#include "search.h"
#include "constants.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern __thread long g_clusters_visited, g_vectors_scanned;

static uint64_t now_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + t.tv_nsec;
}
static int cmp_d(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static void unpack(const int16_t *chunks, int chunk, int lane, int16_t v[DIM]) {
    const int16_t *c = chunks + (size_t)chunk * 112;
    for (int p = 0; p < 7; p++) { v[2 * p] = c[p * 16 + lane * 2]; v[2 * p + 1] = c[p * 16 + lane * 2 + 1]; }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "uso: bench <index.bin> [queries=20000] [noise=30]\n"); return 1; }
    int Q = argc > 2 ? atoi(argv[2]) : 20000;
    int noise = argc > 3 ? atoi(argv[3]) : 30;

    index_t ix;
    if (index_load(argv[1], &ix) != 0) return 1;
    printf("indice: N=%d K=%d chunks=%d max_lanes=%d\n", ix.n, ix.k, ix.total_chunks, ix.max_lanes);

    srand(123);
    int16_t *queries = malloc((size_t)Q * 16 * sizeof(int16_t));
    for (int i = 0; i < Q; i++) {
        int ch = rand() % ix.total_chunks, lane = rand() % 8;
        int16_t base[DIM];
        unpack(ix.chunks, ch, lane, base);
        int16_t *q = queries + (size_t)i * 16;
        for (int d = 0; d < DIM; d++) {
            int v = base[d] + (rand() % (2 * noise + 1) - noise);
            q[d] = (int16_t)(v < -32768 ? -32768 : v > 32767 ? 32767 : v);
        }
        q[14] = 0; q[15] = 0;
    }

    int32_t *scratch = malloc((size_t)ix.max_lanes * sizeof(int32_t));
    for (int i = 0; i < 2000; i++) fraud_count(&ix, queries + (size_t)(i % Q) * 16, scratch);

    double *lat = malloc((size_t)Q * sizeof(double));
    long sum_cl = 0, sum_vec = 0;
    for (int i = 0; i < Q; i++) {
        uint64_t t0 = now_ns();
        fraud_count(&ix, queries + (size_t)i * 16, scratch);
        lat[i] = (now_ns() - t0) / 1000.0;
        sum_cl += g_clusters_visited;
        sum_vec += g_vectors_scanned;
    }
    qsort(lat, Q, sizeof(double), cmp_d);
    printf("queries=%d noise=%d\n", Q, noise);
    printf("  latencia: p50=%.3fus p99=%.3fus max=%.3fus\n", lat[Q/2], lat[(int)(Q*0.99)], lat[Q-1]);
    printf("  clusters visitados (media): %.1f / %d\n", (double)sum_cl / Q, ix.k);
    printf("  vetores escaneados (media): %.0f / %d (%.2f%%)\n", (double)sum_vec / Q, ix.n, 100.0 * sum_vec / Q / ix.n);
    return 0;
}
