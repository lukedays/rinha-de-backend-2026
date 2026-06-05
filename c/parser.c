#include "parser.h"
#include "constants.h"
#include "timestamp.h"
#include <stdlib.h>

typedef struct {
    const uint8_t *b;
    size_t n;
    size_t i;
} cur;

static inline void ws(cur *c) {
    while (c->i < c->n) {
        uint8_t ch = c->b[c->i];
        if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') c->i++;
        else break;
    }
}
static inline uint8_t peek(cur *c) {
    ws(c);
    return c->i < c->n ? c->b[c->i] : 0;
}
static inline int eat(cur *c, uint8_t ch) {
    if (peek(c) == ch) { c->i++; return 1; }
    return 0;
}
// Le string JSON (sem escapes). Retorna ponteiro/len do conteudo; avanca o cursor.
static const uint8_t *jstr(cur *c, size_t *len) {
    if (peek(c) != '"') return NULL;
    c->i++;
    size_t start = c->i;
    while (c->i < c->n && c->b[c->i] != '"') c->i++;
    *len = c->i - start;
    const uint8_t *p = c->b + start;
    if (c->i < c->n) c->i++; // aspa final
    return p;
}
static double jnum(cur *c) {
    ws(c);
    const char *p = (const char *)(c->b + c->i);
    char *end;
    double v = strtod(p, &end);
    c->i += (size_t)(end - p);
    return v;
}
static int jbool(cur *c) {
    int t = peek(c) == 't';
    while (c->i < c->n) {
        uint8_t ch = c->b[c->i];
        if (ch == ',' || ch == '}' || ch == ']') break;
        c->i++;
    }
    return t;
}
static void skip_value(cur *c);

static void skip_obj_or_arr(cur *c, uint8_t open, uint8_t close) {
    c->i++; // consome open
    for (;;) {
        if (eat(c, close)) return;
        if (open == '{') { size_t l; jstr(c, &l); eat(c, ':'); }
        skip_value(c);
        eat(c, ',');
        if (peek(c) == close) { c->i++; return; }
        if (peek(c) == 0) return;
    }
}
static void skip_value(cur *c) {
    uint8_t ch = peek(c);
    if (ch == '"') { size_t l; jstr(c, &l); }
    else if (ch == '{') skip_obj_or_arr(c, '{', '}');
    else if (ch == '[') skip_obj_or_arr(c, '[', ']');
    else {
        while (c->i < c->n) {
            uint8_t x = c->b[c->i];
            if (x == ',' || x == '}' || x == ']') break;
            c->i++;
        }
    }
}

#define KEY_IS(p, l, s) ((l) == sizeof(s) - 1 && !memcmp((p), (s), (l)))

static void parse_transaction(cur *c, tx_input *t, int64_t *req_epoch) {
    if (peek(c) != '{') { skip_value(c); return; }
    c->i++;
    for (;;) {
        if (eat(c, '}')) break;
        size_t kl; const uint8_t *k = jstr(c, &kl); eat(c, ':');
        if (!k) break;
        if (KEY_IS(k, kl, "amount")) t->amount = jnum(c);
        else if (KEY_IS(k, kl, "installments")) t->installments = (int)jnum(c);
        else if (KEY_IS(k, kl, "requested_at")) {
            size_t sl; const uint8_t *s = jstr(c, &sl);
            if (s && sl >= 19) { time_parts tp = ts_parse(s); *req_epoch = tp.epoch; t->hour = tp.hour; t->dow_mon0 = tp.dow_mon0; }
        } else skip_value(c);
        eat(c, ',');
        if (peek(c) == '}') { c->i++; break; }
        if (peek(c) == 0) break;
    }
}

static void parse_customer(cur *c, tx_input *t, size_t *km_off, size_t *km_len, int *km_n) {
    if (peek(c) != '{') { skip_value(c); return; }
    c->i++;
    for (;;) {
        if (eat(c, '}')) break;
        size_t kl; const uint8_t *k = jstr(c, &kl); eat(c, ':');
        if (!k) break;
        if (KEY_IS(k, kl, "avg_amount")) t->cust_avg = jnum(c);
        else if (KEY_IS(k, kl, "tx_count_24h")) t->tx_count_24h = (int)jnum(c);
        else if (KEY_IS(k, kl, "known_merchants")) {
            if (peek(c) == '[') {
                c->i++;
                for (;;) {
                    if (eat(c, ']')) break;
                    size_t sl; const uint8_t *s = jstr(c, &sl);
                    if (s && *km_n < 32) { km_off[*km_n] = (size_t)(s - c->b); km_len[*km_n] = sl; (*km_n)++; }
                    eat(c, ',');
                    if (peek(c) == ']') { c->i++; break; }
                    if (peek(c) == 0) break;
                }
            } else skip_value(c);
        } else skip_value(c);
        eat(c, ',');
        if (peek(c) == '}') { c->i++; break; }
        if (peek(c) == 0) break;
    }
}

static void parse_merchant(cur *c, tx_input *t, size_t *mid_off, size_t *mid_len) {
    if (peek(c) != '{') { skip_value(c); return; }
    c->i++;
    for (;;) {
        if (eat(c, '}')) break;
        size_t kl; const uint8_t *k = jstr(c, &kl); eat(c, ':');
        if (!k) break;
        if (KEY_IS(k, kl, "avg_amount")) t->merch_avg = jnum(c);
        else if (KEY_IS(k, kl, "mcc")) { size_t sl; const uint8_t *s = jstr(c, &sl); if (s) t->mcc_risk = mcc_risk(s, sl); }
        else if (KEY_IS(k, kl, "id")) { size_t sl; const uint8_t *s = jstr(c, &sl); if (s) { *mid_off = (size_t)(s - c->b); *mid_len = sl; } }
        else skip_value(c);
        eat(c, ',');
        if (peek(c) == '}') { c->i++; break; }
        if (peek(c) == 0) break;
    }
}

static void parse_terminal(cur *c, tx_input *t) {
    if (peek(c) != '{') { skip_value(c); return; }
    c->i++;
    for (;;) {
        if (eat(c, '}')) break;
        size_t kl; const uint8_t *k = jstr(c, &kl); eat(c, ':');
        if (!k) break;
        if (KEY_IS(k, kl, "is_online")) t->is_online = jbool(c);
        else if (KEY_IS(k, kl, "card_present")) t->card_present = jbool(c);
        else if (KEY_IS(k, kl, "km_from_home")) t->km_from_home = jnum(c);
        else skip_value(c);
        eat(c, ',');
        if (peek(c) == '}') { c->i++; break; }
        if (peek(c) == 0) break;
    }
}

static void parse_last(cur *c, tx_input *t, int64_t *last_epoch) {
    if (peek(c) == 'n') { skip_value(c); return; } // null
    if (peek(c) != '{') { skip_value(c); return; }
    t->has_last = 1;
    c->i++;
    for (;;) {
        if (eat(c, '}')) break;
        size_t kl; const uint8_t *k = jstr(c, &kl); eat(c, ':');
        if (!k) break;
        if (KEY_IS(k, kl, "timestamp")) { size_t sl; const uint8_t *s = jstr(c, &sl); if (s && sl >= 19) *last_epoch = ts_parse(s).epoch; }
        else if (KEY_IS(k, kl, "km_from_current")) t->km_from_last = jnum(c);
        else skip_value(c);
        eat(c, ',');
        if (peek(c) == '}') { c->i++; break; }
        if (peek(c) == 0) break;
    }
}

int parse_payload(const uint8_t *json, size_t len, tx_input *out) {
    cur c = { json, len, 0 };
    tx_input t = {0};
    int64_t req_epoch = 0, last_epoch = 0;
    size_t km_off[32], km_len[32];
    int km_n = 0;
    size_t mid_off = 0, mid_len = 0;
    int has_mid = 0;

    if (peek(&c) != '{') return 0;
    c.i++;
    for (;;) {
        if (eat(&c, '}')) break;
        size_t kl; const uint8_t *k = jstr(&c, &kl); eat(&c, ':');
        if (!k) break;
        if (KEY_IS(k, kl, "transaction")) parse_transaction(&c, &t, &req_epoch);
        else if (KEY_IS(k, kl, "customer")) parse_customer(&c, &t, km_off, km_len, &km_n);
        else if (KEY_IS(k, kl, "merchant")) { parse_merchant(&c, &t, &mid_off, &mid_len); has_mid = mid_len > 0; }
        else if (KEY_IS(k, kl, "terminal")) parse_terminal(&c, &t);
        else if (KEY_IS(k, kl, "last_transaction")) parse_last(&c, &t, &last_epoch);
        else skip_value(&c);
        eat(&c, ',');
        if (peek(&c) == '}') { c.i++; break; }
        if (peek(&c) == 0) break;
    }

    t.minutes_since_last = t.has_last ? (double)(req_epoch - last_epoch) / 60.0 : 0.0;

    // unknown_merchant: merchant.id nao esta em known_merchants
    t.unknown_merchant = 1;
    if (has_mid) {
        const uint8_t *id = json + mid_off;
        for (int i = 0; i < km_n; i++) {
            if (km_len[i] == mid_len && !memcmp(json + km_off[i], id, mid_len)) { t.unknown_merchant = 0; break; }
        }
    }

    *out = t;
    return 1;
}
