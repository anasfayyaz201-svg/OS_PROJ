#include "switch.h"
#include <stdlib.h>
#include <string.h>

frame_t *frame_alloc(const frame_hdr_t *hdr, const uint8_t *payload)
{
    frame_t *f = malloc(sizeof(*f));
    if (f == NULL) return NULL;

    f->hdr      = *hdr;
    f->refcount = 1;
    f->payload  = NULL;
    pthread_mutex_init(&f->reflock, NULL);

    if (hdr->length > 0 && payload != NULL) {
        f->payload = malloc(hdr->length);
        if (f->payload == NULL) {
            pthread_mutex_destroy(&f->reflock);
            free(f);
            return NULL;
        }
        memcpy(f->payload, payload, hdr->length);
    }
    return f;
}

void frame_ref(frame_t *f)
{
    pthread_mutex_lock(&f->reflock);
    f->refcount++;
    pthread_mutex_unlock(&f->reflock);
}

void frame_unref(frame_t *f)
{
    pthread_mutex_lock(&f->reflock);
    int refs = --f->refcount;
    pthread_mutex_unlock(&f->reflock);

    if (refs == 0) {
        pthread_mutex_destroy(&f->reflock);
        free(f->payload);
        free(f);
    }
}