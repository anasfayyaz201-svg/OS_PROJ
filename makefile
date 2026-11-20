CC      = gcc
CFLAGS  = -std=c11 -Wall -Wextra -Werror -g -O0 -pthread
LDFLAGS = -pthread

TSAN_FLAGS = $(CFLAGS) -fsanitize=thread

SWITCH_SRCS = switch.c io.c frame.c mac_shm.c log.c queue.c forward.c \
              port.c threads.c admin.c util.c
HOST_SRCS   = host.c io.c frame.c util.c
SWCTL_SRCS  = swctl.c io.c util.c
SUP_SRCS    = supervisor.c
SW2_SRCS    = switch2.c io.c frame.c util.c
BENCH_SRCS  = benchmark.c

SWITCH_OBJS = $(SWITCH_SRCS:.c=.o)
HOST_OBJS   = $(HOST_SRCS:.c=.o)
SWCTL_OBJS  = $(SWCTL_SRCS:.c=.o)
SUP_OBJS    = $(SUP_SRCS:.c=.o)
SW2_OBJS    = $(SW2_SRCS:.c=.o)
BENCH_OBJS  = $(BENCH_SRCS:.c=.o)

.PHONY: all bonus clean tsan

all: switch host swctl

bonus: all supervisor switch2 benchmark

switch: $(SWITCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

host: $(HOST_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

swctl: $(SWCTL_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

supervisor: $(SUP_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

switch2: $(SW2_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

benchmark: $(BENCH_OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c switch.h
	$(CC) $(CFLAGS) -c -o $@ $<

# supervisor.c and benchmark.c don't depend on switch.h
supervisor.o: supervisor.c
	$(CC) $(CFLAGS) -c -o $@ $<

benchmark.o: benchmark.c
	$(CC) $(CFLAGS) -c -o $@ $<

tsan: switch_tsan host_tsan

switch_tsan: $(SWITCH_SRCS) switch.h
	$(CC) $(TSAN_FLAGS) -o $@ $(SWITCH_SRCS) $(LDFLAGS)

host_tsan: $(HOST_SRCS) switch.h
	$(CC) $(TSAN_FLAGS) -o $@ $(HOST_SRCS) $(LDFLAGS)

clean:
	rm -f *.o switch host swctl supervisor switch2 benchmark \
	    switch_tsan host_tsan \
	    switch.log switch_mac_dump.txt benchmark.csv
	rm -f /tmp/switch_admin.sock