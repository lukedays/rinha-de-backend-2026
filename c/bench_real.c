// Microbench da busca sobre os PAYLOADS REAIS (mesma massa do k6): isola a
// latencia de fraud_count sem serving. Loop rapido p/ iterar otimizacao.
#include "index.h"
#include "parser.h"
#include "search.h"
#include "vectorize.h"
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

// Itera objetos {...} de um array JSON (igual ao selftest).
typedef void (*obj_cb)(const uint8_t *, size_t, void *);
static void each_object(const uint8_t *buf, size_t n, obj_cb cb, void *ud) {
    size_t i = 0;
    while (i < n && buf[i] != '[') i++;
    i++;
    while (i < n) {
        while (i < n && buf[i] != '{' && buf[i] != ']') i++;
        if (i >= n || buf[i] == ']') break;
        size_t start = i, depth = 0; int in_str = 0;
        for (; i < n; i++) {
            uint8_t ch = buf[i];
            if (in_str) { if (ch == '"') in_str = 0; continue; }
            if (ch == '"') in_str = 1;
            else if (ch == '{') depth++;
            else if (ch == '}') { depth--; if (depth == 0) { i++; break; } }
        }
        cb(buf + start, i - start, ud);
    }
}

typedef struct { int16_t (*q)[16]; uint8_t (*raw)[1024]; int *rlen; int n, cap; } qset_t;
static void collect(const uint8_t *obj, size_t len, void *ud) {
    qset_t *s = (qset_t *)ud;
    tx_input t;
    if (!parse_payload(obj, len, &t)) return;
    if (s->n >= s->cap || len >= 1024) return;
    float v[DIM];
    to_vector(&t, v);
    quantize(v, s->q[s->n]);
    memcpy(s->raw[s->n], obj, len); s->rlen[s->n] = (int)len;
    s->n++;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: bench_real <index.bin> <payloads.json> [iters=20000]\n"); return 1; }
    int iters = argc > 3 ? atoi(argv[3]) : 20000;
    index_t ix;
    if (index_load(argv[1], &ix) != 0) return 1;

    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("payloads"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("fread"); return 1; }
    fclose(f);

    qset_t qs = { .q = malloc(4096 * sizeof(*qs.q)), .raw = malloc(4096 * sizeof(*qs.raw)),
                  .rlen = malloc(4096 * sizeof(int)), .n = 0, .cap = 4096 };
    each_object(buf, sz, collect, &qs);
    printf("indice K=%d | payloads=%d | iters=%d\n", ix.k, qs.n, iters);
    if (qs.n == 0) return 1;

    // custo do parse+vectorize+quantize isolado (sem busca)
    {
        int16_t qq[16]; tx_input t; float v[DIM];
        for (int i = 0; i < 4000; i++) { const int j = i % qs.n; parse_payload(qs.raw[j], qs.rlen[j], &t); to_vector(&t, v); quantize(v, qq); }
        double *pl = malloc((size_t)iters * sizeof(double));
        for (int i = 0; i < iters; i++) {
            const int j = i % qs.n;
            uint64_t t0 = now_ns();
            parse_payload(qs.raw[j], qs.rlen[j], &t); to_vector(&t, v); quantize(v, qq);
            pl[i] = (now_ns() - t0) / 1000.0;
        }
        qsort(pl, iters, sizeof(double), cmp_d);
        printf("  parse+vec+quant: p50=%.3fus p99=%.3fus\n", pl[iters / 2], pl[(int)(iters * 0.99)]);
        free(pl);
    }

    int32_t *scratch = malloc((size_t)ix.max_lanes * sizeof(int32_t));
    for (int i = 0; i < 4000; i++) fraud_count(&ix, qs.q[i % qs.n], scratch);

    double *lat = malloc((size_t)iters * sizeof(double));
    long sum_vec = 0;
    for (int i = 0; i < iters; i++) {
        const int16_t *q = qs.q[i % qs.n];
        uint64_t t0 = now_ns();
        fraud_count(&ix, q, scratch);
        lat[i] = (now_ns() - t0) / 1000.0;
        sum_vec += g_vectors_scanned;
    }
    qsort(lat, iters, sizeof(double), cmp_d);
    printf("  latencia: p50=%.3fus p90=%.3fus p99=%.3fus max=%.3fus\n",
           lat[iters / 2], lat[(int)(iters * 0.90)], lat[(int)(iters * 0.99)], lat[iters - 1]);
    printf("  vetores escaneados (media): %.0f (%.2f%%)\n", (double)sum_vec / iters, 100.0 * sum_vec / iters / ix.n);
    return 0;
}
