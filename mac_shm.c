/*
 * mac_shm.c  —  MAC table backed by shared memory.
 *
 * Uses open-addressing (linear probing) with tombstones so the
 * flat array can be placed in shared memory without pointers.
 *
 * Locking: striped — one process-shared mutex per bucket (256 total).
 * The bucket for a slot is  mac_hash(mac) % MAC_BUCKETS,
 * but the slot index walks linearly through SHM_MAC_SLOTS.
 * We lock the bucket derived from the *original* hash, so all
 * threads/processes that hash to the same bucket serialise on the
 * same mutex regardless of where linear probing lands.
 *
 * This file replaces mac.c.  The function signatures are identical
 * so nothing else in the project needs to change.
 */

#define _POSIX_C_SOURCE 200809L

#include "switch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

/* ── helpers ─────────────────────────────────────────────────── */
static unsigned int mac_hash(const uint8_t *mac)
{
    unsigned int h = 2166136261u;
    for (int i = 0; i < MAC_LEN; i++) { h ^= mac[i]; h *= 16777619u; }
    return h;
}

static int mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, MAC_LEN) == 0;
}

static int is_broadcast(const uint8_t *mac)
{
    return mac_eq(mac, BROADCAST_MAC);
}

/* bucket index from hash */
static unsigned int bucket_of(unsigned int h)
{
    return h % MAC_BUCKETS;
}

/* slot index from hash (wraps inside SHM_MAC_SLOTS) */
static unsigned int slot_of(unsigned int h)
{
    return h % SHM_MAC_SLOTS;
}

/* ── shm init / destroy ──────────────────────────────────────── */
static int shm_mac_create(switch_t *sw)
{
    sw->shm_mac_size = sizeof(shm_mac_t);

    /* Try to open existing segment first (supervisor restart path) */
    sw->shm_mac_fd = shm_open(SHM_MAC_NAME, O_RDWR, 0600);
    if (sw->shm_mac_fd >= 0) {
        /* existing segment — just map it, don't reinitialise */
        sw->shm_mac = mmap(NULL, sw->shm_mac_size,
                           PROT_READ | PROT_WRITE, MAP_SHARED,
                           sw->shm_mac_fd, 0);
        if (sw->shm_mac == MAP_FAILED) {
            perror("mmap shm_mac (existing)");
            close(sw->shm_mac_fd);
            sw->shm_mac_fd = -1;
            sw->shm_mac    = NULL;
            return -1;
        }
        fprintf(stderr, "[switch] MAC table recovered from shared memory\n");
        return 0;
    }

    /* First run — create fresh */
    sw->shm_mac_fd = shm_open(SHM_MAC_NAME, O_CREAT | O_RDWR, 0600);
    if (sw->shm_mac_fd < 0) { perror("shm_open shm_mac"); return -1; }

    if (ftruncate(sw->shm_mac_fd, (off_t)sw->shm_mac_size) < 0) {
        perror("ftruncate shm_mac");
        close(sw->shm_mac_fd); shm_unlink(SHM_MAC_NAME); return -1;
    }

    sw->shm_mac = mmap(NULL, sw->shm_mac_size,
                       PROT_READ | PROT_WRITE, MAP_SHARED,
                       sw->shm_mac_fd, 0);
    if (sw->shm_mac == MAP_FAILED) {
        perror("mmap shm_mac");
        close(sw->shm_mac_fd); shm_unlink(SHM_MAC_NAME); return -1;
    }

    memset(sw->shm_mac, 0, sw->shm_mac_size);

    /* init process-shared mutexes */
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_init(&sw->shm_mac->locks[i], &attr);
    pthread_mutexattr_destroy(&attr);

    sw->shm_mac->total = 0;
    return 0;
}

/* ── mac_table_init ──────────────────────────────────────────── */
void mac_table_init(switch_t *sw)
{
    sw->shm_mac    = NULL;
    sw->shm_mac_fd = -1;

    if (shm_mac_create(sw) < 0)
        fprintf(stderr, "warning: MAC table not in shared memory\n");

    /* legacy bucket heads not used — zero them */
    for (int i = 0; i < MAC_BUCKETS; i++) {
        sw->mac_table[i].head  = NULL;
        sw->mac_table[i].count = 0;
        pthread_mutex_init(&sw->mac_table[i].lock, NULL);
    }
    sw->mac_total = 0;
    pthread_mutex_init(&sw->mac_total_lock, NULL);
}

/* ── mac_table_destroy ───────────────────────────────────────── */
void mac_table_destroy(switch_t *sw)
{
    /* legacy bucket mutexes */
    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_destroy(&sw->mac_table[i].lock);
    pthread_mutex_destroy(&sw->mac_total_lock);

    /* shared memory — unmap but do NOT unlink (supervisor may restart) */
    if (sw->shm_mac && sw->shm_mac != MAP_FAILED) {
        /* destroy mutexes only on clean exit (not crash) */
        for (int i = 0; i < MAC_BUCKETS; i++)
            pthread_mutex_destroy(&sw->shm_mac->locks[i]);
        munmap(sw->shm_mac, sw->shm_mac_size);
        sw->shm_mac = NULL;
    }
    if (sw->shm_mac_fd >= 0) {
        close(sw->shm_mac_fd);
        sw->shm_mac_fd = -1;
    }
    /* Note: we do NOT shm_unlink here so supervisor can recover */
}

/* ── mac_shm_unlink: call only on final clean exit ───────────── */
void mac_shm_unlink(void)
{
    shm_unlink(SHM_MAC_NAME);
}

/* ── internal: find slot for mac (bucket lock must be held) ──── */
/*
 * Returns index of existing slot, or first empty/tombstone slot
 * for insertion, or -1 if table is full.
 * *found is set to 1 if the MAC was found, 0 otherwise.
 */
static int find_slot(shm_mac_t *m, const uint8_t *mac, int *found)
{
    unsigned int h    = mac_hash(mac);
    unsigned int base = slot_of(h);
    int first_tomb    = -1;

    for (unsigned int i = 0; i < SHM_MAC_SLOTS; i++) {
        unsigned int idx = (base + i) % SHM_MAC_SLOTS;
        shm_mac_slot_t *s = &m->slots[idx];

        if (s->used == 0) {
            /* empty — MAC definitely not here */
            *found = 0;
            return (first_tomb >= 0) ? first_tomb : (int)idx;
        }
        if (s->used == 2) {
            /* tombstone — remember first one for insertion */
            if (first_tomb < 0) first_tomb = (int)idx;
            continue;
        }
        /* used == 1 */
        if (mac_eq(s->mac, mac)) {
            *found = 1;
            return (int)idx;
        }
    }
    *found = 0;
    return first_tomb;   /* -1 if totally full */
}

/* ── mac_lookup ──────────────────────────────────────────────── */
int mac_lookup(switch_t *sw, const uint8_t *mac)
{
    if (!sw->shm_mac) return -1;

    unsigned int     bucket = bucket_of(mac_hash(mac));
    shm_mac_t       *m      = sw->shm_mac;

    pthread_mutex_lock(&m->locks[bucket]);
    int found;
    int idx = find_slot(m, mac, &found);
    int port = -1;
    if (found && idx >= 0) {
        port = m->slots[idx].port_index;
        m->slots[idx].last_seen = time(NULL);
    }
    pthread_mutex_unlock(&m->locks[bucket]);
    return port;
}

/* ── mac_learn ───────────────────────────────────────────────── */
void mac_learn(switch_t *sw, const uint8_t *mac, int port_index)
{
    if (is_broadcast(mac)) return;
    if (!sw->shm_mac) return;

    unsigned int  bucket = bucket_of(mac_hash(mac));
    shm_mac_t    *m      = sw->shm_mac;

    pthread_mutex_lock(&m->locks[bucket]);

    int found;
    int idx = find_slot(m, mac, &found);

    if (found && idx >= 0) {
        if (m->slots[idx].port_index != port_index) {
            char mstr[18]; mac_to_str(mac, mstr);
            m->slots[idx].port_index = port_index;
            pthread_mutex_unlock(&m->locks[bucket]);
            log_msg(sw, "MAC moved: %s -> port %d", mstr, port_index);
            return;
        }
        m->slots[idx].last_seen = time(NULL);
        pthread_mutex_unlock(&m->locks[bucket]);
        return;
    }

    /* port security */
    int port_count = 0;
    for (int i = 0; i < SHM_MAC_SLOTS; i++) {
        if (m->slots[i].used == 1 &&
            m->slots[i].port_index == port_index)
            port_count++;
    }
    if (port_count >= MAC_MAX_PER_PORT) {
        char mstr[18]; mac_to_str(mac, mstr);
        pthread_mutex_unlock(&m->locks[bucket]);
        log_msg(sw, "MAC flooding on port %d (new %s), shutting port down",
                port_index, mstr);
        port_shutdown(sw, port_index);
        return;
    }

    /* LRU eviction if full */
    if (m->total >= SHM_MAC_SLOTS - 1) {
        time_t oldest = 0; int old_idx = -1;
        for (int i = 0; i < SHM_MAC_SLOTS; i++) {
            if (m->slots[i].used == 1) {
                if (old_idx < 0 || m->slots[i].last_seen < oldest) {
                    oldest  = m->slots[i].last_seen;
                    old_idx = i;
                }
            }
        }
        if (old_idx >= 0) {
            m->slots[old_idx].used = 2; /* tombstone */
            m->total--;
        }
        /* recalculate idx after eviction */
        idx = find_slot(m, mac, &found);
        if (found) {
            /* was just evicted slot — overwrite tombstone below */
        }
    }

    if (idx < 0) {
        pthread_mutex_unlock(&m->locks[bucket]);
        return; /* table truly full */
    }

    memcpy(m->slots[idx].mac, mac, MAC_LEN);
    m->slots[idx].port_index = port_index;
    m->slots[idx].last_seen  = time(NULL);
    m->slots[idx].used       = 1;
    m->total++;

    pthread_mutex_unlock(&m->locks[bucket]);
}

/* ── mac_remove_port ─────────────────────────────────────────── */
void mac_remove_port(switch_t *sw, int port_index)
{
    if (!sw->shm_mac) return;
    shm_mac_t *m = sw->shm_mac;

    for (int b = 0; b < MAC_BUCKETS; b++) {
        pthread_mutex_lock(&m->locks[b]);
        for (int i = 0; i < SHM_MAC_SLOTS; i++) {
            if (m->slots[i].used == 1 &&
                m->slots[i].port_index == port_index) {
                m->slots[i].used = 2; /* tombstone */
                m->total--;
            }
        }
        pthread_mutex_unlock(&m->locks[b]);
    }
}

/* ── mac_clear ───────────────────────────────────────────────── */
void mac_clear(switch_t *sw)
{
    if (!sw->shm_mac) return;
    shm_mac_t *m = sw->shm_mac;

    for (int b = 0; b < MAC_BUCKETS; b++) {
        pthread_mutex_lock(&m->locks[b]);
        for (int i = 0; i < SHM_MAC_SLOTS; i++)
            m->slots[i].used = 0;
        m->total = 0;
        pthread_mutex_unlock(&m->locks[b]);
    }
}

/* ── mac_age ─────────────────────────────────────────────────── */
void mac_age(switch_t *sw)
{
    if (!sw->shm_mac) return;
    shm_mac_t *m   = sw->shm_mac;
    time_t     now = time(NULL);

    for (int b = 0; b < MAC_BUCKETS; b++) {
        pthread_mutex_lock(&m->locks[b]);
        for (int i = 0; i < SHM_MAC_SLOTS; i++) {
            if (m->slots[i].used == 1 &&
                now - m->slots[i].last_seen > MAC_AGE_SECS) {
                m->slots[i].used = 2;
                m->total--;
            }
        }
        pthread_mutex_unlock(&m->locks[b]);
    }
}

/* ── mac_dump ────────────────────────────────────────────────── */
void mac_dump(switch_t *sw, int fd)
{
    if (!sw->shm_mac) { write_full(fd, ".\n", 2); return; }
    shm_mac_t *m   = sw->shm_mac;
    time_t     now = time(NULL);
    char buf[128];

    /* lock bucket 0 only as a global serialiser, then iterate slots once */
    pthread_mutex_lock(&m->locks[0]);
    for (int i = 0; i < SHM_MAC_SLOTS; i++) {
        if (m->slots[i].used != 1) continue;
        char mstr[18]; mac_to_str(m->slots[i].mac, mstr);
        int n = snprintf(buf, sizeof(buf),
            "%s  port %d  age %lds\n",
            mstr, m->slots[i].port_index,
            (long)(now - m->slots[i].last_seen));
        write_full(fd, buf, (size_t)n);
    }
    pthread_mutex_unlock(&m->locks[0]);
    write_full(fd, ".\n", 2);
}

/* ── mac_dump_file ───────────────────────────────────────────── */
void mac_dump_file(switch_t *sw)
{
    if (!sw->shm_mac) return;
    FILE      *fp  = fopen(MAC_DUMP_FILE, "w");
    if (!fp) return;
    shm_mac_t *m   = sw->shm_mac;
    time_t     now = time(NULL);

    pthread_mutex_lock(&m->locks[0]);
    for (int i = 0; i < SHM_MAC_SLOTS; i++) {
        if (m->slots[i].used != 1) continue;
        char mstr[18]; mac_to_str(m->slots[i].mac, mstr);
        fprintf(fp, "%s  port %d  age %lds\n",
            mstr, m->slots[i].port_index,
            (long)(now - m->slots[i].last_seen));
    }
    pthread_mutex_unlock(&m->locks[0]);
    fclose(fp);
}