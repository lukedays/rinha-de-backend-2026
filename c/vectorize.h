// Vetorizacao das 14 dimensoes + quantizacao. Paridade exata com a versao validada.
#ifndef RINHA_VECTORIZE_H
#define RINHA_VECTORIZE_H

#include <math.h>
#include <stdint.h>
#include "constants.h"

typedef struct {
    double amount;
    int installments;
    int hour;
    int dow_mon0;
    double cust_avg;
    int tx_count_24h;
    int unknown_merchant;
    double mcc_risk;
    double merch_avg;
    int is_online;
    int card_present;
    double km_from_home;
    int has_last;
    double minutes_since_last;
    double km_from_last;
} tx_input;

static inline float clamp01(double x) {
    return x <= 0.0 ? 0.0f : (x >= 1.0 ? 1.0f : (float)x);
}

static inline void to_vector(const tx_input *t, float v[DIM]) {
    v[0] = clamp01(t->amount / MAX_AMOUNT);
    v[1] = clamp01(t->installments / MAX_INSTALLMENTS);
    double ratio = t->cust_avg > 0.0 ? t->amount / t->cust_avg : INFINITY;
    v[2] = clamp01(ratio / AMOUNT_VS_AVG_RATIO);
    v[3] = (float)(t->hour / 23.0);
    v[4] = (float)(t->dow_mon0 / 6.0);
    v[5] = t->has_last ? clamp01(t->minutes_since_last / MAX_MINUTES) : -1.0f;
    v[6] = t->has_last ? clamp01(t->km_from_last / MAX_KM) : -1.0f;
    v[7] = clamp01(t->km_from_home / MAX_KM);
    v[8] = clamp01(t->tx_count_24h / MAX_TX_COUNT_24H);
    v[9] = t->is_online ? 1.0f : 0.0f;
    v[10] = t->card_present ? 1.0f : 0.0f;
    v[11] = t->unknown_merchant ? 1.0f : 0.0f;
    v[12] = (float)t->mcc_risk;
    v[13] = clamp01(t->merch_avg / MAX_MERCHANT_AVG_AMOUNT);
}

// Quantiza para int16 escala 10000; words 14,15 sao padding (mascarados no SIMD).
static inline void quantize(const float v[DIM], int16_t q[16]) {
    for (int i = 0; i < DIM; i++) q[i] = quant_i16(v[i]);
    q[14] = 0;
    q[15] = 0;
}

#endif
