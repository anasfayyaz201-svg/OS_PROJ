#include "switch.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>

typedef struct { switch_t *sw; int idx; } port_arg_t;

/* ═══════════════════════════════════════════════════════════════
 * RECEIVER THREAD
 *
 * Reads frames, learns MACs, calls forward_frame().
 *
 * On disconnect or error:
 *   1. Sets closing=1 on the queue and signals the sender.
 *   2. Sets port->state = PORT_DOWN.
 *   3. Returns.
 *
 * Does NOT call port_shutdown() — that would deadlock (join self).
 * switch_cleanup() handles the PORT_DOWN case by joining both
 * threads and freeing resources.
 * ═══════════════════════════════════════════════════════════════ */
void *thread_receiver(void *arg)
{
    port_arg_t *a   = (port_arg_t *)arg;
    switch_t   *sw  = a->sw;
    int         idx = a->idx;
    free(a);

    port_t *p = &sw->ports[idx];

    for (;;) {
        frame_hdr_t  hdr;
        uint8_t     *payload = NULL;
        int rc = recv_frame(p->fd, &hdr, &payload);
        if (rc != 0) {
            free(payload);
            if (rc == 1) log_msg(sw, "port %d: host disconnected", idx);
            else         log_msg(sw, "port %d: recv error", idx);
            break;
        }

        if (sw->stats) {
            pthread_mutex_lock(&sw->stats->lock);
            sw->stats->port[idx].frames_in++;
            pthread_mutex_unlock(&sw->stats->lock);
        }

        mac_learn(sw, hdr.src, idx);

        frame_t *f = frame_alloc(&hdr, payload);
        free(payload);
        if (f == NULL) continue;

        forward_frame(sw, f, idx);
    }

    /* Wake the sender so it drains and exits */
    pthread_mutex_lock(&p->qlock);
    p->closing = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_cond_broadcast(&p->tokens_ready);
    pthread_mutex_unlock(&p->qlock);

    /* Mark DOWN so forward_frame stops routing frames here */
    pthread_mutex_lock(&sw->ports_lock);
    pthread_mutex_lock(&p->lock);
    if (p->state == PORT_ACTIVE) p->state = PORT_DOWN;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&sw->ports_lock);

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * SENDER THREAD
 *
 * Dequeues frames with WRR, checks token bucket, writes to socket.
 * Returns when port_dequeue_wrr() returns NULL (port closing).
 * ═══════════════════════════════════════════════════════════════ */
void *thread_sender(void *arg)
{
    port_arg_t *a   = (port_arg_t *)arg;
    switch_t   *sw  = a->sw;
    int         idx = a->idx;
    free(a);

    port_t *p = &sw->ports[idx];

    for (;;) {
        frame_t *f = port_dequeue_wrr(p);
        if (f == NULL) break;   /* closing */

        /* Token bucket (rate limiting) */
        if (p->rate_limit > 0) {
            pthread_mutex_lock(&p->qlock);
            while (p->tokens <= 0 && !p->closing) {
                struct timespec dl;
                clock_gettime(CLOCK_REALTIME, &dl);
                dl.tv_nsec += 50 * 1000000L;   /* 50 ms max wait */
                if (dl.tv_nsec >= 1000000000L) {
                    dl.tv_sec++;
                    dl.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&p->tokens_ready, &p->qlock, &dl);
            }
            int closing = p->closing;
            if (!closing && p->tokens > 0) p->tokens--;
            pthread_mutex_unlock(&p->qlock);

            if (closing) { frame_unref(f); break; }
        }

        /* SIGPIPE is blocked; EPIPE comes back as -1 from write_full */
        send_frame(p->fd, &f->hdr, f->payload);
        frame_unref(f);
    }

    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * ACCEPT THREAD
 * ═══════════════════════════════════════════════════════════════ */
void *thread_accept(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    pthread_mutex_lock(&sw->shutdown_lock);
    int lfd = sw->listen_fd;
    pthread_mutex_unlock(&sw->shutdown_lock);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            /* EBADF/EINVAL: listen_fd closed by signal thread */
            break;
        }

        pthread_mutex_lock(&sw->shutdown_lock);
        int stopping = sw->shutting_down;
        pthread_mutex_unlock(&sw->shutdown_lock);
        if (stopping) { close(cfd); break; }

        /* macOS has no sem_timedwait; poll with sem_trywait */
        int got_sem = 0;
        for (int _i = 0; _i < PORT_WAIT_SECS * 10; _i++) {
            if (sem_trywait(sw->free_ports) == 0) { got_sem = 1; break; }
            struct timespec _sl = { .tv_sec = 0, .tv_nsec = 100000000L };
            nanosleep(&_sl, NULL);
        }
        if (!got_sem) {
            write_full(cfd, "SWITCH_FULL\n", 12);
            close(cfd);
            log_msg(sw, "connection refused: switch full");
            continue;
        }

        if (port_alloc(sw, cfd) < 0) {
            sem_post(sw->free_ports);
            close(cfd);
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════
 * AGING THREAD — sweeps MAC table every 10 s
 * ═══════════════════════════════════════════════════════════════ */
void *thread_aging(void *arg)
{
    switch_t *sw = (switch_t *)arg;
    for (;;) {
        for (int i = 0; i < 10; i++) {
            struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
            while (nanosleep(&ts, &ts) == -1 && errno == EINTR) ;

            pthread_mutex_lock(&sw->shutdown_lock);
            int stopping = sw->shutting_down;
            pthread_mutex_unlock(&sw->shutdown_lock);
            if (stopping) return NULL;
        }
        mac_age(sw);
    }
}

/* ═══════════════════════════════════════════════════════════════
 * TOKEN REFILL THREAD — refills token buckets every second
 * ═══════════════════════════════════════════════════════════════ */
void *thread_token(void *arg)
{
    switch_t *sw = (switch_t *)arg;
    for (;;) {
        struct timespec ts = { .tv_sec = 1, .tv_nsec = 0 };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR) ;

        pthread_mutex_lock(&sw->shutdown_lock);
        int stopping = sw->shutting_down;
        pthread_mutex_unlock(&sw->shutdown_lock);
        if (stopping) return NULL;

        for (int i = 0; i < sw->num_ports; i++) {
            port_t *p = &sw->ports[i];
            /* read rate_limit under port lock, then update tokens under qlock */
            pthread_mutex_lock(&p->lock);
            int rl    = p->rate_limit;
            int state = p->state;
            pthread_mutex_unlock(&p->lock);

            if (rl > 0 && state == PORT_ACTIVE) {
                pthread_mutex_lock(&p->qlock);
                p->tokens = rl;
                pthread_cond_broadcast(&p->tokens_ready);
                pthread_mutex_unlock(&p->qlock);
            }
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * SIGNAL THREAD
 *
 * Receives signals via sigwait (all other threads block them).
 * On SIGINT/SIGTERM: sets shutting_down, signals main(), closes
 * listen sockets, then returns so main() can join it.
 * ═══════════════════════════════════════════════════════════════ */
void *thread_signal(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    sigaddset(&set, SIGUSR1);

    for (;;) {
        int sig;
        if (sigwait(&set, &sig) != 0) continue;

        if (sig == SIGINT || sig == SIGTERM) {
            log_msg(sw, "signal %d — shutting down", sig);

            pthread_mutex_lock(&sw->shutdown_lock);
            sw->shutting_down = 1;
            pthread_cond_signal(&sw->shutdown_cond);
            pthread_mutex_unlock(&sw->shutdown_lock);

            /* Unblock accept() and admin accept() */
            pthread_mutex_lock(&sw->shutdown_lock);
            int lfd = sw->listen_fd;
            int afd = sw->admin_fd;
            sw->listen_fd = -1;
            sw->admin_fd  = -1;
            pthread_mutex_unlock(&sw->shutdown_lock);

            if (lfd >= 0) { shutdown(lfd, SHUT_RDWR); close(lfd); }
            if (afd >= 0) { shutdown(afd, SHUT_RDWR); close(afd); }
            return NULL;   /* main() will join us */
        }

        if (sig == SIGHUP)
            log_msg(sw, "SIGHUP: config reload not implemented");

        if (sig == SIGUSR1) {
            mac_dump_file(sw);
            log_msg(sw, "SIGUSR1: MAC table dumped to %s", MAC_DUMP_FILE);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════
 * ADMIN THREAD
 * ═══════════════════════════════════════════════════════════════ */
void *thread_admin(void *arg)
{
    switch_t *sw = (switch_t *)arg;

    pthread_mutex_lock(&sw->shutdown_lock);
    int afd = sw->admin_fd;
    pthread_mutex_unlock(&sw->shutdown_lock);

    for (;;) {
        int cfd = accept(afd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) continue;
            break;   /* EBADF/EINVAL: socket closed by signal thread */
        }
        handle_admin_cmd(sw, cfd);
        close(cfd);
    }
    return NULL;
}