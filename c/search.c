#include "search.h"
#include "constants.h"
#include <immintrin.h>

// Kernel AVX2 8-lanes em assembly (distance_avx2.S).
extern void dist_chunks_i16(const int32_t *qpairs, const int16_t *chunks, long nchunks, int32_t *out);

// Instrumentacao opcional (bench), thread-local: sem corrida entre workers.
__thread long g_clusters_visited = 0;
__thread long g_vectors_scanned = 0;

#define MAX_K 16384
#define MAX_SUPER 256

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

// Carrega qpairs (layout pair-interleaved p/ o kernel) a partir da query.
static inline void load_qpairs(const int16_t q16[16], int32_t qpairs[7]) {
    for (int p = 0; p < 7; p++)
        qpairs[p] = (int32_t)((uint16_t)q16[2 * p] | ((uint32_t)(uint16_t)q16[2 * p + 1] << 16));
}

// Busca 1 nivel (referencia exata): calcula o LB de todos os K clusters e visita
// em ordem de LB com early-stop + chromatic deferral. Usada como fallback e como
// gabarito de exatidao p/ a versao de 2 niveis.
static int search_single(const index_t *ix, const int16_t q16[16], int32_t *scratch) {
    int K = ix->k;
    if (K > MAX_K) K = MAX_K;

    int32_t qpairs[7];
    load_qpairs(q16, qpairs);

    int32_t lbv[MAX_K];
    __m256i vq = _mm256_loadu_si256((const __m256i *)q16);
    __m256i m14 = _mm256_loadu_si256((const __m256i *)MASK14);
    const int16_t *amn = ix->aabb_min, *amx = ix->aabb_max;

    // Passe unico: calcula o LB (AABB) de cada cluster E mantem os M menores ja
    // ordenados por LB ascendente (insertion). Substitui a selecao antiga, que
    // re-varria os K clusters a cada visita (O(visitas*K)) -> destrava K grande.
    enum { M = 64 };
    int     cand[M];
    int32_t candlb[M];
    int ncand = 0;
    int32_t cap = INT32_MAX; // maior LB ainda aceito na lista dos M menores
    for (int c = 0; c < K; c++) {
        int32_t lb = aabb_lb_avx2(amn + (size_t)c * DIM, amx + (size_t)c * DIM, vq, m14);
        lbv[c] = lb;
        if (lb >= cap) continue;
        int pos = ncand < M ? ncand : M - 1;
        if (ncand < M) ncand++;
        while (pos > 0 && candlb[pos - 1] > lb) { candlb[pos] = candlb[pos - 1]; cand[pos] = cand[pos - 1]; pos--; }
        candlb[pos] = lb; cand[pos] = c;
        if (ncand == M) cap = candlb[M - 1];
    }

    int32_t bd[5] = { INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX };
    uint8_t bl[5] = { 0, 0, 0, 0, 0 };
    int32_t worst = INT32_MAX;
    int worst_pos = 0;

    const int16_t *chunks = ix->chunks;
    const uint8_t *labels = ix->labels;
    long visited = 0, scanned = 0;

    // Visita os clusters em ordem de LB crescente: primeiro a lista dos M menores
    // (caso comum, ~8 visitas), e so cai no fallback O(K) se ela esgotar sem o
    // early-stop disparar (raro). Para quando lb >= worst (exato).
    int idx = 0;
    for (;;) {
        int32_t best_lb;
        int best_c;
        if (idx < ncand) {
            best_c = cand[idx]; best_lb = candlb[idx]; idx++;
        } else {
            best_lb = INT32_MAX; best_c = -1;
            for (int c = 0; c < K; c++) if (lbv[c] < best_lb) { best_lb = lbv[c]; best_c = c; }
            if (best_c < 0) break;
        }
        if (best_lb >= worst) break;

        // Poda da decisao (chromatic deferral exato): vizinhos com dist < best_lb
        // estao fixados (nenhum cluster restante os supera). Se >=3 fixados sao da
        // mesma classe, a decisao approved=(nf<3) ja esta determinada -> para.
        int fixed_fraud = 0, fixed_legit = 0;
        for (int j = 0; j < 5; j++)
            if (bd[j] < best_lb) { if (bl[j]) fixed_fraud++; else fixed_legit++; }
        if (fixed_fraud >= THRESHOLD_FRAUDS || fixed_legit > 5 - THRESHOLD_FRAUDS) break;

        lbv[best_c] = INT32_MAX; // marca visitado (consistencia com o fallback)

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

// Busca 2 niveis: poda super-clusters inteiros pelo AABB-uniao (lower bound
// admissivel p/ todos os membros) antes de tocar os AABBs dos clusters. Visita
// supers em ordem de LB; expande so os com lb < worst; dentro de cada um, visita
// os membros (clusters) em ordem de LB. Mesmos early-stops exatos do 1 nivel.
static int search_two(const index_t *ix, const int16_t q16[16], int32_t *scratch) {
    int S = ix->n_super;

    int32_t qpairs[7];
    load_qpairs(q16, qpairs);
    __m256i vq = _mm256_loadu_si256((const __m256i *)q16);
    __m256i m14 = _mm256_loadu_si256((const __m256i *)MASK14);

    // LB de cada super + ordem ascendente (S ~= sqrt(k), pequeno).
    int32_t slb[MAX_SUPER];
    int sord[MAX_SUPER];
    const int16_t *smn = ix->super_min, *smx = ix->super_max;
    for (int s = 0; s < S; s++) {
        slb[s] = aabb_lb_avx2(smn + (size_t)s * 16, smx + (size_t)s * 16, vq, m14);
        sord[s] = s;
    }
    for (int i = 1; i < S; i++) {
        int x = sord[i]; int32_t xl = slb[x]; int j = i - 1;
        while (j >= 0 && slb[sord[j]] > xl) { sord[j + 1] = sord[j]; j--; }
        sord[j + 1] = x;
    }

    int32_t bd[5] = { INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX };
    uint8_t bl[5] = { 0, 0, 0, 0, 0 };
    int32_t worst = INT32_MAX;
    int worst_pos = 0;

    const int16_t *amn = ix->aabb_min, *amx = ix->aabb_max;
    const int16_t *chunks = ix->chunks;
    const uint8_t *labels = ix->labels;
    const int32_t *soff = ix->super_off, *smem = ix->super_mem;
    long visited = 0, scanned = 0;

    // Membros do super ordenados por LB (thread-local p/ nao estourar a stack).
    static __thread int     tmid[MAX_K];
    static __thread int32_t  tmlb[MAX_K];
    int *mids = tmid; int32_t *mlb = tmlb;

    for (int si = 0; si < S; si++) {
        int s = sord[si];
        if (slb[s] >= worst) break;                 // poda super (e todos os seguintes): exato
        int ff = 0, fl = 0;
        for (int j = 0; j < 5; j++)
            if (bd[j] < slb[s]) { if (bl[j]) ff++; else fl++; }
        if (ff >= THRESHOLD_FRAUDS || fl > 5 - THRESHOLD_FRAUDS) break;

        // expande: LB dos clusters-membros, ordenados asc (descarta lb >= worst)
        int lo = soff[s], hi = soff[s + 1], cn = 0;
        for (int m = lo; m < hi; m++) {
            int c = smem[m];
            int32_t lb = aabb_lb_avx2(amn + (size_t)c * DIM, amx + (size_t)c * DIM, vq, m14);
            if (lb >= worst) continue;
            int pos = cn++;
            while (pos > 0 && mlb[pos - 1] > lb) { mlb[pos] = mlb[pos - 1]; mids[pos] = mids[pos - 1]; pos--; }
            mlb[pos] = lb; mids[pos] = c;
        }
        // LB do proximo super (sorted): junto com o membro atual, da a fronteira =
        // menor LB entre TODOS os clusters nao-visitados -> chromatic exato.
        int32_t next_super_lb = (si + 1 < S) ? slb[sord[si + 1]] : INT32_MAX;
        for (int i = 0; i < cn; i++) {
            int32_t best_lb = mlb[i];
            int best_c = mids[i];
            if (best_lb >= worst) break;

            // Chromatic deferral exato: vizinhos com dist < fronteira estao fixados
            // (todo cluster nao-visitado tem LB >= fronteira). Se a decisao ja esta
            // determinada, para tudo.
            int32_t frontier = best_lb < next_super_lb ? best_lb : next_super_lb;
            int ff2 = 0, fl2 = 0;
            for (int j = 0; j < 5; j++)
                if (bd[j] < frontier) { if (bl[j]) ff2++; else fl2++; }
            if (ff2 >= THRESHOLD_FRAUDS || fl2 > 5 - THRESHOLD_FRAUDS) goto finish;

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
    }

finish:
    g_clusters_visited = visited;
    g_vectors_scanned = scanned;
    return bl[0] + bl[1] + bl[2] + bl[3] + bl[4];
}

// ===================== KD-tree (reescrita da abordagem do #1 em C+ASM) =====================
// Traversal exato com poda por plano de corte: na subarvore "longe" todo ponto
// tem coord[dim] do lado oposto ao split, logo dist^2 >= (q[dim]-split)^2; se isso
// >= worst, poda. Leaves escaneadas com o mesmo kernel AVX2 8-lanes (dist_chunks_i16).
typedef struct {
    const index_t *ix;
    const int32_t *qpairs;
    const int16_t *q16;
    int32_t *scratch;
    __m256i vq, m14;
    int32_t bd[5];
    uint8_t bl[5];
    int32_t worst;
    int worst_pos;
    long visited, scanned;
} kdctx;

static void kd_leaf(kdctx *s, const kdnode *nd) {
    int sz = nd->b, nch = (sz + 7) / 8;
    dist_chunks_i16(s->qpairs, s->ix->chunks + (size_t)nd->a * 112, nch, s->scratch);
    const uint8_t *lab = s->ix->labels + nd->c;
    s->visited++; s->scanned += sz;
    int32_t worst = s->worst, *bd = s->bd; int wp = s->worst_pos; uint8_t *bl = s->bl;
    for (int j = 0; j < sz; j++) {
        int32_t d = s->scratch[j];
        if (d >= worst) continue;
        bd[wp] = d; bl[wp] = lab[j];
        worst = bd[0]; wp = 0;
        for (int t = 1; t < 5; t++) if (bd[t] > worst) { worst = bd[t]; wp = t; }
    }
    s->worst = worst; s->worst_pos = wp;
}

static void kd_recurse(kdctx *s, int node) {
    const kdnode *nd = &s->ix->nodes[node];
    // Poda por bounding-box: o AABB-LB do subtree e um lower bound admissivel p/
    // todos os seus vetores. Se >= worst, nenhum pode entrar no top-5 -> poda tudo.
    if (aabb_lb_avx2(nd->bmin, nd->bmax, s->vq, s->m14) >= s->worst) return;
    if (nd->leaf) { kd_leaf(s, nd); return; }
    int diff = (int)s->q16[nd->dim] - (int)nd->split;
    int near, far;
    if (diff <= 0) { near = nd->a; far = nd->b; } else { near = nd->b; far = nd->a; }
    kd_recurse(s, near);   // mais proximo primeiro (worst encolhe), depois o outro
    kd_recurse(s, far);
}

static int search_kd(const index_t *ix, const int16_t q16[16], int32_t *scratch) {
    int32_t qpairs[7];
    load_qpairs(q16, qpairs);
    kdctx s = { .ix = ix, .qpairs = qpairs, .q16 = q16, .scratch = scratch,
                .vq = _mm256_loadu_si256((const __m256i *)q16),
                .m14 = _mm256_loadu_si256((const __m256i *)MASK14),
                .bd = { INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX },
                .bl = { 0, 0, 0, 0, 0 }, .worst = INT32_MAX, .worst_pos = 0, .visited = 0, .scanned = 0 };
    kd_recurse(&s, ix->kd_root);
    g_clusters_visited = s.visited; g_vectors_scanned = s.scanned;
    return s.bl[0] + s.bl[1] + s.bl[2] + s.bl[3] + s.bl[4];
}

// Producao: KD-tree se o indice for "RNH4"; senao IVF (2 niveis, ou 1 nivel).
int fraud_count(const index_t *ix, const int16_t q16[16], int32_t *scratch) {
    if (ix->is_kd) return search_kd(ix, q16, scratch);
    if (ix->n_super > 0 && ix->n_super <= MAX_SUPER)
        return search_two(ix, q16, scratch);
    return search_single(ix, q16, scratch);
}

// Referencia exata (1 nivel), exposta p/ validacao de paridade.
int fraud_count_single(const index_t *ix, const int16_t q16[16], int32_t *scratch) {
    return search_single(ix, q16, scratch);
}
