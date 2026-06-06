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

// Brute force escalar exato (gabarito independente do asm/busca).
static int brute_force(const index_t *ix, const int16_t q[16]) {
    int bd[5] = { 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff, 0x7fffffff };
    uint8_t bl[5] = { 0, 0, 0, 0, 0 };
    int worst = 0x7fffffff, wp = 0;
    for (int c = 0; c < ix->k; c++) {
        int vstart = ix->offsets[c], size = ix->offsets[c + 1] - vstart, cstart = ix->chunk_offsets[c];
        for (int j = 0; j < size; j++) {
            int16_t v[DIM];
            unpack(ix->chunks, cstart + j / 8, j % 8, v);
            int dist = 0;
            for (int d = 0; d < DIM; d++) { int df = v[d] - q[d]; dist += df * df; }
            if (dist >= worst) continue;
            bd[wp] = dist; bl[wp] = ix->labels[vstart + j];
            worst = bd[0]; wp = 0;
            for (int t = 1; t < 5; t++) if (bd[t] > worst) { worst = bd[t]; wp = t; }
        }
    }
    return bl[0] + bl[1] + bl[2] + bl[3] + bl[4];
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

    // Validacao de exatidao. Se um indice de REFERENCIA for passado (argv[4]),
    // compara fraud_count(test) vs fraud_count(ref) nas mesmas queries — formato-
    // agnostico (valida o KD contra a IVF ja validada exata). Senao, paridade IVF.
    if (argc > 4) {
        index_t ref;
        if (index_load(argv[4], &ref) != 0) return 1;
        int32_t *scrr = malloc(((size_t)ref.max_lanes + 8) * sizeof(int32_t));
        long nf_mism = 0, dec_mism = 0;
        for (int i = 0; i < Q; i++) {
            const int16_t *q = queries + (size_t)i * 16;
            int a = fraud_count(&ix, q, scratch);
            int b = fraud_count(&ref, q, scrr);
            if (a != b) nf_mism++;
            if ((a < THRESHOLD_FRAUDS) != (b < THRESHOLD_FRAUDS)) dec_mism++;
        }
        printf("  vs REF (%s): nf-mism=%ld | DECISAO-mism=%ld / %d\n", argv[4], nf_mism, dec_mism, Q);
    } else if (!ix.is_kd) {
        long nf_mism = 0, dec_mism = 0, brute_dec_mism = 0;
        for (int i = 0; i < Q; i++) {
            const int16_t *q = queries + (size_t)i * 16;
            int a = fraud_count(&ix, q, scratch);
            int b = fraud_count_single(&ix, q, scratch);
            if (a != b) nf_mism++;
            if ((a < THRESHOLD_FRAUDS) != (b < THRESHOLD_FRAUDS)) dec_mism++;
        }
        int nb = Q < 4000 ? Q : 4000;
        for (int i = 0; i < nb; i++) {
            const int16_t *q = queries + (size_t)i * 16;
            int a = fraud_count(&ix, q, scratch);
            int bf = brute_force(&ix, q);
            if ((a < THRESHOLD_FRAUDS) != (bf < THRESHOLD_FRAUDS)) brute_dec_mism++;
        }
        printf("  paridade nf(2v-1v)=%ld | DECISAO(2v-1v)=%ld/%d | DECISAO(2v-brute)=%ld/%d\n",
               nf_mism, dec_mism, Q, brute_dec_mism, nb);
    }

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
