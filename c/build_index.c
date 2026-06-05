// Builder offline (C puro): references.json.gz -> indice IVF int16 com AABB por
// cluster. Quantiza int16 escala 10000 (lossless), k-means, reordena por cluster.
//
// Formato index_v2.bin (little-endian):
//   magic "RNH2" u32 | N i32 | dim i32 (14) | K i32
//   centroids: K*14 f32
//   aabb_min:  K*14 i16   aabb_max: K*14 i16
//   offsets:   (K+1) i32
//   data:      N*14 i16 (reordenado por cluster)
//   labels:    N u8
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include "constants.h"

#define MAGIC2 0x32484E52u // "RNH2" em LE

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

// Descomprime todo o .gz para memoria.
static uint8_t *read_gz(const char *path, size_t *out_len) {
    gzFile f = gzopen(path, "rb");
    if (!f) { perror("gzopen"); exit(1); }
    size_t cap = 1u << 28, len = 0;
    uint8_t *buf = malloc(cap);
    for (;;) {
        if (len + (1u << 20) > cap) { cap *= 2; buf = realloc(buf, cap); }
        int n = gzread(f, buf + len, 1u << 20);
        if (n <= 0) break;
        len += n;
    }
    gzclose(f);
    *out_len = len;
    return buf;
}

// Parser do array [{ "vector":[14], "label":"fraud|legit" }, ...] -> int16 + labels.
static int parse_refs(const uint8_t *b, size_t n, int16_t **out_data, uint8_t **out_lab) {
    size_t cap = 3100000;
    int16_t *data = malloc((size_t)cap * DIM * sizeof(int16_t));
    uint8_t *lab = malloc(cap);
    int count = 0;
    size_t i = 0;
    while (i < n && b[i] != '[') i++;
    i++;
    while (i < n) {
        while (i < n && b[i] != '{' && b[i] != ']') i++;
        if (i >= n || b[i] == ']') break;
        // dentro do objeto: achar "vector"
        while (i < n && b[i] != '[') i++; // inicio do array vector
        i++;
        int16_t *dst = data + (size_t)count * DIM;
        for (int d = 0; d < DIM; d++) {
            while (i < n && (b[i] == ' ' || b[i] == ',')) i++;
            char *end;
            float v = strtof((const char *)(b + i), &end);
            i = (size_t)((const uint8_t *)end - b);
            dst[d] = quant_i16(v);
        }
        while (i < n && b[i] != ']') i++; // fim do array vector
        i++;
        // label
        while (i < n && b[i] != 'f' && b[i] != 'l' && b[i] != '}') {
            // pular ate o valor da label (procura "fraud"/"legit" apos "label")
            if (b[i] == 'l' && i + 6 < n && !memcmp(b + i, "label", 5)) break;
            i++;
        }
        // achar o valor "fraud" ou "legit"
        size_t j = i;
        while (j < n && b[j] != '}') {
            if (b[j] == 'f' && j + 5 <= n && !memcmp(b + j, "fraud", 5)) { lab[count] = 1; break; }
            if (b[j] == 'l' && j + 5 <= n && !memcmp(b + j, "legit", 5)) { lab[count] = 0; break; }
            j++;
        }
        while (i < n && b[i] != '}') i++;
        i++;
        count++;
        if ((size_t)count >= cap) { cap *= 2; data = realloc(data, (size_t)cap * DIM * sizeof(int16_t)); lab = realloc(lab, cap); }
    }
    *out_data = data;
    *out_lab = lab;
    return count;
}

// --- k-means ---
static float *kmeans(const int16_t *data, int n, int k, int iters, unsigned seed) {
    float *cen = malloc((size_t)k * DIM * sizeof(float));
    srand(seed);
    for (int c = 0; c < k; c++) {
        long src = rand() % n;
        for (int d = 0; d < DIM; d++) cen[c * DIM + d] = data[src * DIM + d];
    }
    int sample = n < 300000 ? n : 300000;
    int *sidx = malloc((size_t)sample * sizeof(int));
    for (int s = 0; s < sample; s++) sidx[s] = rand() % n;

    double *sum = malloc((size_t)k * DIM * sizeof(double));
    int *cnt = malloc((size_t)k * sizeof(int));
    for (int it = 0; it < iters; it++) {
        memset(sum, 0, (size_t)k * DIM * sizeof(double));
        memset(cnt, 0, (size_t)k * sizeof(int));
        for (int s = 0; s < sample; s++) {
            const int16_t *p = data + (size_t)sidx[s] * DIM;
            int best = 0; double bd = 1e30;
            for (int c = 0; c < k; c++) {
                const float *cc = cen + c * DIM;
                double dd = 0;
                for (int d = 0; d < DIM; d++) { double df = p[d] - cc[d]; dd += df * df; }
                if (dd < bd) { bd = dd; best = c; }
            }
            cnt[best]++;
            for (int d = 0; d < DIM; d++) sum[best * DIM + d] += p[d];
        }
        for (int c = 0; c < k; c++) {
            if (!cnt[c]) { long r = rand() % n; for (int d = 0; d < DIM; d++) cen[c * DIM + d] = data[r * DIM + d]; continue; }
            for (int d = 0; d < DIM; d++) cen[c * DIM + d] = (float)(sum[c * DIM + d] / cnt[c]);
        }
        fprintf(stderr, "  kmeans iter %d/%d\r", it + 1, iters);
    }
    fprintf(stderr, "\n");
    free(sidx); free(sum); free(cnt);
    return cen;
}

static int nearest(const int16_t *p, const float *cen, int k) {
    int best = 0; double bd = 1e30;
    for (int c = 0; c < k; c++) {
        const float *cc = cen + c * DIM;
        double dd = 0;
        for (int d = 0; d < DIM; d++) { double df = p[d] - cc[d]; dd += df * df; }
        if (dd < bd) { bd = dd; best = c; }
    }
    return best;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: build_index <references.json.gz> <out.bin> [K=2048] [iters=12]\n"); return 1; }
    int K = argc > 3 ? atoi(argv[3]) : 2048;
    int iters = argc > 4 ? atoi(argv[4]) : 12;

    double t = now();
    size_t blen;
    uint8_t *raw = read_gz(argv[1], &blen);
    fprintf(stderr, "descomprimido: %zu bytes (%.1fs)\n", blen, now() - t);

    int16_t *data; uint8_t *lab;
    t = now();
    int n = parse_refs(raw, blen, &data, &lab);
    free(raw);
    fprintf(stderr, "parse: %d vetores (%.1fs)\n", n, now() - t);

    t = now();
    float *cen = kmeans(data, n, K, iters, 42);
    fprintf(stderr, "kmeans (%.1fs)\n", now() - t);

    // atribuir todos e reordenar por cluster (CSR)
    t = now();
    int *assign = malloc((size_t)n * sizeof(int));
    int *off = calloc((size_t)K + 1, sizeof(int));
    for (int i = 0; i < n; i++) { int c = nearest(data + (size_t)i * DIM, cen, K); assign[i] = c; off[c + 1]++; }
    for (int c = 0; c < K; c++) off[c + 1] += off[c];

    int16_t *sdata = malloc((size_t)n * DIM * sizeof(int16_t));
    uint8_t *slab = malloc((size_t)n);
    int16_t *amin = malloc((size_t)K * DIM * sizeof(int16_t));
    int16_t *amax = malloc((size_t)K * DIM * sizeof(int16_t));
    for (int c = 0; c < K; c++) for (int d = 0; d < DIM; d++) { amin[c * DIM + d] = 32767; amax[c * DIM + d] = -32768; }
    int *cur = malloc((size_t)K * sizeof(int));
    memcpy(cur, off, (size_t)K * sizeof(int));
    for (int i = 0; i < n; i++) {
        int c = assign[i];
        int pos = cur[c]++;
        const int16_t *src = data + (size_t)i * DIM;
        memcpy(sdata + (size_t)pos * DIM, src, DIM * sizeof(int16_t));
        slab[pos] = lab[i];
        for (int d = 0; d < DIM; d++) {
            if (src[d] < amin[c * DIM + d]) amin[c * DIM + d] = src[d];
            if (src[d] > amax[c * DIM + d]) amax[c * DIM + d] = src[d];
        }
    }
    fprintf(stderr, "assign+reorder+aabb (%.1fs)\n", now() - t);

    // chunks pair-interleaved (8 vetores/chunk) para o kernel madd 8-lanes.
    // chunk[p*16 + lane*2 + r] = vetor_lane[2p + r], p=0..6, r=0..1, lane=0..7.
    // Lanes de padding no ultimo chunk recebem o ultimo vetor real (sem overflow).
    int *choff = calloc((size_t)K + 1, sizeof(int));
    for (int c = 0; c < K; c++) { int sz = off[c + 1] - off[c]; choff[c + 1] = choff[c] + (sz + 7) / 8; }
    int total_chunks = choff[K];
    int16_t *chunks = calloc((size_t)total_chunks * 112, sizeof(int16_t));
    for (int c = 0; c < K; c++) {
        int s = off[c], sz = off[c + 1] - off[c];
        int nch = (sz + 7) / 8;
        for (int ch = 0; ch < nch; ch++) {
            int16_t *dst = chunks + (size_t)(choff[c] + ch) * 112;
            for (int lane = 0; lane < 8; lane++) {
                int vi = ch * 8 + lane;
                if (vi >= sz) vi = sz - 1; // padding = ultimo vetor real
                const int16_t *v = sdata + (size_t)(s + vi) * DIM;
                for (int p = 0; p < 7; p++) {
                    dst[p * 16 + lane * 2 + 0] = v[2 * p];
                    dst[p * 16 + lane * 2 + 1] = v[2 * p + 1];
                }
            }
        }
    }

    // serializar v3
    FILE *f = fopen(argv[2], "wb");
    uint32_t magic = 0x33484E52u; int dim = DIM; // "RNH3"
    fwrite(&magic, 4, 1, f); fwrite(&n, 4, 1, f); fwrite(&dim, 4, 1, f); fwrite(&K, 4, 1, f);
    fwrite(&total_chunks, 4, 1, f);
    fwrite(cen, sizeof(float), (size_t)K * DIM, f);
    fwrite(amin, sizeof(int16_t), (size_t)K * DIM, f);
    fwrite(amax, sizeof(int16_t), (size_t)K * DIM, f);
    fwrite(off, sizeof(int), (size_t)K + 1, f);
    fwrite(choff, sizeof(int), (size_t)K + 1, f);
    fwrite(slab, 1, (size_t)n, f);
    fwrite(chunks, sizeof(int16_t), (size_t)total_chunks * 112, f);
    fclose(f);
    fprintf(stderr, "salvo %s (N=%d K=%d chunks=%d)\n", argv[2], n, K, total_chunks);
    return 0;
}
