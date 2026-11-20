#include "switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/* ── Shared memory ───────────────────────────────────────────── */
static int shm_init(switch_t *sw)
{
    sw->shm_size = sizeof(shm_stats_t) +
                   (size_t)sw->num_ports * sizeof(port_stats_t);
    shm_unlink(SHM_NAME);
    sw->shm_fd = shm_open(SHM_NAME, O_CREAT | O_RDWR, 0600);
    if (sw->shm_fd < 0) { perror("shm_open"); return -1; }
    if (ftruncate(sw->shm_fd, (off_t)sw->shm_size) < 0) {
        perror("ftruncate"); close(sw->shm_fd); shm_unlink(SHM_NAME); return -1;
    }
    sw->stats = mmap(NULL, sw->shm_size, PROT_READ|PROT_WRITE, MAP_SHARED, sw->shm_fd, 0);
    if (sw->stats == MAP_FAILED) {
        perror("mmap"); close(sw->shm_fd); shm_unlink(SHM_NAME); return -1;
    }
    memset(sw->stats, 0, sw->shm_size);
    sw->stats->num_ports = sw->num_ports;
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED);
    pthread_mutex_init(&sw->stats->lock, &attr);
    pthread_mutexattr_destroy(&attr);
    return 0;
}

/* ── Sockets ─────────────────────────────────────────────────── */
static int tcp_listen(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(fd); return -1;
    }
    if (listen(fd, 32) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

static int unix_listen(const char *path)
{
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket AF_UNIX"); return -1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind AF_UNIX"); close(fd); return -1;
    }
    if (listen(fd, 8) < 0) { perror("listen AF_UNIX"); close(fd); return -1; }
    return fd;
}

/* ── Clean shutdown ──────────────────────────────────────────── */
/*
 * Called from main() after signal_tid has returned.
 * listen_fd and admin_fd are already closed by signal thread.
 */
static void switch_cleanup(switch_t *sw)
{
    /* 1. Join accept and admin threads.
     *    Their listen sockets are closed so accept() returns EBADF/EINVAL. */
    pthread_join(sw->accept_tid, NULL);

    /* Only join admin_tid if it was actually started */
    if (sw->admin_thread_started)
        pthread_join(sw->admin_tid, NULL);

    /* 2. Shut down / clean up every port. */
    for (int i = 0; i < sw->num_ports; i++) {
        port_t *p = &sw->ports[i];

        pthread_mutex_lock(&sw->ports_lock);
        int state   = p->state;
        int started = p->threads_started;
        pthread_mutex_unlock(&sw->ports_lock);

        if (state == PORT_ACTIVE) {
            /* full teardown: joins both threads internally */
            port_shutdown(sw, i);
        } else if (state == PORT_DOWN && started) {
            /*
             * Receiver exited on its own; it set closing=1 and
             * the sender will have drained and returned.
             * We just need to join and free.
             */
            pthread_join(p->receiver_tid, NULL);
            pthread_join(p->sender_tid,   NULL);
            if (p->fd >= 0) { close(p->fd); p->fd = -1; }
            port_queue_drain_and_destroy(p);
            mac_remove_port(sw, i);
            pthread_mutex_lock(&sw->ports_lock);
            p->state           = PORT_FREE;
            p->threads_started = 0;
            pthread_mutex_unlock(&sw->ports_lock);
            sem_post(sw->free_ports);
        }
        /* PORT_FREE with threads_started==0: nothing to do */
    }

    /* 3. Join aging and token threads (they check shutting_down). */
    pthread_join(sw->aging_tid, NULL);
    pthread_join(sw->token_tid, NULL);

    /* 4. Destroy per-port mutexes (qlock already destroyed per port). */
    for (int i = 0; i < sw->num_ports; i++)
        pthread_mutex_destroy(&sw->ports[i].lock);
    free(sw->ports);

    /* 5. MAC table. */
    mac_table_destroy(sw);

    /* 6. Shared memory. */
    if (sw->stats != NULL) {
        pthread_mutex_destroy(&sw->stats->lock);
        munmap(sw->stats, sw->shm_size);
    }
    if (sw->shm_fd >= 0) close(sw->shm_fd);
    shm_unlink(SHM_NAME);
    unlink(ADMIN_SOCK_PATH);

    /* 7. Sync objects. */
    pthread_mutex_destroy(&sw->ports_lock);
    pthread_cond_destroy(&sw->shutdown_cond);
    pthread_mutex_destroy(&sw->shutdown_lock);
    sem_close(sw->free_ports);
}

/* ── main ────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return 1;
    }
    int tcp_port  = atoi(argv[1]);
    int num_ports = atoi(argv[2]);
    if (tcp_port <= 0 || tcp_port > 65535 || num_ports <= 0 || num_ports > 64) {
        fprintf(stderr, "tcp_port 1-65535, num_ports 1-64\n"); return 1;
    }

    /*
     * Block all signals before creating any thread.
     * Every thread inherits the mask; only signal_tid calls sigwait.
     */
    sigset_t sigset;
    sigfillset(&sigset);
    sigdelset(&sigset, SIGKILL);
    sigdelset(&sigset, SIGSTOP);
    pthread_sigmask(SIG_BLOCK, &sigset, NULL);

    switch_t sw;
    memset(&sw, 0, sizeof(sw));
    sw.tcp_port              = tcp_port;
    sw.num_ports             = num_ports;
    sw.listen_fd             = -1;
    sw.admin_fd              = -1;
    sw.shm_fd                = -1;
    sw.stats                 = NULL;
    sw.shm_mac               = NULL;
    sw.shm_mac_fd            = -1;
    sw.shm_mac_size          = 0;
    sw.shutting_down         = 0;
    sw.admin_thread_started  = 0;

    pthread_mutex_init(&sw.ports_lock,    NULL);
    pthread_mutex_init(&sw.shutdown_lock, NULL);
    pthread_cond_init(&sw.shutdown_cond,  NULL);

    sw.ports = calloc((size_t)num_ports, sizeof(port_t));
    if (!sw.ports) { perror("calloc"); return 1; }

    for (int i = 0; i < num_ports; i++) {
        sw.ports[i].index          = i;
        sw.ports[i].state          = PORT_FREE;
        sw.ports[i].fd             = -1;
        sw.ports[i].mirror_to      = -1;
        sw.ports[i].vlan           = 1;
        sw.ports[i].queues_inited  = 0;
        sw.ports[i].threads_started = 0;
        pthread_mutex_init(&sw.ports[i].lock, NULL);
        /* qlock/conds/tokens inited in port_queue_init() */
    }

    char _sem_name[32]; snprintf(_sem_name, sizeof(_sem_name), "/swports_%d", tcp_port); sem_unlink(_sem_name); sw.free_ports = sem_open(_sem_name, O_CREAT|O_EXCL, 0600, (unsigned int)num_ports); if (sw.free_ports == SEM_FAILED) {
        perror("sem_init"); free(sw.ports); return 1;
    }

    mac_table_init(&sw);

    log_init(&sw);
    if (pthread_create(&sw.logger_tid, NULL, thread_logger, &sw) != 0) {
        perror("pthread_create logger"); return 1;
    }

    if (shm_init(&sw) < 0)
        fprintf(stderr, "warning: stats unavailable\n");

    sw.listen_fd = tcp_listen(tcp_port);
    if (sw.listen_fd < 0) return 1;

    sw.admin_fd = unix_listen(ADMIN_SOCK_PATH);
    if (sw.admin_fd < 0)
        fprintf(stderr, "warning: admin socket unavailable\n");

    log_msg(&sw, "switch started: tcp_port=%d num_ports=%d", tcp_port, num_ports);
    printf("switch listening on TCP port %d (%d ports)\n", tcp_port, num_ports);

    /* Start helper threads */
    if (pthread_create(&sw.signal_tid, NULL, thread_signal, &sw) != 0) {
        perror("pthread_create signal"); return 1;
    }
    if (pthread_create(&sw.accept_tid, NULL, thread_accept, &sw) != 0) {
        perror("pthread_create accept"); return 1;
    }
    if (pthread_create(&sw.aging_tid, NULL, thread_aging, &sw) != 0) {
        perror("pthread_create aging"); return 1;
    }
    if (pthread_create(&sw.token_tid, NULL, thread_token, &sw) != 0) {
        perror("pthread_create token"); return 1;
    }
    if (sw.admin_fd >= 0) {
        if (pthread_create(&sw.admin_tid, NULL, thread_admin, &sw) == 0)
            sw.admin_thread_started = 1;
        else
            perror("pthread_create admin");
    }

    /* Wait for shutdown_cond (set by signal thread on SIGINT/SIGTERM) */
    pthread_mutex_lock(&sw.shutdown_lock);
    while (!sw.shutting_down)
        pthread_cond_wait(&sw.shutdown_cond, &sw.shutdown_lock);
    pthread_mutex_unlock(&sw.shutdown_lock);

    /* Signal thread already returned; join it before cleanup */
    pthread_join(sw.signal_tid, NULL);

    switch_cleanup(&sw);

    log_msg(&sw, "switch exited cleanly");
    log_shutdown(&sw);

    mac_shm_unlink();
    printf("switch exited cleanly\n");
    return 0;
}