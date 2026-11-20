#include "switch.h"
#include <stdio.h>

void mac_to_str(const uint8_t *mac, char *buf)
{
    snprintf(buf, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

int str_to_mac(const char *s, uint8_t *mac)
{
    unsigned int v[MAC_LEN];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x",
        &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != MAC_LEN)
        return -1;
    for (int i = 0; i < MAC_LEN; i++) {
        if (v[i] > 0xff) return -1;
        mac[i] = (uint8_t)v[i];
    }
    return 0;
}