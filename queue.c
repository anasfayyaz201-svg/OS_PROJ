#include "switch.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

void port_queue_init(port_t *p)
{
    for (int i = 0; i < NUM_PRIO; i++) {
        prio_queue_t *q = &p->q[i];
        q->items   = malloc(sizeof(frame_t *) * QUEUE_CAP);
        q->head    = 0;
        q->tail    = 0;
        q->count   = 0;
        q->cap     = QUEUE_CAP;
        q->dropped = 0;
    }
    memset(p->enqueue_time, 0, sizeof(p->enqueue_time));
    pthread_mutex_init(&p->qlock, NULL);
    pthread_cond_init(&p->not_empty,    NULL);
    pthread_cond_init(&p->tokens_ready, NULL);
    p->tokens        = 0;
    p->closing       = 0;
    p->queues_inited = 1;
}

/*
 * Drain remaining frames (unref each) and free all queue memory.
 * Also destroys qlock and cond vars.
 *
 * PRE: sender thread has already been joined — no concurrent access.
 * PRE: queues_inited == 1  (guarded; safe to call even if == 0).
 */
void port_queue_drain_and_destroy(port_t *p)
{
    if (!p->queues_inited) return;

    for (int i = 0; i < NUM_PRIO; i++) {
        prio_queue_t *q = &p->q[i];
        while (q->count > 0) {
            frame_t *f = q->items[q->head];
            q->head    = (q->head + 1) % q->cap;
            q->count--;
            frame_unref(f);
        }
        free(q->items);
        q->items = NULL;
    }

    pthread_cond_destroy(&p->tokens_ready);
    pthread_cond_destroy(&p->not_empty);
    pthread_mutex_destroy(&p->qlock);
    p->queues_inited = 0;
}

/*
 * Enqueue f onto port p at the priority given by f->hdr.priority.
 * Caller has already called frame_ref(f) for this slot.
 * Never blocks.
 * Returns  0 on success,
 *         -1 if the queue was full (caller must frame_unref the extra ref).
 */
int port_enqueue(port_t *p, frame_t *f)
{
    int prio = f->hdr.priority;
    if (prio < PRIO_HIGH || prio > PRIO_LOW) prio = PRIO_NORMAL;

    pthread_mutex_lock(&p->qlock);

    if (p->closing) {
        pthread_mutex_unlock(&p->qlock);
        return -1;
    }

    prio_queue_t *q = &p->q[prio];
    if (q->count == q->cap) {
        q->dropped++;
        pthread_mutex_unlock(&p->qlock);
        return -1;
    }

    q->items[q->tail] = f;
    q->tail = (q->tail + 1) % q->cap;
    q->count++;

    if (q->count == 1)
        clock_gettime(CLOCK_MONOTONIC, &p->enqueue_time[prio]);

    pthread_cond_signal(&p->not_empty);
    pthread_mutex_unlock(&p->qlock);
    return 0;
}

/* ── WRR helpers (called with qlock held) ────────────────────── */
static void wrr_reset(int credits[NUM_PRIO])
{
    credits[PRIO_HIGH]   = WRR_HIGH;
    credits[PRIO_NORMAL] = WRR_NORMAL;
    credits[PRIO_LOW]    = WRR_LOW;
}

/*
 * Return index of any queue whose oldest frame has waited >= AGING_MS,
 * preferring lower priority (more starved).  -1 if none.
 * Called with qlock held.
 */
static int aged_queue(port_t *p)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    for (int i = PRIO_LOW; i >= PRIO_HIGH; i--) {
        if (p->q[i].count == 0) continue;
        long ms = (now.tv_sec  - p->enqueue_time[i].tv_sec)  * 1000
                + (now.tv_nsec - p->enqueue_time[i].tv_nsec) / 1000000;
        if (ms >= AGING_MS) return i;
    }
    return -1;
}

static frame_t *dequeue_from(port_t *p, int prio)
{
    prio_queue_t *q = &p->q[prio];
    frame_t *f = q->items[q->head];
    q->head    = (q->head + 1) % q->cap;
    q->count--;
    if (q->count > 0)
        clock_gettime(CLOCK_MONOTONIC, &p->enqueue_time[prio]);
    return f;
}

/*
 * Block until a frame is available or the port is closing.
 * Returns the next frame (WRR + aging), or NULL if closing.
 */
frame_t *port_dequeue_wrr(port_t *p)
{
    int credits[NUM_PRIO];
    wrr_reset(credits);

    pthread_mutex_lock(&p->qlock);

    for (;;) {
        int any = p->q[0].count + p->q[1].count + p->q[2].count;

        if (any == 0) {
            if (p->closing) { pthread_mutex_unlock(&p->qlock); return NULL; }
            pthread_cond_wait(&p->not_empty, &p->qlock);
            continue;
        }

        /* aging check */
        int aged = aged_queue(p);
        if (aged >= 0) {
            frame_t *f = dequeue_from(p, aged);
            pthread_mutex_unlock(&p->qlock);
            return f;
        }

        /* WRR */
        int picked = -1;
        for (int i = PRIO_HIGH; i <= PRIO_LOW; i++) {
            if (p->q[i].count > 0 && credits[i] > 0) { picked = i; break; }
        }
        if (picked >= 0) {
            credits[picked]--;
            /* reset if all non-empty queues exhausted credits */
            int all_zero = 1;
            for (int i = PRIO_HIGH; i <= PRIO_LOW; i++)
                if (p->q[i].count > 0 && credits[i] > 0) { all_zero = 0; break; }
            if (all_zero) wrr_reset(credits);

            frame_t *f = dequeue_from(p, picked);
            pthread_mutex_unlock(&p->qlock);
            return f;
        }

        /* all non-empty queues have zero credits: reset and retry */
        wrr_reset(credits);
    }
}