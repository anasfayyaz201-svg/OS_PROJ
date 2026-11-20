#include "switch.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

typedef struct { switch_t *sw; int idx; } port_arg_t;

/*
 * port_alloc
 *
 * Caller has already decremented the semaphore (acquired a slot).
 * Returns port index on success, -1 on failure (caller must sem_post + close fd).
 *
 * Lock order: ports_lock -> port[idx].lock
 */
int port_alloc(switch_t *sw, int fd)
{
    pthread_mutex_lock(&sw->ports_lock);
    int idx = -1;
    for (int i = 0; i < sw->num_ports; i++) {
        if (sw->ports[i].state == PORT_FREE) { idx = i; break; }
    }
    if (idx < 0) { pthread_mutex_unlock(&sw->ports_lock); return -1; }

    port_t *p = &sw->ports[idx];
    pthread_mutex_lock(&p->lock);
    p->fd              = fd;
    p->state           = PORT_ACTIVE;
    p->vlan            = 1;
    p->is_trunk        = 0;
    p->mirror_to       = -1;
    p->rate_limit      = 0;
    p->threads_started = 0;
    port_queue_init(p);         /* sets queues_inited=1, inits qlock+conds */
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&sw->ports_lock);

    port_arg_t *as = malloc(sizeof(*as));
    port_arg_t *ar = malloc(sizeof(*ar));
    if (!as || !ar) {
        free(as); free(ar);
        pthread_mutex_lock(&p->lock);
        port_queue_drain_and_destroy(p);
        p->state = PORT_FREE; p->fd = -1;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    as->sw = ar->sw = sw;
    as->idx = ar->idx = idx;

    if (pthread_create(&p->sender_tid, NULL, thread_sender, as) != 0) {
        free(as); free(ar);
        pthread_mutex_lock(&p->lock);
        port_queue_drain_and_destroy(p);
        p->state = PORT_FREE; p->fd = -1;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }
    if (pthread_create(&p->receiver_tid, NULL, thread_receiver, ar) != 0) {
        free(ar);
        pthread_mutex_lock(&p->qlock);
        p->closing = 1;
        pthread_cond_broadcast(&p->not_empty);
        pthread_cond_broadcast(&p->tokens_ready);
        pthread_mutex_unlock(&p->qlock);
        pthread_join(p->sender_tid, NULL);
        pthread_mutex_lock(&p->lock);
        port_queue_drain_and_destroy(p);
        p->state = PORT_FREE; p->fd = -1;
        pthread_mutex_unlock(&p->lock);
        return -1;
    }

    p->threads_started = 1;
    log_msg(sw, "port %d up (fd %d)", idx, fd);
    return idx;
}

/*
 * port_shutdown
 *
 * Safe to call from any thread that is NOT receiver or sender of idx.
 *
 * Lock order: ports_lock -> port[idx].lock -> port[idx].qlock
 */
void port_shutdown(switch_t *sw, int idx)
{
    if (idx < 0 || idx >= sw->num_ports) return;
    port_t *p = &sw->ports[idx];

    pthread_mutex_lock(&sw->ports_lock);
    pthread_mutex_lock(&p->lock);
    if (p->state != PORT_ACTIVE) {
        pthread_mutex_unlock(&p->lock);
        pthread_mutex_unlock(&sw->ports_lock);
        return;
    }
    p->state = PORT_DOWN;
    pthread_mutex_unlock(&p->lock);
    pthread_mutex_unlock(&sw->ports_lock);

    /* 1. Close socket so receiver's read() returns */
    shutdown(p->fd, SHUT_RDWR);

    /* 2. Wake sender */
    pthread_mutex_lock(&p->qlock);
    p->closing = 1;
    pthread_cond_broadcast(&p->not_empty);
    pthread_cond_broadcast(&p->tokens_ready);
    pthread_mutex_unlock(&p->qlock);

    /* 3. Join both threads */
    pthread_join(p->receiver_tid, NULL);
    pthread_join(p->sender_tid,   NULL);

    /* 4. Close fd */
    close(p->fd); p->fd = -1;

    /* 5. Drain queues (safe: sender is gone) */
    port_queue_drain_and_destroy(p);

    /* 6. Remove MACs */
    mac_remove_port(sw, idx);

    /* 7. Mark free */
    pthread_mutex_lock(&sw->ports_lock);
    p->state           = PORT_FREE;
    p->threads_started = 0;
    pthread_mutex_unlock(&sw->ports_lock);

    /* 8. Release semaphore */
    sem_post(sw->free_ports);

    log_msg(sw, "port %d down", idx);
}