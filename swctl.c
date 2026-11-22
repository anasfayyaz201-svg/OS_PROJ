#include "switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <time.h>

static int admin_connect(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, ADMIN_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("cannot connect to switch (is it running?)");
        close(fd); return -1;
    }
    return fd;
}

/*
 * Send a command line, then read and print response lines until
 * we see "." or "OK" or "ERROR".
 */
static int run_cmd(const char *cmd)
{
    int fd = admin_connect();
    if (fd < 0) return 1;

    char buf[512];
    int n = snprintf(buf, sizeof(buf), "%s\n", cmd);
    if (write_full(fd, buf, (size_t)n) < 0) {
        perror("write"); close(fd); return 1;
    }

    char ch;
    char line[512];
    int  li = 0;
    int  rc = 0;

    while (read(fd, &ch, 1) == 1) {
        if (ch == '\n') {
            line[li] = '\0'; li = 0;
            if (strcmp(line, ".") == 0) break;
            if (strcmp(line, "OK") == 0) { puts("OK"); break; }
            if (strncmp(line, "ERROR:", 6) == 0) rc = 1;
            puts(line);
        } else {
            if (li < (int)sizeof(line) - 1) line[li++] = ch;
        }
    }
    close(fd);
    return rc;
}

/*
 * stats --watch [num_ports]
 * Reads the shared memory segment directly; refreshes every second.
 */
static int stats_watch(int num_ports)
{
    size_t sz = sizeof(shm_stats_t) + (size_t)num_ports * sizeof(port_stats_t);

    int shm_fd = shm_open(SHM_NAME, O_RDONLY, 0);
    if (shm_fd < 0) {
        perror("shm_open (is switch running?)"); return 1;
    }

    shm_stats_t *stats = mmap(NULL, sz, PROT_READ, MAP_SHARED, shm_fd, 0);
    if (stats == MAP_FAILED) { perror("mmap"); close(shm_fd); return 1; }

    /*
     * Trust the num_ports stored in shared memory if the caller
     * passed 0 (the default).
     */
    if (num_ports <= 0) num_ports = stats->num_ports;

    printf("watching %d ports (Ctrl-C to stop)...\n", num_ports);
    for (;;) {
        /* ANSI clear screen */
        fputs("\033[2J\033[H", stdout);
        printf("%-6s  %-12s  %-12s  %-12s  %-12s\n",
               "port", "in", "out", "dropped", "mirrored");
        for (int i = 0; i < num_ports; i++) {
            printf("%-6d  %-12lu  %-12lu  %-12lu  %-12lu\n",
                   i,
                   (unsigned long)stats->port[i].frames_in,
                   (unsigned long)stats->port[i].frames_out,
                   (unsigned long)stats->port[i].frames_dropped,
                   (unsigned long)stats->port[i].frames_mirrored);
        }
        fflush(stdout);
        sleep(1);
    }

    munmap(stats, sz);
    close(shm_fd);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s <command>\n"
        "\ncommands:\n"
        "  show mac\n"
        "  show ports\n"
        "  shutdown <port>\n"
        "  up <port>\n"
        "  vlan <port> <vlan_id>\n"
        "  trunk <port> <0|1>\n"
        "  mirror <src_port> <sniffer_port>\n"
        "  rate <port> <frames_per_sec>   (0 = unlimited)\n"
        "  clear mac\n"
        "  stats\n"
        "  stats --watch [num_ports]\n",
        prog);
}

int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "stats") == 0 && argc >= 3 &&
        strcmp(argv[2], "--watch") == 0) {
        int np = (argc >= 4) ? atoi(argv[3]) : 0;
        return stats_watch(np);
    }

    /* reassemble remaining args into one command string */
    char cmd[512];
    int  pos = 0;
    for (int i = 1; i < argc; i++) {
        if (pos > 0 && pos < (int)sizeof(cmd) - 1)
            cmd[pos++] = ' ';
        int k = snprintf(cmd + pos, sizeof(cmd) - (size_t)pos, "%s", argv[i]);
        pos += k;
    }
    cmd[pos] = '\0';

    return run_cmd(cmd);
}