// Valida o pipeline C (parse -> vetor -> quant -> busca 8-lanes + early-stop)
// comparando com brute force EXATO escalar (referencia independente do asm).
#include "index.h"
#include "parser.h"
#include "search.h"
#include "vectorize.h"
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static void unpack(const int16_t *chunks, int chunk, int lane, int16_t v[DIM]) {
    const int16_t *c = chunks + (size_t)chunk * 112;
    for (int p = 0; p < 7; p++) { v[2 * p] = c[p * 16 + lane * 2]; v[2 * p + 1] = c[p * 16 + lane * 2 + 1]; }
}

// Brute force escalar sobre todos os vetores reais (gabarito independente).
static int brute_force(const index_t *ix, const int16_t q[16]) {
    int bd[5] = { INT_MAX, INT_MAX, INT_MAX, INT_MAX, INT_MAX };
    uint8_t bl[5] = { 0, 0, 0, 0, 0 };
    int worst = INT_MAX, wp = 0;
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

static void each_object(const uint8_t *buf, size_t n, void (*cb)(const uint8_t *, size_t, void *), void *ud) {
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

typedef struct {
    const index_t *ix;
    int32_t *scratch;
    int total, count_mismatch, dec_mismatch, shown;
} ctx_t;

static void on_obj(const uint8_t *obj, size_t len, void *ud) {
    ctx_t *c = (ctx_t *)ud;
    tx_input t;
    if (!parse_payload(obj, len, &t)) return;
    float v[DIM];
    int16_t q[16];
    to_vector(&t, v);
    quantize(v, q);
    int got = fraud_count(c->ix, q, c->scratch);
    int bf = brute_force(c->ix, q);
    c->total++;
    if (got != bf) c->count_mismatch++;
    if ((got < THRESHOLD_FRAUDS) != (bf < THRESHOLD_FRAUDS)) c->dec_mismatch++;
    if (c->shown < 8) {
        printf("  bf nf=%d approved=%d | busca nf=%d approved=%d\n",
               bf, bf < THRESHOLD_FRAUDS, got, got < THRESHOLD_FRAUDS);
        c->shown++;
    }
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "uso: selftest <index.bin> <payloads.json>\n"); return 1; }
    index_t ix;
    if (index_load(argv[1], &ix) != 0) return 1;
    printf("indice: N=%d K=%d chunks=%d max_lanes=%d\n", ix.n, ix.k, ix.total_chunks, ix.max_lanes);

    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("payloads"); return 1; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(sz);
    if (fread(buf, 1, sz, f) != (size_t)sz) { perror("fread"); return 1; }
    fclose(f);

    ctx_t c = { .ix = &ix, .scratch = malloc((size_t)ix.max_lanes * sizeof(int32_t)) };
    each_object(buf, sz, on_obj, &c);
    printf("total=%d\n", c.total);
    printf("  fraud_count divergente: %d\n", c.count_mismatch);
    printf("  decisao divergente:     %d\n", c.dec_mismatch);
    return 0;
}
