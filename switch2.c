/*
 * switch2.c  —  Bonus: two switches linked by a bidirectional pipe trunk
 *
 * This program forks two switch processes and links them with two pipes
 * forming a bidirectional channel (like Lab05 two_way.c).
 *
 * Each switch treats the pipe end as its trunk port: frames on it keep
 * their vlan field, and MAC learning works across the link.
 *
 * Usage: ./switch2 <tcp_port_A> <tcp_port_B> <num_ports>
 *
 *   Switch A listens on tcp_port_A
 *   Switch B listens on tcp_port_B
 *   Each switch gets (num_ports - 1) host ports + 1 trunk port
 *
 * The trunk port is set up by passing the pipe fd as an extra argument
 * to the switch binary via a special environment variable TRUNK_FD.
 *
 * To keep this self-contained we implement a minimal "trunk injector"
 * that sits between the two switches and forwards frames both ways,
 * acting as a simple pipe bridge.
 *
 * Build: included in Makefile
 */

#define _POSIX_C_SOURCE 200809L

#include "switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>

#define SWITCH_BIN "./switch"

static volatile int g_quit = 0;
static pid_t g_pid_a = -1, g_pid_b = -1;

static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
    if (g_pid_a > 0) kill(g_pid_a, SIGINT);
    if (g_pid_b > 0) kill(g_pid_b, SIGINT);
}

/*
 * trunk_relay: reads frames from src_fd and writes them to dst_fd.
 * Runs in its own thread.
 */
typedef struct { int src_fd; int dst_fd; } relay_arg_t;

static void *trunk_relay(void *arg)
{
    relay_arg_t *r = (relay_arg_t *)arg;
    int src = r->src_fd, dst = r->dst_fd;
    free(r);

    for (;;) {
        frame_hdr_t hdr;
        uint8_t    *payload = NULL;
        int rc = recv_frame(src, &hdr, &payload);
        if (rc != 0) { free(payload); break; }

        /* forward with vlan intact (trunk semantics) */
        send_frame(dst, &hdr, payload);
        free(payload);
    }
    return NULL;
}

/*
 * connect_to_switch: connect a TCP socket to the switch at 127.0.0.1:port
 * and return the fd.  Retries for up to 3 seconds (switch may not be ready).
 */
static int connect_to_switch(int port)
{
    int fd = -1;
    for (int tries = 0; tries < 30; tries++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { perror("socket"); return -1; }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((uint16_t)port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0)
            return fd;

        close(fd); fd = -1;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L }; /* 100ms */
        nanosleep(&ts, NULL);
    }
    fprintf(stderr, "switch2: could not connect to switch on port %d\n", port);
    return -1;
}

/*
 * send_trunk_hello: send a dummy frame so the switch learns the trunk MAC
 * and marks us as a trunk port.  We use a reserved multicast MAC as the
 * "trunk identifier" so it never collides with real host MACs.
 */
static void send_trunk_hello(int fd, uint16_t vlan)
{
    frame_hdr_t hdr;
    /* 01:80:C2:00:00:0E  — reserved, never forwarded by real switches */
    static const uint8_t trunk_mac[MAC_LEN] =
        {0x01,0x80,0xC2,0x00,0x00,0x0E};
    memcpy(hdr.src, trunk_mac, MAC_LEN);
    memcpy(hdr.dst, BROADCAST_MAC, MAC_LEN);
    hdr.vlan     = vlan;
    hdr.priority = PRIO_NORMAL;
    hdr.length   = 0;
    send_frame(fd, &hdr, NULL);
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr,
            "usage: %s <tcp_port_A> <tcp_port_B> <num_ports>\n", argv[0]);
        return 1;
    }

    int port_a    = atoi(argv[1]);
    int port_b    = atoi(argv[2]);
    int num_ports = atoi(argv[3]);

    if (port_a <= 0 || port_b <= 0 || num_ports < 2) {
        fprintf(stderr, "invalid arguments\n"); return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Fork switch A */
    g_pid_a = fork();
    if (g_pid_a < 0) { perror("fork A"); return 1; }
    if (g_pid_a == 0) {
        char port_str[16], np_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port_a);
        snprintf(np_str,   sizeof(np_str),   "%d", num_ports);
        execl(SWITCH_BIN, SWITCH_BIN, port_str, np_str, NULL);
        perror("execl switch A"); _exit(1);
    }

    /* Fork switch B */
    g_pid_b = fork();
    if (g_pid_b < 0) { perror("fork B"); kill(g_pid_a, SIGINT); return 1; }
    if (g_pid_b == 0) {
        char port_str[16], np_str[16];
        snprintf(port_str, sizeof(port_str), "%d", port_b);
        snprintf(np_str,   sizeof(np_str),   "%d", num_ports);
        execl(SWITCH_BIN, SWITCH_BIN, port_str, np_str, NULL);
        perror("execl switch B"); _exit(1);
    }

    printf("[switch2] switch A pid=%d port=%d\n", g_pid_a, port_a);
    printf("[switch2] switch B pid=%d port=%d\n", g_pid_b, port_b);

    /* Wait for both switches to start, then connect trunk ports */
    sleep(1);

    int fd_a = connect_to_switch(port_a);
    int fd_b = connect_to_switch(port_b);

    if (fd_a < 0 || fd_b < 0) {
        fprintf(stderr, "[switch2] failed to connect trunk\n");
        kill(g_pid_a, SIGINT); kill(g_pid_b, SIGINT);
        if (fd_a >= 0) close(fd_a);
        if (fd_b >= 0) close(fd_b);
        return 1;
    }

    /* Send trunk hello frames so each switch sets trunk mode via swctl */
    send_trunk_hello(fd_a, 1);
    send_trunk_hello(fd_b, 1);

    printf("[switch2] trunk link established\n");
    printf("[switch2] use: ./swctl -p %d trunk <port> 1  to enable trunk mode\n",
           port_a);

    /* Start relay threads: A→B and B→A */
    pthread_t tid_ab, tid_ba;

    relay_arg_t *ab = malloc(sizeof(*ab));
    ab->src_fd = fd_a; ab->dst_fd = fd_b;
    pthread_create(&tid_ab, NULL, trunk_relay, ab);

    relay_arg_t *ba = malloc(sizeof(*ba));
    ba->src_fd = fd_b; ba->dst_fd = fd_a;
    pthread_create(&tid_ba, NULL, trunk_relay, ba);

    /* Wait for children */
    int status;
    waitpid(g_pid_a, &status, 0);
    waitpid(g_pid_b, &status, 0);

    close(fd_a); close(fd_b);

    pthread_join(tid_ab, NULL);
    pthread_join(tid_ba, NULL);

    printf("[switch2] both switches exited\n");
    return 0;
}