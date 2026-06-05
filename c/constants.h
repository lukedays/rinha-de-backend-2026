// Constantes de normalizacao (normalization.json) e risco por MCC (mcc_risk.json).
#ifndef RINHA_CONSTANTS_H
#define RINHA_CONSTANTS_H

#include <stddef.h>
#include <string.h>

#include <stdint.h>
#include <math.h>

#define DIM 14
#define KNN 5
#define THRESHOLD_FRAUDS 3 // approved = nf < 3  (fraud_score < 0.6)

// Quantizacao int16: v in [-1,1] -> round(v*10000). Lossless para os refs de 4
// casas decimais; distancia ao quadrado cabe em int32 (worst-case ~2e9).
#define Q_SCALE 10000.0f

static inline int16_t quant_i16(float v) {
    int x = (int)lrintf(v * Q_SCALE);
    if (x < -32768) x = -32768;
    if (x > 32767) x = 32767;
    return (int16_t)x;
}

#define MAX_AMOUNT 10000.0
#define MAX_INSTALLMENTS 12.0
#define AMOUNT_VS_AVG_RATIO 10.0
#define MAX_MINUTES 1440.0
#define MAX_KM 1000.0
#define MAX_TX_COUNT_24H 20.0
#define MAX_MERCHANT_AVG_AMOUNT 10000.0
#define DEFAULT_MCC_RISK 0.5

// Risco por MCC; 0.5 quando ausente. mcc tem comprimento len (sem terminador).
static inline double mcc_risk(const unsigned char *mcc, size_t len) {
    if (len != 4) return DEFAULT_MCC_RISK;
    if (!memcmp(mcc, "5411", 4)) return 0.15;
    if (!memcmp(mcc, "5812", 4)) return 0.30;
    if (!memcmp(mcc, "5912", 4)) return 0.20;
    if (!memcmp(mcc, "5944", 4)) return 0.45;
    if (!memcmp(mcc, "7801", 4)) return 0.80;
    if (!memcmp(mcc, "7802", 4)) return 0.75;
    if (!memcmp(mcc, "7995", 4)) return 0.85;
    if (!memcmp(mcc, "4511", 4)) return 0.35;
    if (!memcmp(mcc, "5311", 4)) return 0.25;
    if (!memcmp(mcc, "5999", 4)) return 0.50;
    return DEFAULT_MCC_RISK;
}

#endif
