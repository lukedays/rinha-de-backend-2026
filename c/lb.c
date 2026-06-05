// Load balancer: aceita TCP na porta 9999 e distribui as conexoes (round-robin)
// para as instancias da API passando o file descriptor via Unix socket
// (SCM_RIGHTS). Nao inspeciona nem proxia o payload — so reparte o socket.
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define MAX_BACKENDS 32

static int send_fd(int ctrl, int fd) {
    char b[1] = { 0 };
    struct iovec io = { .iov_base = b, .iov_len = 1 };
    union { char buf[CMSG_SPACE(sizeof(int))]; struct cmsghdr align; } u;
    memset(&u, 0, sizeof(u));
    struct msghdr m = { .msg_iov = &io, .msg_iovlen = 1, .msg_control = u.buf, .msg_controllen = sizeof(u.buf) };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(int));
    return (int)sendmsg(ctrl, &m, MSG_NOSIGNAL);
}

// Conecta ao Unix socket de controle de uma API, com retry (a API pode demorar).
static int connect_backend(const char *path) {
    for (int attempt = 0; attempt < 600; attempt++) {
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        struct sockaddr_un a = { .sun_family = AF_UNIX };
        strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
        if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) return fd;
        close(fd);
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000 }; // 100ms
        nanosleep(&ts, NULL);
    }
    return -1;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    int port = getenv("PORT") ? atoi(getenv("PORT")) : 9999;
    const char *backends_env = getenv("RINHA_BACKENDS");
    if (!backends_env) { fprintf(stderr, "RINHA_BACKENDS nao definido\n"); return 1; }

    char list[1024];
    strncpy(list, backends_env, sizeof(list) - 1);
    int ctrl[MAX_BACKENDS], nb = 0;
    for (char *tok = strtok(list, ","); tok && nb < MAX_BACKENDS; tok = strtok(NULL, ",")) {
        int fd = connect_backend(tok);
        if (fd < 0) { fprintf(stderr, "falha ao conectar backend %s\n", tok); return 1; }
        ctrl[nb++] = fd;
        fprintf(stderr, "backend conectado: %s\n", tok);
    }
    if (nb == 0) { fprintf(stderr, "nenhum backend\n"); return 1; }

    int lfd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    listen(lfd, 1024);
    fprintf(stderr, "lb ouvindo na porta %d, %d backends\n", port, nb);

    int rr = 0;
    for (;;) {
        int cfd = accept4(lfd, NULL, NULL, SOCK_CLOEXEC);
        if (cfd < 0) { if (errno == EINTR || errno == ECONNABORTED) continue; break; }
        int b = rr++ % nb;
        if (send_fd(ctrl[b], cfd) < 0) {
            // backend caiu: tenta o proximo uma vez
            int b2 = rr++ % nb;
            send_fd(ctrl[b2], cfd);
        }
        close(cfd); // a API agora e dona do fd
    }
    return 0;
}
