#include "index.h"
#include "constants.h"
#include <fcntl.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// Agrupa os k clusters em S ~= sqrt(k) super-clusters via k-means nos centroides
// (em memoria, no load). super_min/max = AABB-uniao dos membros: lower bound
// admissivel p/ todos eles, entao a busca poda super-clusters inteiros (2 niveis)
// sem varrer os k AABBs por query. Em falha, deixa n_super=0 (busca cai p/ 1 nivel).
static void build_superclusters(index_t *ix) {
    ix->n_super = 0;
    ix->super_min = NULL; ix->super_max = NULL; ix->super_off = NULL; ix->super_mem = NULL;
    int k = ix->k;
    int S = 1;
    while (S * S < k) S++;            // S = ceil(sqrt(k)): melhor tradeoff selecao/pruning
    if (S > 256) S = 256;            // cap = MAX_SUPER do search.c
    if (S < 2 || S >= k) return;     // poucos clusters: nao vale 2 niveis

    float *cen = malloc((size_t)S * DIM * sizeof(float));
    int *assign = malloc((size_t)k * sizeof(int));
    float *sums = malloc((size_t)S * DIM * sizeof(float));
    int *cnt = malloc((size_t)S * sizeof(int));
    int16_t *smin = malloc((size_t)S * 16 * sizeof(int16_t));
    int16_t *smax = malloc((size_t)S * 16 * sizeof(int16_t));
    int32_t *soff = malloc((size_t)(S + 1) * sizeof(int32_t));
    int32_t *smem = malloc((size_t)k * sizeof(int32_t));
    int *cur = malloc((size_t)S * sizeof(int));
    if (!cen || !assign || !sums || !cnt || !smin || !smax || !soff || !smem || !cur) {
        free(cen); free(assign); free(sums); free(cnt);
        free(smin); free(smax); free(soff); free(smem); free(cur);
        return;
    }

    for (int s = 0; s < S; s++)       // init: striding nos centroides
        memcpy(cen + (size_t)s * DIM, ix->centroids + (size_t)((long)s * k / S) * DIM, DIM * sizeof(float));

    for (int it = 0; it < 6; it++) {
        memset(sums, 0, (size_t)S * DIM * sizeof(float));
        memset(cnt, 0, (size_t)S * sizeof(int));
        for (int c = 0; c < k; c++) {
            const float *v = ix->centroids + (size_t)c * DIM;
            int best = 0; float bd = FLT_MAX;
            for (int s = 0; s < S; s++) {
                const float *cc = cen + (size_t)s * DIM;
                float d = 0;
                for (int j = 0; j < DIM; j++) { float df = v[j] - cc[j]; d += df * df; }
                if (d < bd) { bd = d; best = s; }
            }
            assign[c] = best;
            float *su = sums + (size_t)best * DIM;
            for (int j = 0; j < DIM; j++) su[j] += v[j];
            cnt[best]++;
        }
        for (int s = 0; s < S; s++) {
            if (cnt[s] == 0) continue;
            float *cc = cen + (size_t)s * DIM, *su = sums + (size_t)s * DIM;
            for (int j = 0; j < DIM; j++) cc[j] = su[j] / (float)cnt[s];
        }
    }

    soff[0] = 0;
    for (int s = 0; s < S; s++) soff[s + 1] = soff[s] + cnt[s];
    for (int s = 0; s < S; s++) cur[s] = soff[s];
    for (int c = 0; c < k; c++) { int s = assign[c]; smem[cur[s]++] = c; }

    for (int s = 0; s < S; s++) {
        int16_t *mn = smin + (size_t)s * 16, *mx = smax + (size_t)s * 16;
        for (int j = 0; j < 16; j++) { mn[j] = 32767; mx[j] = -32768; }
        for (int m = soff[s]; m < soff[s + 1]; m++) {
            int c = smem[m];
            const int16_t *amn = ix->aabb_min + (size_t)c * DIM, *amx = ix->aabb_max + (size_t)c * DIM;
            for (int j = 0; j < DIM; j++) {
                if (amn[j] < mn[j]) mn[j] = amn[j];
                if (amx[j] > mx[j]) mx[j] = amx[j];
            }
        }
        mn[14] = mn[15] = 0; mx[14] = mx[15] = 0;
    }

    free(cen); free(assign); free(sums); free(cnt); free(cur);
    ix->n_super = S; ix->super_min = smin; ix->super_max = smax;
    ix->super_off = soff; ix->super_mem = smem;
}

// "RNH3" (bytes 0x52,0x4E,0x48,0x33) => 0x33484E52 como u32 little-endian.
#define MAGIC3 0x33484E52u
#define MAGIC4 0x34484E52u  // "RNH4": KD-tree

static uint32_t rd_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static int32_t rd_i32(const uint8_t *p) { return (int32_t)rd_u32(p); }

int index_load(const char *path, index_t *ix) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("open index"); return -1; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return -1; }
    size_t len = (size_t)st.st_size;
    void *map = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { perror("mmap"); return -1; }
    // mantem o indice quente na RAM: sem page-fault na cauda de latencia.
    madvise(map, len, MADV_WILLNEED);
    mlock(map, len); // best-effort (precisa de ulimit memlock); ignora falha

    const uint8_t *b = (const uint8_t *)map;
    ix->is_kd = 0; ix->n_super = 0;
    ix->super_min = NULL; ix->super_max = NULL; ix->super_off = NULL; ix->super_mem = NULL;
    ix->map = map; ix->map_len = len;

    if (rd_u32(b) == MAGIC4) {  // KD-tree
        int n = rd_i32(b + 4), dim = rd_i32(b + 8), n_nodes = rd_i32(b + 12);
        int total_chunks = rd_i32(b + 16), root = rd_i32(b + 20);
        if (dim != DIM) { fprintf(stderr, "dim=%d inesperado\n", dim); munmap(map, len); return -1; }
        size_t o = 24;
        ix->nodes = (const kdnode *)(b + o); o += (size_t)n_nodes * sizeof(kdnode);
        ix->labels = b + o;                  o += (size_t)n;
        ix->chunks = (const int16_t *)(b + o);
        ix->n = n; ix->is_kd = 1; ix->n_nodes = n_nodes; ix->total_chunks = total_chunks; ix->kd_root = root;
        int maxc = 0;
        for (int i = 0; i < n_nodes; i++) if (ix->nodes[i].leaf && ix->nodes[i].b > maxc) maxc = ix->nodes[i].b;
        ix->max_lanes = ((maxc + 7) / 8) * 8;
        return 0;
    }

    if (rd_u32(b) != MAGIC3) { fprintf(stderr, "magic invalido: %08x\n", rd_u32(b)); munmap(map, len); return -1; }
    int n = rd_i32(b + 4);
    int dim = rd_i32(b + 8);
    int k = rd_i32(b + 12);
    int total_chunks = rd_i32(b + 16);
    if (dim != DIM) { fprintf(stderr, "dim=%d inesperado\n", dim); munmap(map, len); return -1; }

    size_t o = 20;
    ix->centroids = (const float *)(b + o);      o += (size_t)k * DIM * 4;
    ix->aabb_min = (const int16_t *)(b + o);      o += (size_t)k * DIM * 2;
    ix->aabb_max = (const int16_t *)(b + o);      o += (size_t)k * DIM * 2;
    ix->offsets = (const int32_t *)(b + o);       o += (size_t)(k + 1) * 4;
    ix->chunk_offsets = (const int32_t *)(b + o); o += (size_t)(k + 1) * 4;
    ix->labels = b + o;                           o += (size_t)n;
    ix->chunks = (const int16_t *)(b + o);
    ix->n = n;
    ix->k = k;
    ix->total_chunks = total_chunks;
    ix->map = map;
    ix->map_len = len;

    int maxch = 0;
    for (int c = 0; c < k; c++) {
        int nc = ix->chunk_offsets[c + 1] - ix->chunk_offsets[c];
        if (nc > maxch) maxch = nc;
    }
    ix->max_lanes = maxch * 8;
    build_superclusters(ix);
    return 0;
}
