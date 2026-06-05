#ifndef RINHA_SEARCH_H
#define RINHA_SEARCH_H

#include <stdint.h>
#include "index.h"

// k-NN k=5 EXATO via IVF com poda por AABB. Retorna quantos dos 5 vizinhos mais
// proximos sao fraude (0..5). scratch: int32 com >= ix->max_cluster elementos.
int fraud_count(const index_t *ix, const int16_t q16[16], int32_t *scratch);

#endif
