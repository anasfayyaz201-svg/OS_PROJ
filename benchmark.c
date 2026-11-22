/*
 * benchmark.c  —  Bonus: multithreaded MAC table lock benchmark
 *
 * Compares throughput of three locking strategies for the MAC table:
 *   1. One global lock
 *   2. Striped locks (one per bucket) — the real implementation
 *   3. Read-write lock (pthread_rwlock)
 *
 * Usage: ./benchmark [num_threads] [num_ops]
 *
 * Outputs a CSV suitable for plotting:
 *   strategy,threads,ops_per_sec
 *
 * Build: included in Makefile
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <time.h>

#define MAC_LEN      6
#define MAC_BUCKETS  256
#define NUM_OPS      1000000

/* ── MAC entry ───────────────────────────────────────────────── */
typedef struct entry {
    uint8_t       mac[MAC_LEN];
    int           port;
    struct entry *next;
} entry_t;

/* ══════════════════════════════════════════════════════════════
 * Strategy 1: single global lock
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    entry_t        *buckets[MAC_BUCKETS];
    pthread_mutex_t lock;
} global_table_t;

static void global_init(global_table_t *t)
{
    memset(t->buckets, 0, sizeof(t->buckets));
    pthread_mutex_init(&t->lock, NULL);
}

static void global_destroy(global_table_t *t)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        entry_t *e = t->buckets[i];
        while (e) { entry_t *n = e->next; free(e); e = n; }
    }
    pthread_mutex_destroy(&t->lock);
}

static unsigned int mac_hash(const uint8_t *mac)
{
    unsigned int h = 2166136261u;
    for (int i = 0; i < MAC_LEN; i++) { h ^= mac[i]; h *= 16777619u; }
    return h % MAC_BUCKETS;
}

static void global_insert(global_table_t *t, const uint8_t *mac, int port)
{
    unsigned int idx = mac_hash(mac);
    pthread_mutex_lock(&t->lock);
    for (entry_t *e = t->buckets[idx]; e; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            e->port = port;
            pthread_mutex_unlock(&t->lock);
            return;
        }
    }
    entry_t *ne = malloc(sizeof(*ne));
    memcpy(ne->mac, mac, MAC_LEN);
    ne->port = port;
    ne->next = t->buckets[idx];
    t->buckets[idx] = ne;
    pthread_mutex_unlock(&t->lock);
}

static int global_lookup(global_table_t *t, const uint8_t *mac)
{
    unsigned int idx = mac_hash(mac);
    pthread_mutex_lock(&t->lock);
    for (entry_t *e = t->buckets[idx]; e; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            int p = e->port;
            pthread_mutex_unlock(&t->lock);
            return p;
        }
    }
    pthread_mutex_unlock(&t->lock);
    return -1;
}

/* ══════════════════════════════════════════════════════════════
 * Strategy 2: striped locks (one per bucket)
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    entry_t        *buckets[MAC_BUCKETS];
    pthread_mutex_t locks[MAC_BUCKETS];
} striped_table_t;

static void striped_init(striped_table_t *t)
{
    memset(t->buckets, 0, sizeof(t->buckets));
    for (int i = 0; i < MAC_BUCKETS; i++)
        pthread_mutex_init(&t->locks[i], NULL);
}

static void striped_destroy(striped_table_t *t)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        entry_t *e = t->buckets[i];
        while (e) { entry_t *n = e->next; free(e); e = n; }
        pthread_mutex_destroy(&t->locks[i]);
    }
}

static void striped_insert(striped_table_t *t, const uint8_t *mac, int port)
{
    unsigned int idx = mac_hash(mac);
    pthread_mutex_lock(&t->locks[idx]);
    for (entry_t *e = t->buckets[idx]; e; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            e->port = port;
            pthread_mutex_unlock(&t->locks[idx]);
            return;
        }
    }
    entry_t *ne = malloc(sizeof(*ne));
    memcpy(ne->mac, mac, MAC_LEN);
    ne->port = port;
    ne->next = t->buckets[idx];
    t->buckets[idx] = ne;
    pthread_mutex_unlock(&t->locks[idx]);
}

static int striped_lookup(striped_table_t *t, const uint8_t *mac)
{
    unsigned int idx = mac_hash(mac);
    pthread_mutex_lock(&t->locks[idx]);
    for (entry_t *e = t->buckets[idx]; e; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            int p = e->port;
            pthread_mutex_unlock(&t->locks[idx]);
            return p;
        }
    }
    pthread_mutex_unlock(&t->locks[idx]);
    return -1;
}

/* ══════════════════════════════════════════════════════════════
 * Strategy 3: read-write lock
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    entry_t         *buckets[MAC_BUCKETS];
    pthread_rwlock_t lock;
} rwlock_table_t;

static void rwlock_init(rwlock_table_t *t)
{
    memset(t->buckets, 0, sizeof(t->buckets));
    pthread_rwlock_init(&t->lock, NULL);
}

static void rwlock_destroy(rwlock_table_t *t)
{
    for (int i = 0; i < MAC_BUCKETS; i++) {
        entry_t *e = t->buckets[i];
        while (e) { entry_t *n = e->next; free(e); e = n; }
    }
    pthread_rwlock_destroy(&t->lock);
}

static void rwlock_insert(rwlock_table_t *t, const uint8_t *mac, int port)
{
    unsigned int idx = mac_hash(mac);
    pthread_rwlock_wrlock(&t->lock);
    for (entry_t *e = t->buckets[idx]; e; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            e->port = port;
            pthread_rwlock_unlock(&t->lock);
            return;
        }
    }
    entry_t *ne = malloc(sizeof(*ne));
    memcpy(ne->mac, mac, MAC_LEN);
    ne->port = port;
    ne->next = t->buckets[idx];
    t->buckets[idx] = ne;
    pthread_rwlock_unlock(&t->lock);
}

static int rwlock_lookup(rwlock_table_t *t, const uint8_t *mac)
{
    unsigned int idx = mac_hash(mac);
    pthread_rwlock_rdlock(&t->lock);
    for (entry_t *e = t->buckets[idx]; e; e = e->next) {
        if (memcmp(e->mac, mac, MAC_LEN) == 0) {
            int p = e->port;
            pthread_rwlock_unlock(&t->lock);
            return p;
        }
    }
    pthread_rwlock_unlock(&t->lock);
    return -1;
}

/* ══════════════════════════════════════════════════════════════
 * Benchmark runner
 * ══════════════════════════════════════════════════════════════ */
typedef struct {
    int   strategy;   /* 0=global 1=striped 2=rwlock */
    int   thread_id;
    int   num_ops;
    void *table;
    /* 70% lookups, 30% inserts (realistic mix) */
} bench_arg_t;

static void make_mac(uint8_t *mac, int seed)
{
    mac[0] = 0x02;
    mac[1] = (seed >> 16) & 0xff;
    mac[2] = (seed >> 8)  & 0xff;
    mac[3] =  seed        & 0xff;
    mac[4] = (seed >> 24) & 0xff;
    mac[5] = 0x00;
}

static void *bench_thread(void *arg)
{
    bench_arg_t *a = (bench_arg_t *)arg;
    uint8_t mac[MAC_LEN];

    for (int i = 0; i < a->num_ops; i++) {
        int key = (i * 6271 + a->thread_id * 1009) % 1024;
        make_mac(mac, key);

        if (i % 10 < 7) {
            /* lookup */
            switch (a->strategy) {
            case 0: global_lookup ((global_table_t  *)a->table, mac); break;
            case 1: striped_lookup((striped_table_t *)a->table, mac); break;
            case 2: rwlock_lookup ((rwlock_table_t  *)a->table, mac); break;
            }
        } else {
            /* insert */
            switch (a->strategy) {
            case 0: global_insert ((global_table_t  *)a->table, mac, a->thread_id); break;
            case 1: striped_insert((striped_table_t *)a->table, mac, a->thread_id); break;
            case 2: rwlock_insert ((rwlock_table_t  *)a->table, mac, a->thread_id); break;
            }
        }
    }
    return NULL;
}

static double run_bench(int strategy, int num_threads, int num_ops,
                        const char *label)
{
    global_table_t  gt;
    striped_table_t st;
    rwlock_table_t  rt;
    void *table = NULL;

    switch (strategy) {
    case 0: global_init (&gt); table = &gt; break;
    case 1: striped_init(&st); table = &st; break;
    case 2: rwlock_init (&rt); table = &rt; break;
    }

    pthread_t    *tids = malloc((size_t)num_threads * sizeof(pthread_t));
    bench_arg_t  *args = malloc((size_t)num_threads * sizeof(bench_arg_t));

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    for (int i = 0; i < num_threads; i++) {
        args[i].strategy  = strategy;
        args[i].thread_id = i;
        args[i].num_ops   = num_ops / num_threads;
        args[i].table     = table;
        pthread_create(&tids[i], NULL, bench_thread, &args[i]);
    }
    for (int i = 0; i < num_threads; i++)
        pthread_join(tids[i], NULL);

    clock_gettime(CLOCK_MONOTONIC, &t1);

    double elapsed = (t1.tv_sec  - t0.tv_sec) +
                     (t1.tv_nsec - t0.tv_nsec) / 1e9;
    double ops_per_sec = (double)num_ops / elapsed;

    fprintf(stderr, "%-12s  threads=%-2d  ops/sec=%.0f\n",
            label, num_threads, ops_per_sec);

    free(tids); free(args);

    switch (strategy) {
    case 0: global_destroy (&gt); break;
    case 1: striped_destroy(&st); break;
    case 2: rwlock_destroy (&rt); break;
    }

    return ops_per_sec;
}

int main(int argc, char *argv[])
{
    int max_threads = (argc >= 2) ? atoi(argv[1]) : 8;
    int num_ops     = (argc >= 3) ? atoi(argv[2]) : NUM_OPS;

    if (max_threads < 1) max_threads = 1;
    if (num_ops     < 1) num_ops     = NUM_OPS;

    printf("strategy,threads,ops_per_sec\n");

    int thread_counts[] = {1, 2, 4, 8, 16};
    int nc = (int)(sizeof(thread_counts)/sizeof(thread_counts[0]));

    for (int ti = 0; ti < nc; ti++) {
        int nt = thread_counts[ti];
        if (nt > max_threads) break;

        double g = run_bench(0, nt, num_ops, "global");
        double s = run_bench(1, nt, num_ops, "striped");
        double r = run_bench(2, nt, num_ops, "rwlock");

        printf("global,%d,%.0f\n",  nt, g);
        printf("striped,%d,%.0f\n", nt, s);
        printf("rwlock,%d,%.0f\n",  nt, r);
        fflush(stdout);
    }

    return 0;
}