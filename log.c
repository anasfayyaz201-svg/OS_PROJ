#include "switch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

void log_init(switch_t *sw)
{
    log_queue_t *q = &sw->logq;
    q->msgs    = malloc(sizeof(char *) * LOG_QUEUE_CAP);
    q->head    = 0;
    q->tail    = 0;
    q->count   = 0;
    q->cap     = LOG_QUEUE_CAP;
    q->closing = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->not_empty, NULL);
}

void log_msg(switch_t *sw, const char *fmt, ...)
{
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);

    /* also print to stderr so it's visible during development */
    fprintf(stderr, "[switch] %s\n", tmp);

    size_t len = strlen(tmp);
    char *msg = malloc(len + 1);
    if (msg == NULL) return;
    memcpy(msg, tmp, len + 1);

    log_queue_t *q = &sw->logq;
    pthread_mutex_lock(&q->lock);
    if (!q->closing && q->count < q->cap) {
        q->msgs[q->tail] = msg;
        q->tail = (q->tail + 1) % q->cap;
        q->count++;
        pthread_cond_signal(&q->not_empty);
    } else {
        free(msg);
    }
    pthread_mutex_unlock(&q->lock);
}

void log_shutdown(switch_t *sw)
{
    log_queue_t *q = &sw->logq;
    pthread_mutex_lock(&q->lock);
    q->closing = 1;
    pthread_cond_signal(&q->not_empty);
    pthread_mutex_unlock(&q->lock);
    pthread_join(sw->logger_tid, NULL);

    /* free any unsent messages */
    while (q->count > 0) {
        free(q->msgs[q->head]);
        q->head = (q->head + 1) % q->cap;
        q->count--;
    }
    free(q->msgs);
    pthread_cond_destroy(&q->not_empty);
    pthread_mutex_destroy(&q->lock);
}

void *thread_logger(void *arg)
{
    switch_t    *sw = (switch_t *)arg;
    log_queue_t *q  = &sw->logq;

    FILE *fp = fopen(LOG_FILE, "a");
    if (fp == NULL) {
        perror("fopen log");
        return NULL;
    }

    for (;;) {
        pthread_mutex_lock(&q->lock);
        while (q->count == 0 && !q->closing)
            pthread_cond_wait(&q->not_empty, &q->lock);

        if (q->count == 0) {
            /* closing and nothing left */
            pthread_mutex_unlock(&q->lock);
            break;
        }

        char *msg = q->msgs[q->head];
        q->head   = (q->head + 1) % q->cap;
        q->count--;
        pthread_mutex_unlock(&q->lock);

        time_t now = time(NULL);
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        char ts[32];
        strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
        fprintf(fp, "[%s] %s\n", ts, msg);
        fflush(fp);
        free(msg);
    }

    fclose(fp);
    return NULL;
}