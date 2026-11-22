#include "switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_line(int fd, char *buf, size_t bufsz)
{
    size_t i = 0;
    while (i < bufsz - 1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r <= 0) return -1;
        if (c == '\n' || c == '\r') break;
        buf[i++] = c;
    }
    buf[i] = '\0';
    return 0;
}

static void send_str(int fd, const char *s)
{
    write_full(fd, s, strlen(s));
}

void handle_admin_cmd(switch_t *sw, int fd)
{
    char line[256];
    if (read_line(fd, line, sizeof(line)) < 0) return;

    /* show mac */
    if (strcmp(line, "show mac") == 0) {
        mac_dump(sw, fd);
        return;
    }

    /* show ports */
    if (strcmp(line, "show ports") == 0) {
        char buf[256];
        for (int i = 0; i < sw->num_ports; i++) {
            port_t *p = &sw->ports[i];
            pthread_mutex_lock(&p->lock);
            const char *s = (p->state == PORT_ACTIVE) ? "up"   :
                            (p->state == PORT_DOWN)   ? "down" : "free";
            int n = snprintf(buf, sizeof(buf),
                "port %d  %-4s  vlan %d  trunk %d  mirror_to %d  rate %d\n",
                i, s, p->vlan, p->is_trunk, p->mirror_to, p->rate_limit);
            pthread_mutex_unlock(&p->lock);
            write_full(fd, buf, (size_t)n);
        }
        write_full(fd, ".\n", 2);
        return;
    }

    /* shutdown <port> */
    if (strncmp(line, "shutdown ", 9) == 0) {
        int idx = atoi(line + 9);
        if (idx < 0 || idx >= sw->num_ports) {
            send_str(fd, "ERROR: invalid port\n"); return;
        }
        port_shutdown(sw, idx);
        send_str(fd, "OK\n");
        return;
    }

    /* up <port>  — allow a shut-down port to accept new connections */
    if (strncmp(line, "up ", 3) == 0) {
        int idx = atoi(line + 3);
        if (idx < 0 || idx >= sw->num_ports) {
            send_str(fd, "ERROR: invalid port\n"); return;
        }
        pthread_mutex_lock(&sw->ports_lock);
        pthread_mutex_lock(&sw->ports[idx].lock);
        /* only allow transition from DOWN to FREE */
        if (sw->ports[idx].state == PORT_DOWN)
            sw->ports[idx].state = PORT_FREE;
        pthread_mutex_unlock(&sw->ports[idx].lock);
        pthread_mutex_unlock(&sw->ports_lock);
        send_str(fd, "OK\n");
        return;
    }

    /* vlan <port> <vlan_id> */
    if (strncmp(line, "vlan ", 5) == 0) {
        int idx, vlan;
        if (sscanf(line + 5, "%d %d", &idx, &vlan) != 2 ||
            idx < 0 || idx >= sw->num_ports ||
            vlan < 1 || vlan > 4094) {
            send_str(fd, "ERROR: usage: vlan <port> <1-4094>\n"); return;
        }
        pthread_mutex_lock(&sw->ports[idx].lock);
        sw->ports[idx].vlan = vlan;
        pthread_mutex_unlock(&sw->ports[idx].lock);
        mac_remove_port(sw, idx);   /* flush stale entries for old VLAN */
        send_str(fd, "OK\n");
        return;
    }

    /* mirror <src_port> <sniffer_port> */
    if (strncmp(line, "mirror ", 7) == 0) {
        int src, dst;
        if (sscanf(line + 7, "%d %d", &src, &dst) != 2 ||
            src < 0 || src >= sw->num_ports ||
            dst < 0 || dst >= sw->num_ports) {
            send_str(fd, "ERROR: usage: mirror <src> <dst>\n"); return;
        }
        pthread_mutex_lock(&sw->ports[src].lock);
        sw->ports[src].mirror_to = dst;
        pthread_mutex_unlock(&sw->ports[src].lock);
        send_str(fd, "OK\n");
        return;
    }

    /* clear mac */
    if (strcmp(line, "clear mac") == 0) {
        mac_clear(sw);
        send_str(fd, "OK\n");
        return;
    }

    /* stats */
    if (strcmp(line, "stats") == 0) {
        if (sw->stats == NULL) {
            send_str(fd, "ERROR: stats unavailable\n"); return;
        }
        char buf[256];
        pthread_mutex_lock(&sw->stats->lock);
        for (int i = 0; i < sw->num_ports; i++) {
            int n = snprintf(buf, sizeof(buf),
                "port %d  in %lu  out %lu  drop %lu  mirror %lu\n",
                i,
                (unsigned long)sw->stats->port[i].frames_in,
                (unsigned long)sw->stats->port[i].frames_out,
                (unsigned long)sw->stats->port[i].frames_dropped,
                (unsigned long)sw->stats->port[i].frames_mirrored);
            write_full(fd, buf, (size_t)n);
        }
        pthread_mutex_unlock(&sw->stats->lock);
        write_full(fd, ".\n", 2);
        return;
    }

    /* rate <port> <fps>  — set rate limit in frames/sec, 0=unlimited */
    if (strncmp(line, "rate ", 5) == 0) {
        int idx, fps;
        if (sscanf(line + 5, "%d %d", &idx, &fps) != 2 ||
            idx < 0 || idx >= sw->num_ports || fps < 0) {
            send_str(fd, "ERROR: usage: rate <port> <fps>\n"); return;
        }
        pthread_mutex_lock(&sw->ports[idx].qlock);
        sw->ports[idx].rate_limit = fps;
        sw->ports[idx].tokens     = fps;
        pthread_mutex_unlock(&sw->ports[idx].qlock);
        send_str(fd, "OK\n");
        return;
    }

    /* trunk <port> <0|1> */
    if (strncmp(line, "trunk ", 6) == 0) {
        int idx, on;
        if (sscanf(line + 6, "%d %d", &idx, &on) != 2 ||
            idx < 0 || idx >= sw->num_ports) {
            send_str(fd, "ERROR: usage: trunk <port> <0|1>\n"); return;
        }
        pthread_mutex_lock(&sw->ports[idx].lock);
        sw->ports[idx].is_trunk = (on != 0);
        pthread_mutex_unlock(&sw->ports[idx].lock);
        send_str(fd, "OK\n");
        return;
    }

    send_str(fd, "ERROR: unknown command\n");
}