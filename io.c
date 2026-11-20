#include "switch.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*
 * Read exactly n bytes from fd.
 * Returns n on success, 0 on clean EOF, -1 on error.
 */
ssize_t read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r == 0)  return 0;
        if (r < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        got += (size_t)r;
    }
    return (ssize_t)got;
}

/*
 * Write exactly n bytes to fd.
 * Returns n on success, -1 on error (EPIPE → -1, errno=EPIPE).
 */
ssize_t write_full(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const char *)buf + sent, n - sent);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)w;
    }
    return (ssize_t)sent;
}

/*
 * Receive one complete frame from fd.
 *
 * Wire layout (17 bytes, no compiler padding — read manually):
 *   [0..1]   length   uint16 big-endian
 *   [2..7]   src MAC
 *   [8..13]  dst MAC
 *   [14..15] vlan     uint16 big-endian
 *   [16]     priority
 *
 * Returns:
 *   0  success; *payload_out malloc'd (caller must free; may be NULL if length==0)
 *   1  peer closed cleanly
 *  -1  error
 */
int recv_frame(int fd, frame_hdr_t *hdr, uint8_t **payload_out)
{
    uint8_t buf[HEADER_LEN];
    ssize_t r = read_full(fd, buf, HEADER_LEN);
    if (r == 0) return 1;
    if (r < 0)  return -1;

    hdr->length   = (uint16_t)((buf[0] << 8) | buf[1]);
    memcpy(hdr->src, buf + 2,  MAC_LEN);
    memcpy(hdr->dst, buf + 8,  MAC_LEN);
    hdr->vlan     = (uint16_t)((buf[14] << 8) | buf[15]);
    hdr->priority = buf[16];

    if (hdr->length > MAX_PAYLOAD) return -1;
    if (hdr->priority > PRIO_LOW)  hdr->priority = PRIO_LOW;

    *payload_out = NULL;
    if (hdr->length > 0) {
        *payload_out = malloc(hdr->length);
        if (*payload_out == NULL) return -1;
        r = read_full(fd, *payload_out, hdr->length);
        if (r <= 0) {
            free(*payload_out);
            *payload_out = NULL;
            return (r == 0) ? 1 : -1;
        }
    }
    return 0;
}

/*
 * Send one complete frame to fd.
 * Returns 0 on success, -1 on error.
 */
int send_frame(int fd, const frame_hdr_t *hdr, const uint8_t *payload)
{
    uint8_t buf[HEADER_LEN];
    buf[0]  = (uint8_t)(hdr->length >> 8);
    buf[1]  = (uint8_t)(hdr->length & 0xff);
    memcpy(buf + 2,  hdr->src, MAC_LEN);
    memcpy(buf + 8,  hdr->dst, MAC_LEN);
    buf[14] = (uint8_t)(hdr->vlan >> 8);
    buf[15] = (uint8_t)(hdr->vlan & 0xff);
    buf[16] = hdr->priority;

    if (write_full(fd, buf, HEADER_LEN) < 0) return -1;
    if (hdr->length > 0 && payload != NULL)
        if (write_full(fd, payload, hdr->length) < 0) return -1;
    return 0;
}