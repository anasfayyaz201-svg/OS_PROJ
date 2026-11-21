#include "switch.h"
#include <string.h>

/*
 * forward_frame
 *
 * Lock order respected:  ports_lock -> port[i].lock
 *                        stats->lock taken independently (never nested under ports_lock)
 *
 * We release ports_lock BEFORE doing the enqueue + stats updates so
 * that stats->lock is never taken while ports_lock is held.
 */
void forward_frame(switch_t *sw, frame_t *f, int in_port)
{
    int is_bcast = (memcmp(f->hdr.dst, BROADCAST_MAC, MAC_LEN) == 0);

    /* Read ingress port config under its own lock */
    pthread_mutex_lock(&sw->ports[in_port].lock);
    int      in_trunk = sw->ports[in_port].is_trunk;
    int      in_vlan  = sw->ports[in_port].vlan;
    int      mirror   = sw->ports[in_port].mirror_to;
    pthread_mutex_unlock(&sw->ports[in_port].lock);

    uint16_t vlan = in_trunk ? f->hdr.vlan : (uint16_t)in_vlan;
    f->hdr.vlan   = vlan;

    int dst_port = -1;
    if (!is_bcast)
        dst_port = mac_lookup(sw, f->hdr.dst);

    /* Collect target port indices */
    int targets[66];
    int is_mirror_target[66]; /* 1 if this slot is the mirror copy */
    int ntargets = 0;

    pthread_mutex_lock(&sw->ports_lock);

    if (!is_bcast && dst_port >= 0 && dst_port < sw->num_ports) {
        pthread_mutex_lock(&sw->ports[dst_port].lock);
        if (sw->ports[dst_port].state == PORT_ACTIVE && dst_port != in_port) {
            targets[ntargets]           = dst_port;
            is_mirror_target[ntargets]  = 0;
            ntargets++;
        }
        pthread_mutex_unlock(&sw->ports[dst_port].lock);
    } else {
        for (int i = 0; i < sw->num_ports; i++) {
            pthread_mutex_lock(&sw->ports[i].lock);
            int ok = (sw->ports[i].state == PORT_ACTIVE) &&
                     (i != in_port) &&
                     (sw->ports[i].is_trunk || sw->ports[i].vlan == (int)vlan);
            pthread_mutex_unlock(&sw->ports[i].lock);
            if (ok) {
                targets[ntargets]          = i;
                is_mirror_target[ntargets] = 0;
                ntargets++;
            }
        }
    }

    /* Add mirror port if set and not already in list */
    if (mirror >= 0 && mirror < sw->num_ports) {
        int already = 0;
        for (int i = 0; i < ntargets; i++)
            if (targets[i] == mirror) { already = 1; break; }
        if (!already) {
            pthread_mutex_lock(&sw->ports[mirror].lock);
            int ok = (sw->ports[mirror].state == PORT_ACTIVE);
            pthread_mutex_unlock(&sw->ports[mirror].lock);
            if (ok) {
                targets[ntargets]          = mirror;
                is_mirror_target[ntargets] = 1;
                ntargets++;
            }
        }
    }

    pthread_mutex_unlock(&sw->ports_lock);
    /* ports_lock is now FREE — stats->lock can be taken safely */

    if (ntargets == 0) {
        frame_unref(f);
        return;
    }

    /* Bump refcount for every target beyond the first */
    for (int i = 1; i < ntargets; i++)
        frame_ref(f);

    /* Enqueue and update stats — ports_lock not held here */
    for (int i = 0; i < ntargets; i++) {
        int idx     = targets[i];
        int dropped = port_enqueue(&sw->ports[idx], f);

        if (sw->stats) {
            pthread_mutex_lock(&sw->stats->lock);
            if (dropped < 0) {
                sw->stats->port[idx].frames_dropped++;
            } else {
                sw->stats->port[idx].frames_out++;
                if (is_mirror_target[i])
                    sw->stats->port[idx].frames_mirrored++;
            }
            pthread_mutex_unlock(&sw->stats->lock);
        }

        if (dropped < 0)
            frame_unref(f);
    }
}