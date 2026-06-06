#ifndef RINHA_SEARCH_H
#define RINHA_SEARCH_H

#include <stdint.h>
#include "index.h"

// k-NN k=5 EXATO via IVF com poda por AABB. Retorna quantos dos 5 vizinhos mais
// proximos sao fraude (0..5). scratch: int32 com >= ix->max_cluster elementos.
int fraud_count(const index_t *ix, const int16_t q16[16], int32_t *scratch);

// Referencia 1 nivel (sem o filtro grosseiro), exposta p/ validar paridade exata
// da versao de producao (2 niveis) contra ela.
int fraud_count_single(const index_t *ix, const int16_t q16[16], int32_t *scratch);

#endif
