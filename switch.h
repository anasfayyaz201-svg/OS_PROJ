#ifndef SWITCH_H
#define SWITCH_H

#define _POSIX_C_SOURCE 200809L

#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

/* ── Frame constants ─────────────────────────────────────────── */
#define HEADER_LEN       17
#define MAX_PAYLOAD      1500
#define MAC_LEN          6

#define PRIO_HIGH        0
#define PRIO_NORMAL      1
#define PRIO_LOW         2
#define NUM_PRIO         3

#define WRR_HIGH         5
#define WRR_NORMAL       3
#define WRR_LOW          1
#define AGING_MS         200

#define QUEUE_CAP        64

#define MAC_BUCKETS      256
#define MAC_MAX_ENTRIES  1024
#define MAC_AGE_SECS     30
#define MAC_MAX_PER_PORT 16

#define PORT_WAIT_SECS   5

#define LOG_FILE         "switch.log"
#define LOG_QUEUE_CAP    512
#define ADMIN_SOCK_PATH  "/tmp/switch_admin.sock"
#define SHM_NAME         "/switch_stats"
#define MAC_DUMP_FILE    "switch_mac_dump.txt"

static const uint8_t BROADCAST_MAC[MAC_LEN] = {0xff,0xff,0xff,0xff,0xff,0xff};

/* ── Wire frame header ───────────────────────────────────────── */
typedef struct {
    uint16_t length;
    uint8_t  src[MAC_LEN];
    uint8_t  dst[MAC_LEN];
    uint16_t vlan;
    uint8_t  priority;
} frame_hdr_t;

/* ── Reference-counted frame ─────────────────────────────────── */
typedef struct frame {
    frame_hdr_t      hdr;
    uint8_t         *payload;
    int              refcount;
    pthread_mutex_t  reflock;
} frame_t;

/* ── One priority queue ──────────────────────────────────────── */
typedef struct {
    frame_t **items;
    int       head, tail, count, cap;
    uint64_t  dropped;
} prio_queue_t;

/* ── Per-port stats in shared memory ────────────────────────── */
typedef struct {
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t frames_dropped;
    uint64_t frames_mirrored;
} port_stats_t;

typedef struct {
    pthread_mutex_t lock;      /* PTHREAD_PROCESS_SHARED */
    int             num_ports;
    port_stats_t    port[];    /* flexible array         */
} shm_stats_t;

/* ── Port states ─────────────────────────────────────────────── */
#define PORT_FREE    0
#define PORT_ACTIVE  1
#define PORT_DOWN    2

/* ── Per-port state ──────────────────────────────────────────── */
typedef struct port {
    int   index;
    int   fd;
    int   state;        /* PORT_FREE / PORT_ACTIVE / PORT_DOWN */
    int   vlan;
    int   is_trunk;
    int   mirror_to;    /* -1 or dest port index               */
    int   rate_limit;   /* frames/sec, 0 = unlimited           */

    /* queues */
    prio_queue_t    q[NUM_PRIO];
    struct timespec enqueue_time[NUM_PRIO];

    /*
     * qlock guards: q[], enqueue_time[], tokens, closing.
     * It does NOT guard state/vlan/is_trunk/mirror_to/rate_limit;
     * those are guarded by lock below.
     */
    pthread_mutex_t qlock;
    pthread_cond_t  not_empty;
    pthread_cond_t  tokens_ready;
    int             tokens;
    int             closing;

    /*
     * lock guards: state, vlan, is_trunk, mirror_to, rate_limit.
     */
    pthread_mutex_t lock;

    /*
     * queues_inited: set to 1 by port_queue_init(), back to 0 by
     * port_queue_drain_and_destroy().  Checked before destroying.
     */
    int queues_inited;

    pthread_t receiver_tid;
    pthread_t sender_tid;
    /*
     * threads_started: 1 once both receiver and sender are running.
     * Used by switch_cleanup to know whether joining is safe.
     */
    int threads_started;
} port_t;

/* ── Shared MAC table (survives supervisor restart) ──────────── */
#define SHM_MAC_NAME     "/switch_mac"
#define SHM_MAC_SLOTS    2048

typedef struct {
    uint8_t  mac[6];
    int      port_index;
    time_t   last_seen;
    int      used;
} shm_mac_slot_t;

typedef struct {
    pthread_mutex_t locks[256];
    int             total;
    shm_mac_slot_t  slots[2048];
} shm_mac_t;

/* ── MAC table ───────────────────────────────────────────────── */
typedef struct mac_entry {
    uint8_t          mac[MAC_LEN];
    int              port_index;
    time_t           last_seen;
    struct mac_entry *next;
} mac_entry_t;

typedef struct {
    mac_entry_t    *head;
    pthread_mutex_t lock;
    int             count;
} mac_bucket_t;

/* ── Async log queue ─────────────────────────────────────────── */
typedef struct {
    char          **msgs;
    int             head, tail, count, cap;
    int             closing;
    pthread_mutex_t lock;
    pthread_cond_t  not_empty;
} log_queue_t;

/* ── Global switch state ─────────────────────────────────────── */
typedef struct switch_state {
    int   tcp_port;
    int   num_ports;
    int   listen_fd;
    int   admin_fd;
    int   admin_thread_started;   /* 1 if admin_tid is valid */

    port_t         *ports;
    pthread_mutex_t ports_lock;   /* guards ports[i].state  */

    sem_t          *free_ports;

    mac_bucket_t    mac_table[MAC_BUCKETS];
    int             mac_total;
    pthread_mutex_t mac_total_lock;

    shm_stats_t    *stats;
    int             shm_fd;
    size_t          shm_size;

    shm_mac_t      *shm_mac;
    int             shm_mac_fd;
    size_t          shm_mac_size;

    log_queue_t     logq;
    pthread_t       logger_tid;

    pthread_t       accept_tid;
    pthread_t       aging_tid;
    pthread_t       signal_tid;
    pthread_t       admin_tid;
    pthread_t       token_tid;

    volatile int    shutting_down;
    pthread_mutex_t shutdown_lock;
    pthread_cond_t  shutdown_cond;
} switch_t;

/* ── Prototypes ──────────────────────────────────────────────── */
/* io.c */
ssize_t  read_full(int fd, void *buf, size_t n);
ssize_t  write_full(int fd, const void *buf, size_t n);
int      recv_frame(int fd, frame_hdr_t *hdr, uint8_t **payload_out);
int      send_frame(int fd, const frame_hdr_t *hdr, const uint8_t *payload);

/* frame.c */
frame_t *frame_alloc(const frame_hdr_t *hdr, const uint8_t *payload);
void     frame_ref(frame_t *f);
void     frame_unref(frame_t *f);

/* util.c */
void mac_to_str(const uint8_t *mac, char *buf);
int  str_to_mac(const char *s, uint8_t *mac);

/* log.c */
void  log_init(switch_t *sw);
void  log_msg(switch_t *sw, const char *fmt, ...);
void  log_shutdown(switch_t *sw);
void *thread_logger(void *arg);

/* mac.c */
void mac_table_init(switch_t *sw);
void mac_shm_unlink(void);
void mac_table_destroy(switch_t *sw);
int  mac_lookup(switch_t *sw, const uint8_t *mac);
void mac_learn(switch_t *sw, const uint8_t *mac, int port_index);
void mac_remove_port(switch_t *sw, int port_index);
void mac_clear(switch_t *sw);
void mac_age(switch_t *sw);
void mac_dump(switch_t *sw, int fd);
void mac_dump_file(switch_t *sw);

/* queue.c */
void     port_queue_init(port_t *p);
void     port_queue_drain_and_destroy(port_t *p);
int      port_enqueue(port_t *p, frame_t *f);
frame_t *port_dequeue_wrr(port_t *p);

/* forward.c */
void forward_frame(switch_t *sw, frame_t *f, int in_port);

/* port.c */
int  port_alloc(switch_t *sw, int fd);
void port_shutdown(switch_t *sw, int idx);

/* threads.c */
void *thread_receiver(void *arg);
void *thread_sender(void *arg);
void *thread_accept(void *arg);
void *thread_aging(void *arg);
void *thread_signal(void *arg);
void *thread_admin(void *arg);
void *thread_token(void *arg);

/* admin.c */
void handle_admin_cmd(switch_t *sw, int fd);

#endif /* SWITCH_H */