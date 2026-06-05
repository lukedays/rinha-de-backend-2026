#include "search.h"
#include "constants.h"
#include <immintrin.h>

// Kernel AVX2 8-lanes em assembly (distance_avx2.S).
extern void dist_chunks_i16(const int32_t *qpairs, const int16_t *chunks, long nchunks, int32_t *out);

// Instrumentacao opcional (bench), thread-local: sem corrida entre workers.
__thread long g_clusters_visited = 0;
__thread long g_vectors_scanned = 0;

#define MAX_K 4096

// mascara das 14 dims validas (words 14,15 = 0)
static const int16_t MASK14[16] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 0 };

// Lower bound exato (AABB) de um cluster, via AVX2:
//   e_d = max(0, max(min_d - q_d, q_d - max_d));  lb = sum_d e_d^2
static inline int32_t aabb_lb_avx2(const int16_t *mn, const int16_t *mx, __m256i vq, __m256i m14) {
    __m256i a = _mm256_sub_epi16(_mm256_loadu_si256((const __m256i *)mn), vq);
    __m256i b = _mm256_sub_epi16(vq, _mm256_loadu_si256((const __m256i *)mx));
    __m256i e = _mm256_max_epi16(_mm256_max_epi16(a, b), _mm256_setzero_si256());
    e = _mm256_and_si256(e, m14);
    __m256i sq = _mm256_madd_epi16(e, e); // 8x int32
    __m128i s = _mm_add_epi32(_mm256_castsi256_si128(sq), _mm256_extracti128_si256(sq, 1));
    s = _mm_hadd_epi32(s, s);
    s = _mm_hadd_epi32(s, s);
    return _mm_cvtsi128_si32(s);
}

int fraud_count(const index_t *ix, const int16_t q16[16], int32_t *scratch) {
    int K = ix->k;
    if (K > MAX_K) K = MAX_K;

    // query no layout pair-interleaved p/ o kernel: qpairs[p] = q[2p] | q[2p+1]<<16
    int32_t qpairs[7];
    for (int p = 0; p < 7; p++)
        qpairs[p] = (int32_t)((uint16_t)q16[2 * p] | ((uint32_t)(uint16_t)q16[2 * p + 1] << 16));

    int32_t lbv[MAX_K];
    __m256i vq = _mm256_loadu_si256((const __m256i *)q16);
    __m256i m14 = _mm256_loadu_si256((const __m256i *)MASK14);
    const int16_t *amn = ix->aabb_min, *amx = ix->aabb_max;
    for (int c = 0; c < K; c++)
        lbv[c] = aabb_lb_avx2(amn + (size_t)c * DIM, amx + (size_t)c * DIM, vq, m14);

    int32_t bd[5] = { INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX };
    uint8_t bl[5] = { 0, 0, 0, 0, 0 };
    int32_t worst = INT32_MAX;
    int worst_pos = 0;

    const int16_t *chunks = ix->chunks;
    const uint8_t *labels = ix->labels;
    long visited = 0, scanned = 0;

    // Selecao parcial: a cada passo visita o cluster de menor lower bound ainda
    // nao visitado; para quando esse lb >= worst (early-stop exato). Visita ~6
    // clusters, evitando ordenar os 1024.
    for (;;) {
        int32_t best_lb = INT32_MAX;
        int best_c = -1;
        for (int c = 0; c < K; c++) if (lbv[c] < best_lb) { best_lb = lbv[c]; best_c = c; }
        if (best_c < 0 || best_lb >= worst) break;

        // Poda da decisao (chromatic deferral exato): vizinhos com dist < best_lb
        // estao fixados (nenhum cluster restante os supera). Se >=3 fixados sao da
        // mesma classe, a decisao approved=(nf<3) ja esta determinada -> para.
        int fixed_fraud = 0, fixed_legit = 0;
        for (int j = 0; j < 5; j++)
            if (bd[j] < best_lb) { if (bl[j]) fixed_fraud++; else fixed_legit++; }
        if (fixed_fraud >= THRESHOLD_FRAUDS || fixed_legit > 5 - THRESHOLD_FRAUDS) break;

        lbv[best_c] = INT32_MAX; // marca visitado

        int vstart = ix->offsets[best_c];
        int size = ix->offsets[best_c + 1] - vstart;
        if (size <= 0) continue;
        int cstart = ix->chunk_offsets[best_c];
        int nch = ix->chunk_offsets[best_c + 1] - cstart;
        visited++; scanned += size;

        dist_chunks_i16(qpairs, chunks + (size_t)cstart * 112, nch, scratch);

        const uint8_t *lab = labels + vstart;
        for (int j = 0; j < size; j++) {
            int32_t d = scratch[j];
            if (d >= worst) continue;
            bd[worst_pos] = d;
            bl[worst_pos] = lab[j];
            worst = bd[0]; worst_pos = 0;
            for (int t = 1; t < 5; t++) if (bd[t] > worst) { worst = bd[t]; worst_pos = t; }
        }
    }

    g_clusters_visited = visited;
    g_vectors_scanned = scanned;
    return bl[0] + bl[1] + bl[2] + bl[3] + bl[4];
}
