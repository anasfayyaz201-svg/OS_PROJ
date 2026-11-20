#include "switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

static uint8_t g_mac[MAC_LEN];
static int     g_fd = -1;

/* ── Receiver thread ─────────────────────────────────────────── */
static void *host_receiver(void *arg)
{
    int fd = *(int *)arg;
    for (;;) {
        frame_hdr_t  hdr;
        uint8_t     *payload = NULL;
        int rc = recv_frame(fd, &hdr, &payload);
        if (rc != 0) {
            free(payload);
            if (rc == 1) printf("\n[host] connection closed by switch\n");
            else         printf("\n[host] recv error\n");
            exit(0);
        }
        char src[18], dst[18];
        mac_to_str(hdr.src, src);
        mac_to_str(hdr.dst, dst);
        printf("\n[host] RECV src=%-17s dst=%-17s vlan=%u prio=%u len=%u",
               src, dst,
               (unsigned)hdr.vlan,
               (unsigned)hdr.priority,
               (unsigned)hdr.length);
        if (hdr.length > 0 && payload != NULL)
            printf(" payload=\"%.*s\"", (int)hdr.length, (char *)payload);
        printf("\n> ");
        fflush(stdout);
        free(payload);
    }
    return NULL;
}

/* ── Send helper ─────────────────────────────────────────────── */
static void host_send(int fd, const uint8_t *dst,
                      uint16_t vlan, uint8_t prio,
                      const char *msg)
{
    frame_hdr_t hdr;
    memcpy(hdr.src, g_mac, MAC_LEN);
    memcpy(hdr.dst, dst,   MAC_LEN);
    hdr.vlan     = vlan;
    hdr.priority = prio;
    hdr.length   = (uint16_t)(msg ? strlen(msg) : 0);
    if (hdr.length > MAX_PAYLOAD) hdr.length = MAX_PAYLOAD;

    if (send_frame(fd, &hdr, (const uint8_t *)msg) < 0)
        printf("[host] send error\n");
    else {
        char dstr[18]; mac_to_str(dst, dstr);
        printf("[host] SENT dst=%s vlan=%u prio=%u payload=\"%.*s\"\n",
               dstr, (unsigned)vlan, (unsigned)prio,
               (int)hdr.length, msg ? msg : "");
    }
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <switch_ip> <tcp_port> <mac>\n"
        "\ncommands:\n"
        "  send <dst_mac> <message>\n"
        "  sendv <dst_mac> <vlan> <prio> <message>\n"
        "  broadcast <message>\n"
        "  quit\n", prog);
}

int main(int argc, char *argv[])
{
    if (argc < 4) { usage(argv[0]); return 1; }

    if (str_to_mac(argv[3], g_mac) < 0) {
        fprintf(stderr, "invalid MAC: %s\n", argv[3]); return 1;
    }

    signal(SIGPIPE, SIG_IGN);

    g_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_fd < 0) { perror("socket"); return 1; }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)atoi(argv[2]));
    if (inet_pton(AF_INET, argv[1], &addr.sin_addr) <= 0) {
        fprintf(stderr, "invalid IP: %s\n", argv[1]); return 1;
    }
    if (connect(g_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect"); return 1;
    }

    char mac_str[18]; mac_to_str(g_mac, mac_str);
    printf("[host] connected as %s\n> ", mac_str);
    fflush(stdout);

    int fdcopy = g_fd;
    pthread_t rtid;
    pthread_create(&rtid, NULL, host_receiver, &fdcopy);

    char line[1024];
    while (fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        if (strcmp(line, "quit") == 0) break;
        if (len == 0) { printf("> "); fflush(stdout); continue; }

        if (strncmp(line, "broadcast ", 10) == 0) {
            host_send(g_fd, BROADCAST_MAC, 1, PRIO_NORMAL, line + 10);
        }
        else if (strncmp(line, "send ", 5) == 0) {
            char dstr[32], msg[900];
            uint8_t dst[MAC_LEN];
            if (sscanf(line + 5, "%31s %899[^\n]", dstr, msg) < 2) {
                printf("usage: send <mac> <message>\n");
            } else if (str_to_mac(dstr, dst) < 0) {
                printf("invalid MAC\n");
            } else {
                host_send(g_fd, dst, 1, PRIO_NORMAL, msg);
            }
        }
        else if (strncmp(line, "sendv ", 6) == 0) {
            char dstr[32], msg[900];
            uint8_t dst[MAC_LEN];
            uint16_t vlan = 1; uint8_t prio = PRIO_NORMAL;
            if (sscanf(line + 6, "%31s %hu %hhu %899[^\n]",
                       dstr, &vlan, &prio, msg) < 4) {
                printf("usage: sendv <mac> <vlan> <prio> <message>\n");
            } else if (str_to_mac(dstr, dst) < 0) {
                printf("invalid MAC\n");
            } else {
                host_send(g_fd, dst, vlan, prio, msg);
            }
        }
        else {
            printf("unknown command (send / sendv / broadcast / quit)\n");
        }
        printf("> "); fflush(stdout);
    }

    close(g_fd);
    pthread_join(rtid, NULL);
    return 0;
}