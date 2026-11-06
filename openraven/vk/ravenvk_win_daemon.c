// ravenvk_win_daemon.c - Native Windows Vulkan (vulkan-1.dll) background worker for WSL2
// Runs the exact same ravenvk.c cooperative-matrix (VK_KHR_cooperative_matrix + FP16 accumulate)
// neural network directly on the native Windows GPU driver and serves frames to WSL2 over localhost TCP.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int sock_t;
#define INVALID_SOCKET (-1)
#define CLOSESOCK close
#endif

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "ravenvk.h"

#pragma pack(push, 1)
typedef struct {
    uint32_t magic;      // 0x52564B31 ("RVK1")
    int32_t  W, H, mode;
    uint8_t  uuid[16];
    int32_t  nl;
    int32_t  src0[64], mode0[64], src1[64], cin[64], cout[64];
} rvk_rpc_init_req;

typedef struct {
    int32_t  ok, coopmat, acc16;
    char     gpu[256], why[256];
    double   ms;
} rvk_rpc_init_resp;
#pragma pack(pop)

static int read_all(sock_t s, void *buf, size_t n) {
    char *p = (char *)buf;
    while (n > 0) {
        int r = recv(s, p, (int)(n > 0x4000000 ? 0x4000000 : n), 0);
        if (r <= 0) return 0;
        p += r; n -= (size_t)r;
    }
    return 1;
}

static int write_all(sock_t s, const void *buf, size_t n) {
    const char *p = (const char *)buf;
    while (n > 0) {
        int r = send(s, p, (int)(n > 0x4000000 ? 0x4000000 : n), 0);
        if (r <= 0) return 0;
        p += r; n -= (size_t)r;
    }
    return 1;
}

static float *w_ptrs[64], *b_ptrs[64];
static void *frame_buf;
static size_t frame_bytes;

static void free_net_weights(void) {
    for (int i = 0; i < 64; i++) {
        free(w_ptrs[i]); w_ptrs[i] = NULL;
        free(b_ptrs[i]); b_ptrs[i] = NULL;
    }
    free(frame_buf); frame_buf = NULL; frame_bytes = 0;
}

static void handle_client(sock_t c) {
    int one = 1;
    setsockopt(c, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    int buf_sz = 16 * 1024 * 1024;
    setsockopt(c, SOL_SOCKET, SO_RCVBUF, (const char *)&buf_sz, sizeof buf_sz);
    setsockopt(c, SOL_SOCKET, SO_SNDBUF, (const char *)&buf_sz, sizeof buf_sz);

    for (;;) {
        uint8_t cmd = 0;
        if (!read_all(c, &cmd, 1)) break;
        if (cmd == 1) { // INIT
            rvk_rpc_init_req req;
            if (!read_all(c, &req, sizeof req) || req.magic != 0x52564B31u || req.nl <= 0 || req.nl > 64) break;
            rvk_destroy();
            free_net_weights();
            int ok_read = 1;
            for (int l = 0; l < req.nl; l++) {
                size_t nw = (size_t)req.cin[l] * (size_t)req.cout[l] * 9 * sizeof(float);
                size_t nb = (size_t)req.cout[l] * sizeof(float);
                w_ptrs[l] = (float *)malloc(nw);
                b_ptrs[l] = (float *)malloc(nb);
                if (!w_ptrs[l] || !b_ptrs[l] || !read_all(c, w_ptrs[l], nw) || !read_all(c, b_ptrs[l], nb)) {
                    ok_read = 0; break;
                }
            }
            if (!ok_read) break;

            rvk_net net = {
                req.nl, req.src0, req.mode0, req.src1, req.cin, req.cout,
                (const float *const *)w_ptrs, (const float *const *)b_ptrs
            };
            rvk_info info;
            int ok = rvk_init(&net, req.W, req.H, req.uuid, req.mode, 0, &info);
            frame_bytes = (size_t)req.W * (size_t)req.H * 8;
            if (ok) {
                frame_buf = malloc(frame_bytes);
                if (!frame_buf) { rvk_destroy(); ok = 0; snprintf(info.why, sizeof info.why, "out of host memory"); }
            }
            rvk_rpc_init_resp resp;
            memset(&resp, 0, sizeof resp);
            resp.ok = ok;
            resp.coopmat = info.coopmat;
            resp.acc16 = info.acc16;
            memcpy(resp.gpu, info.gpu, sizeof resp.gpu);
            memcpy(resp.why, info.why, sizeof resp.why);
            resp.ms = info.ms;
            if (!write_all(c, &resp, sizeof resp)) break;
        } else if (cmd == 2) { // RUN FRAME
            if (!frame_buf || !frame_bytes) break;
            if (!read_all(c, frame_buf, frame_bytes)) break;
            uint8_t status = (rvk_upload(frame_buf) && rvk_run() && rvk_download(frame_buf)) ? 1 : 0;
            if (!write_all(c, &status, 1)) break;
            if (status && !write_all(c, frame_buf, frame_bytes)) break;
        } else if (cmd == 3) { // DESTROY
            rvk_destroy();
            free_net_weights();
            uint8_t ack = 1;
            write_all(c, &ack, 1);
        } else {
            break;
        }
    }
    rvk_destroy();
    free_net_weights();
}

int main(int argc, char **argv) {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    int port = argc > 1 ? atoi(argv[1]) : 28765;
    sock_t srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) return 1;
    int one = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof one);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(srv, (struct sockaddr *)&addr, sizeof addr) != 0) return 1;
    if (listen(srv, 2) != 0) return 1;
    fprintf(stderr, "[ravenvk_win] Native Windows Vulkan worker listening on port %d\n", port);
    for (;;) {
        sock_t c = accept(srv, NULL, NULL);
        if (c == INVALID_SOCKET) continue;
        handle_client(c);
        CLOSESOCK(c);
    }
    return 0;
}
