#include "index.h"
#include "constants.h"
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

// "RNH3" (bytes 0x52,0x4E,0x48,0x33) => 0x33484E52 como u32 little-endian.
#define MAGIC3 0x33484E52u

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
    return 0;
}
