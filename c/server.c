// Servidor HTTP/1.1: epoll thread-per-core. Recebe conexoes ja aceitas pelo LB
// via Unix socket (fd-passing, SCM_RIGHTS) — sem proxy HTTP no caminho. Fallback
// TCP (SO_REUSEPORT) quando RINHA_SOCKET nao e definido.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// epoll busy-poll (glibc < 2.37 nao traz a ABI). Faz o epoll_wait sondar a NAPI
// dos sockets antes de dormir -> latencia de wakeup muito menor. Por-epoll, sem
// exigir CAP_NET_ADMIN (ao contrario de SO_BUSY_POLL).
#ifndef EPIOCSPARAMS
struct epoll_params {
    uint32_t busy_poll_usecs;
    uint16_t busy_poll_budget;
    uint8_t prefer_busy_poll;
    uint8_t __pad;
};
#define EPIOCSPARAMS _IOW(0x8A, 0x01, struct epoll_params)
#endif

#include "constants.h"
#include "index.h"
#include "parser.h"
#include "search.h"
#include "vectorize.h"

#define BUFSZ 8192
#define MAX_EVENTS 256

static index_t g_ix;
static int g_port = 8080;
static int g_workers = 2;

static char post_resp[6][128];
static int post_len[6];
static char ready_resp[64];
static int ready_len;

static void build_responses(void) {
    static const char *bodies[6] = {
        "{\"approved\":true,\"fraud_score\":0.0}",
        "{\"approved\":true,\"fraud_score\":0.2}",
        "{\"approved\":true,\"fraud_score\":0.4}",
        "{\"approved\":false,\"fraud_score\":0.6}",
        "{\"approved\":false,\"fraud_score\":0.8}",
        "{\"approved\":false,\"fraud_score\":1.0}",
    };
    for (int i = 0; i < 6; i++) {
        int blen = (int)strlen(bodies[i]);
        post_len[i] = snprintf(post_resp[i], sizeof(post_resp[i]),
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n%s",
            blen, bodies[i]);
    }
    ready_len = snprintf(ready_resp, sizeof(ready_resp), "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n");
}

enum { T_CLIENT = 0, T_LISTEN = 1, T_CTRL = 2 };

typedef struct conn_t {
    int fd;
    int type;
    int rlen, wlen, wsent;
    struct conn_t *next; // free-list
    unsigned char rbuf[BUFSZ];
    unsigned char wbuf[BUFSZ];
} conn_t;

// Pool por worker (thread-local): reusa conn_t e evita calloc + page-faults por
// conexao no caminho quente, reduzindo a cauda de latencia.
static __thread conn_t *g_free = NULL;

static conn_t *new_conn(int fd, int type) {
    conn_t *c = g_free;
    if (c) g_free = c->next;
    else c = malloc(sizeof(conn_t));
    c->fd = fd; c->type = type; c->rlen = c->wlen = c->wsent = 0;
    return c;
}

static void release_conn(conn_t *c) {
    c->next = g_free;
    g_free = c;
}

// Pre-aloca e toca N conn_t (page-in antecipado) no startup do worker.
static void warm_pool(int n) {
    for (int i = 0; i < n; i++) {
        conn_t *c = malloc(sizeof(conn_t));
        if (!c) break;
        c->rbuf[0] = 0; c->wbuf[0] = 0; c->rbuf[BUFSZ - 1] = 0; c->wbuf[BUFSZ - 1] = 0;
        release_conn(c);
    }
}

static int content_length(const unsigned char *b, int hdr_end) {
    static const char *key = "content-length:";
    for (int i = 0; i + 15 <= hdr_end; i++) {
        int m = 1;
        for (int j = 0; j < 15; j++) {
            unsigned char ch = b[i + j];
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch != (unsigned char)key[j]) { m = 0; break; }
        }
        if (!m) continue;
        int k = i + 15;
        while (k < hdr_end && (b[k] == ' ' || b[k] == '\t')) k++;
        int v = 0;
        while (k < hdr_end && b[k] >= '0' && b[k] <= '9') v = v * 10 + (b[k++] - '0');
        return v;
    }
    return 0;
}

static void append(conn_t *c, const char *src, int len) {
    if (c->wlen + len > BUFSZ) return;
    memcpy(c->wbuf + c->wlen, src, len);
    c->wlen += len;
}

static void process(conn_t *c, const index_t *ix, int32_t *scratch) {
    int off = 0;
    while (off < c->rlen) {
        unsigned char *p = c->rbuf + off;
        int avail = c->rlen - off;
        unsigned char *he = memmem(p, avail, "\r\n\r\n", 4);
        if (!he) break;
        int hdr_end = (int)(he - p) + 4;
        if (p[0] == 'P') {
            int cl = content_length(p, hdr_end);
            int need = hdr_end + cl;
            if (avail < need) break;
            tx_input t;
            int nf = 0;
            if (parse_payload(p + hdr_end, cl, &t)) {
                float v[DIM];
                int16_t q[16];
                to_vector(&t, v);
                quantize(v, q);
                nf = fraud_count(ix, q, scratch);
            }
            append(c, post_resp[nf], post_len[nf]);
            off += need;
        } else {
            append(c, ready_resp, ready_len);
            off += hdr_end;
        }
    }
    if (off > 0) { memmove(c->rbuf, c->rbuf + off, c->rlen - off); c->rlen -= off; }
}

static void close_conn(int ep, conn_t *c) {
    epoll_ctl(ep, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    release_conn(c);
}

static int flush(conn_t *c) {
    while (c->wsent < c->wlen) {
        ssize_t n = send(c->fd, c->wbuf + c->wsent, c->wlen - c->wsent, MSG_NOSIGNAL);
        if (n > 0) { c->wsent += (int)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        return -1;
    }
    c->wlen = c->wsent = 0;
    return 0;
}

static void set_out(int ep, conn_t *c, int want_out) {
    struct epoll_event ev = { .events = EPOLLIN | (want_out ? EPOLLOUT : 0), .data.ptr = c };
    epoll_ctl(ep, EPOLL_CTL_MOD, c->fd, &ev);
}

// Recebe um fd de conexao via SCM_RIGHTS. Retorna fd>=0, -1 EAGAIN, -2 fechado.
static int recv_fd(int ctrl) {
    char b[1];
    struct iovec io = { .iov_base = b, .iov_len = 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof(u.buf) };
    ssize_t n = recvmsg(ctrl, &m, 0);
    if (n == 0) return -2;
    if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? -1 : -2;
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
        int fd; memcpy(&fd, CMSG_DATA(c), sizeof(int));
        return fd;
    }
    return -1;
}

static int g_busy_poll_us = 0;
static int g_epoll_idle_us = 0; // keep-warm: spin com epoll timeout=0 por ate Nus apos o ultimo evento

static inline uint64_t now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void enable_busy_poll(int ep) {
    if (g_busy_poll_us <= 0) return;
    struct epoll_params p = { .busy_poll_usecs = (uint32_t)g_busy_poll_us, .busy_poll_budget = 64, .prefer_busy_poll = 1 };
    ioctl(ep, EPIOCSPARAMS, &p);
}

static void add_client(int ep, int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
    if (g_busy_poll_us > 0)
        setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &g_busy_poll_us, sizeof(g_busy_poll_us));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
    conn_t *c = new_conn(fd, T_CLIENT);
    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
    epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev);
}

static void handle_client(int ep, conn_t *c, uint32_t e, const index_t *ix, int32_t *scratch) {
    if (e & (EPOLLHUP | EPOLLERR)) { close_conn(ep, c); return; }
    if (e & EPOLLOUT) {
        int r = flush(c);
        if (r < 0) { close_conn(ep, c); return; }
        if (r == 0) set_out(ep, c, 0);
    }
    if (e & EPOLLIN) {
        int closed = 0;
        for (;;) {
            if (c->rlen >= BUFSZ) { closed = 1; break; }
            ssize_t r = recv(c->fd, c->rbuf + c->rlen, BUFSZ - c->rlen, 0);
            if (r > 0) { c->rlen += (int)r; continue; }
            if (r == 0) { closed = 1; break; }
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            closed = 1; break;
        }
        process(c, ix, scratch);
        int pend = flush(c);
        if (pend < 0) { close_conn(ep, c); return; }
        if (pend > 0) set_out(ep, c, 1);
        if (closed && c->wlen == 0) { close_conn(ep, c); return; }
    }
}

// epoll_wait com keep-warm: enquanto houve evento ha menos de idle_us, faz poll
// nao-bloqueante (timeout=0) para pegar o proximo request sem latencia de wakeup
// (independe de busy-poll da NIC); se ocioso, bloqueia (economiza a quota de CPU).
static int kw_wait(int ep, struct epoll_event *events, uint64_t *last) {
    if (g_epoll_idle_us <= 0) return epoll_wait(ep, events, MAX_EVENTS, -1);
    uint64_t idle_ns = (uint64_t)g_epoll_idle_us * 1000;
    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, 0);
        if (n != 0) { *last = now_ns(); return n; }
        if (now_ns() - *last >= idle_ns) {
            n = epoll_wait(ep, events, MAX_EVENTS, -1);  // ocioso: bloqueia (economiza CPU)
            *last = now_ns();
            return n;
        }
        // Pausa curta em userspace entre polls: reduz a taxa de syscall ~100x
        // (nao martela o kernel) mantendo granularidade de wakeup de poucos us.
        for (int s = 0; s < 96; s++) __builtin_ia32_pause();
    }
}

// Worker do modo Unix: aceita a conexao de controle do LB e recebe fds prontos.
static void *unix_worker(void *arg) {
    const char *path = (const char *)arg;
    unlink(path);
    int lfd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
    if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) < 0) { perror("bind unix"); return NULL; }
    listen(lfd, 64);

    int ep = epoll_create1(0);
    enable_busy_poll(ep);
    conn_t lc = { .fd = lfd, .type = T_LISTEN };
    struct epoll_event lev = { .events = EPOLLIN, .data.ptr = &lc };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &lev);

    int32_t *scratch = malloc((size_t)g_ix.max_lanes * sizeof(int32_t));
    warm_pool(512);
    prctl(PR_SET_TIMERSLACK, 1UL); // slack minimo: wakeups mais precisos
    struct epoll_event events[MAX_EVENTS];
    uint64_t last_event = now_ns();

    for (;;) {
        int n = kw_wait(ep, events, &last_event);
        for (int i = 0; i < n; i++) {
            conn_t *c = (conn_t *)events[i].data.ptr;
            if (c->type == T_LISTEN) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    conn_t *cc = new_conn(cfd, T_CTRL);
                    struct epoll_event ev = { .events = EPOLLIN, .data.ptr = cc };
                    epoll_ctl(ep, EPOLL_CTL_ADD, cfd, &ev);
                }
            } else if (c->type == T_CTRL) {
                for (;;) {
                    int fd = recv_fd(c->fd);
                    if (fd == -1) break;          // EAGAIN
                    if (fd == -2) { close_conn(ep, c); break; } // LB fechou
                    add_client(ep, fd);
                }
            } else {
                handle_client(ep, c, events[i].events, &g_ix, scratch);
            }
        }
    }
    return NULL;
}

// Worker do modo TCP (fallback): SO_REUSEPORT.
static void *tcp_worker(void *arg) {
    (void)arg;
    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(lfd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(g_port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return NULL; }
    listen(lfd, 1024);

    int ep = epoll_create1(0);
    enable_busy_poll(ep);
    conn_t lc = { .fd = lfd, .type = T_LISTEN };
    struct epoll_event lev = { .events = EPOLLIN, .data.ptr = &lc };
    epoll_ctl(ep, EPOLL_CTL_ADD, lfd, &lev);

    int32_t *scratch = malloc((size_t)g_ix.max_lanes * sizeof(int32_t));
    warm_pool(512);
    prctl(PR_SET_TIMERSLACK, 1UL); // slack minimo: wakeups mais precisos
    struct epoll_event events[MAX_EVENTS];
    for (;;) {
        int n = epoll_wait(ep, events, MAX_EVENTS, -1);
        for (int i = 0; i < n; i++) {
            conn_t *c = (conn_t *)events[i].data.ptr;
            if (c->type == T_LISTEN) {
                for (;;) {
                    int cfd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
                    if (cfd < 0) break;
                    add_client(ep, cfd);
                }
            } else {
                handle_client(ep, c, events[i].events, &g_ix, scratch);
            }
        }
    }
    return NULL;
}

static int env_int(const char *name, int def) {
    const char *v = getenv(name);
    return v ? atoi(v) : def;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *idx = getenv("INDEX_PATH");
    if (!idx) idx = "/app/index.bin";
    const char *sock = getenv("RINHA_SOCKET");
    g_port = env_int("PORT", 8080);
    g_workers = env_int("WORKERS", 2);
    g_busy_poll_us = env_int("RINHA_C_BUSY_POLL_US", 0);
    g_epoll_idle_us = env_int("RINHA_C_EPOLL_IDLE_US", 0);
    if (g_workers > 64) g_workers = 64;

    if (index_load(idx, &g_ix) != 0) return 1;
    fprintf(stderr, "indice: N=%d K=%d chunks=%d (workers=%d, mode=%s)\n",
            g_ix.n, g_ix.k, g_ix.total_chunks, g_workers, sock ? "unix" : "tcp");
    build_responses();

    pthread_t th[64];
    if (sock) {
        // um control socket por worker: "<sock>.<w>"
        static char paths[64][128];
        for (int i = 0; i < g_workers; i++) snprintf(paths[i], sizeof(paths[i]), "%s.%d", sock, i);
        for (int i = 1; i < g_workers; i++) pthread_create(&th[i], NULL, unix_worker, paths[i]);
        unix_worker(paths[0]);
    } else {
        for (int i = 1; i < g_workers; i++) pthread_create(&th[i], NULL, tcp_worker, NULL);
        tcp_worker(NULL);
    }
    return 0;
}
