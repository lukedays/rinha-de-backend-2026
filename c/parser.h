#ifndef RINHA_PARSER_H
#define RINHA_PARSER_H

#include <stddef.h>
#include <stdint.h>
#include "vectorize.h"

// Parseia o corpo de POST /fraud-score. Retorna 1 em sucesso, 0 em falha.
int parse_payload(const uint8_t *json, size_t len, tx_input *out);

#endif
