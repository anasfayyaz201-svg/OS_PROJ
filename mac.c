#include "switch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/* FNV-1a over 6 MAC bytes */
static unsigned int mac_hash(const uint8_t *mac)
{
    unsigned int h = 2166136261u;
    for (int i = 0; i < MAC_LEN; i++) {
        h ^= mac[i];
        h *= 16777619u;
    }
    return h % MAC_BUCKETS;
}

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, MAC_LEN) == 0;
}

static int is_broadcast(const uint8_t *mac)
{
    return mac_eq(mac, BROADCAST_MAC);
}

/* ── Init / Destroy ──────────────────────────────────────────── */
void mac_table_init(switch_t *sw)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        sw->mac_table[i].head  = NULL;
        sw->mac_table[i].count = 0;
        pthread_mutex_init(&sw->mac_table[i].lock, NULL);
    }
    sw->mac_total = 0;
    pthread_mutex_init(&sw->mac_total_lock, NULL);
}

void mac_table_destroy(switch_t *sw)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        mac_bucket_t *b = &sw->mac_table[i];
        pthread_mutex_lock(&b->lock);
        mac_entry_t *e = b->head;
        while (e) {
            mac_entry_t *next = e->next;
            free(e);
            e = next;
        }
        b->head  = NULL;
        b->count = 0;
        pthread_mutex_unlock(&b->lock);
        pthread_mutex_destroy(&b->lock);
    }
    pthread_mutex_destroy(&sw->mac_total_lock);
}

/* ── Internal: decrement total safely ───────────────────────── */
static void total_dec(switch_t *sw)
{
    pthread_mutex_lock(&sw->mac_total_lock);
    if (sw->mac_total > 0) sw->mac_total--;
    pthread_mutex_unlock(&sw->mac_total_lock);
}

static void total_inc(switch_t *sw)
{
    pthread_mutex_lock(&sw->mac_total_lock);
    sw->mac_total++;
    pthread_mutex_unlock(&sw->mac_total_lock);
}

/* ── LRU eviction: remove the single oldest entry ───────────── */
static void mac_evict_lru(switch_t *sw)
{
    time_t   oldest      = 0;
    int      best_bucket = -1;
    uint8_t  best_mac[MAC_LEN];
    int      found = 0;

    for (int i = 0; i < MAC_BUCKETS; i++) {
        pthread_mutex_lock(&sw->mac_table[i].lock);
        for (mac_entry_t *e = sw->mac_table[i].head; e; e = e->next) {
            if (!found || e->last_seen < oldest) {
                oldest      = e->last_seen;
                best_bucket = i;
                memcpy(best_mac, e->mac, MAC_LEN);
                found = 1;
            }
        }
        pthread_mutex_unlock(&sw->mac_table[i].lock);
    }
    if (!found) return;

    mac_bucket_t *b = &sw->mac_table[best_bucket];
    pthread_mutex_lock(&b->lock);
    mac_entry_t **pp = &b->head;
    while (*pp) {
        if (mac_eq((*pp)->mac, best_mac)) {
            mac_entry_t *del = *pp;
            *pp = del->next;
            free(del);
            b->count--;
            total_dec(sw);
            break;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&b->lock);
}

/* ── mac_lookup ──────────────────────────────────────────────── */
int mac_lookup(switch_t *sw, const uint8_t *mac)
{
    unsigned int   idx = mac_hash(mac);
    mac_bucket_t  *b   = &sw->mac_table[idx];

    pthread_mutex_lock(&b->lock);
    for (mac_entry_t *e = b->head; e; e = e->next) {
        if (mac_eq(e->mac, mac)) {
            int port = e->port_index;
            e->last_seen = time(NULL);
            pthread_mutex_unlock(&b->lock);
            return port;
        }
    }
    pthread_mutex_unlock(&b->lock);
    return -1;
}

/* ── mac_learn ───────────────────────────────────────────────── */
void mac_learn(switch_t *sw, const uint8_t *mac, int port_index)
{
    if (is_broadcast(mac)) return;

    unsigned int  idx = mac_hash(mac);
    mac_bucket_t *b   = &sw->mac_table[idx];

    /* check existing entry */
    pthread_mutex_lock(&b->lock);
    for (mac_entry_t *e = b->head; e; e = e->next) {
        if (mac_eq(e->mac, mac)) {
            if (e->port_index != port_index) {
                char mstr[18];
                mac_to_str(mac, mstr);
                e->port_index = port_index;
                pthread_mutex_unlock(&b->lock);
                log_msg(sw, "MAC moved: %s -> port %d", mstr, port_index);
                return;
            }
            e->last_seen = time(NULL);
            pthread_mutex_unlock(&b->lock);
            return;
        }
    }
    pthread_mutex_unlock(&b->lock);

    /* port security: count MACs on this port across all buckets */
    int port_mac_count = 0;
    for (int i = 0; i < MAC_BUCKETS; i++) {
        pthread_mutex_lock(&sw->mac_table[i].lock);
        for (mac_entry_t *e = sw->mac_table[i].head; e; e = e->next)
            if (e->port_index == port_index) port_mac_count++;
        pthread_mutex_unlock(&sw->mac_table[i].lock);
    }
    if (port_mac_count >= MAC_MAX_PER_PORT) {
        char mstr[18];
        mac_to_str(mac, mstr);
        log_msg(sw, "MAC flooding on port %d (new %s), shutting port down",
                port_index, mstr);
        port_shutdown(sw, port_index);
        return;
    }

    /* evict LRU if table is full */
    pthread_mutex_lock(&sw->mac_total_lock);
    int total = sw->mac_total;
    pthread_mutex_unlock(&sw->mac_total_lock);
    if (total >= MAC_MAX_ENTRIES)
        mac_evict_lru(sw);

    /* insert */
    mac_entry_t *ne = malloc(sizeof(*ne));
    if (ne == NULL) return;
    memcpy(ne->mac, mac, MAC_LEN);
    ne->port_index = port_index;
    ne->last_seen  = time(NULL);

    pthread_mutex_lock(&b->lock);
    ne->next = b->head;
    b->head  = ne;
    b->count++;
    pthread_mutex_unlock(&b->lock);
    total_inc(sw);
}

/* ── mac_remove_port ─────────────────────────────────────────── */
void mac_remove_port(switch_t *sw, int port_index)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        mac_bucket_t *b = &sw->mac_table[i];
        pthread_mutex_lock(&b->lock);
        mac_entry_t **pp = &b->head;
        while (*pp) {
            if ((*pp)->port_index == port_index) {
                mac_entry_t *del = *pp;
                *pp = del->next;
                free(del);
                b->count--;
                total_dec(sw);
            } else {
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&b->lock);
    }
}

/* ── mac_clear ───────────────────────────────────────────────── */
void mac_clear(switch_t *sw)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        mac_bucket_t *b = &sw->mac_table[i];
        pthread_mutex_lock(&b->lock);
        mac_entry_t *e = b->head;
        while (e) {
            mac_entry_t *next = e->next;
            free(e);
            e = next;
        }
        b->head  = NULL;
        b->count = 0;
        pthread_mutex_unlock(&b->lock);
    }
    pthread_mutex_lock(&sw->mac_total_lock);
    sw->mac_total = 0;
    pthread_mutex_unlock(&sw->mac_total_lock);
}

/* ── mac_age ─────────────────────────────────────────────────── */
void mac_age(switch_t *sw)
{
    time_t now = time(NULL);
    for (int i = 0; i < MAC_BUCKETS; i++) {
        mac_bucket_t *b = &sw->mac_table[i];
        pthread_mutex_lock(&b->lock);
        mac_entry_t **pp = &b->head;
        while (*pp) {
            if (now - (*pp)->last_seen > MAC_AGE_SECS) {
                mac_entry_t *del = *pp;
                *pp = del->next;
                free(del);
                b->count--;
                total_dec(sw);
            } else {
                pp = &(*pp)->next;
            }
        }
        pthread_mutex_unlock(&b->lock);
    }
}

/* ── mac_dump: write table to fd (admin socket) ─────────────── */
void mac_dump(switch_t *sw, int fd)
{
    char buf[128];
    time_t now = time(NULL);
    for (int i = 0; i < MAC_BUCKETS; i++) {
        mac_bucket_t *b = &sw->mac_table[i];
        pthread_mutex_lock(&b->lock);
        for (mac_entry_t *e = b->head; e; e = e->next) {
            char mstr[18];
            mac_to_str(e->mac, mstr);
            int n = snprintf(buf, sizeof(buf),
                             "%s  port %d  age %lds\n",
                             mstr, e->port_index,
                             (long)(now - e->last_seen));
            write_full(fd, buf, (size_t)n);
        }
        pthread_mutex_unlock(&b->lock);
    }
    write_full(fd, ".\n", 2);
}

/* ── mac_dump_file: write table to MAC_DUMP_FILE (SIGUSR1) ──── */
void mac_dump_file(switch_t *sw)
{
    FILE *fp = fopen(MAC_DUMP_FILE, "w");
    if (fp == NULL) return;
    time_t now = time(NULL);
    for (int i = 0; i < MAC_BUCKETS; i++) {
        mac_bucket_t *b = &sw->mac_table[i];
        pthread_mutex_lock(&b->lock);
        for (mac_entry_t *e = b->head; e; e = e->next) {
            char mstr[18];
            mac_to_str(e->mac, mstr);
            fprintf(fp, "%s  port %d  age %lds\n",
                    mstr, e->port_index,
                    (long)(now - e->last_seen));
        }
        pthread_mutex_unlock(&b->lock);
    }
    fclose(fp);
}