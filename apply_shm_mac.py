import os
BASE = os.path.dirname(os.path.abspath(__file__))

def patch_file(path, replacements):
    with open(path, 'r') as f:
        content = f.read()
    for old, new in replacements:
        if old not in content:
            print(f"WARNING: could not find target in {path}: {old[:60]}")
            continue
        content = content.replace(old, new, 1)
    with open(path, 'w') as f:
        f.write(content)
    print(f"patched {path}")

patch_file(os.path.join(BASE, 'switch.h'), [
    (
        '/* ── MAC table ───────────────────────────────────────────────── */',
        '''/* ── Shared MAC table (survives supervisor restart) ──────────── */
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

/* ── MAC table ───────────────────────────────────────────────── */'''
    ),
    (
        '    shm_stats_t    *stats;\n    int             shm_fd;\n    size_t          shm_size;',
        '    shm_stats_t    *stats;\n    int             shm_fd;\n    size_t          shm_size;\n\n    shm_mac_t      *shm_mac;\n    int             shm_mac_fd;\n    size_t          shm_mac_size;'
    ),
    (
        'void mac_table_init(switch_t *sw);',
        'void mac_table_init(switch_t *sw);\nvoid mac_shm_unlink(void);'
    ),
])

patch_file(os.path.join(BASE, 'switch.c'), [
    (
        '    sw.shm_fd                = -1;\n    sw.stats                 = NULL;',
        '    sw.shm_fd                = -1;\n    sw.stats                 = NULL;\n    sw.shm_mac               = NULL;\n    sw.shm_mac_fd            = -1;\n    sw.shm_mac_size          = 0;'
    ),
    (
        '    log_msg(&sw, "switch exited cleanly");\n    log_shutdown(&sw);\n\n    printf("switch exited cleanly\\n");\n    return 0;',
        '    log_msg(&sw, "switch exited cleanly");\n    log_shutdown(&sw);\n\n    mac_shm_unlink();\n    printf("switch exited cleanly\\n");\n    return 0;'
    ),
])

patch_file(os.path.join(BASE, 'Makefile'), [
    (
        'frame.c mac.c log.c',
        'frame.c mac_shm.c log.c'
    ),
])

print("All patches applied.")
