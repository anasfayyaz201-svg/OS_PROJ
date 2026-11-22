/*
 * supervisor.c  —  Bonus: supervisor process
 *
 * Forks the switch process and restarts it if it crashes.
 * The MAC table survives crashes because it lives in shared memory.
 *
 * Usage: ./supervisor <tcp_port> <num_ports>
 *
 * Build: included in Makefile
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>

/* path to the switch binary (same directory) */
#define SWITCH_BIN "./switch"

/* if the switch dies within CRASH_WINDOW_SECS of starting,
   count it as a crash; after MAX_CRASHES give up */
#define CRASH_WINDOW_SECS 5
#define MAX_CRASHES       5

static volatile int g_quit = 0;
static pid_t        g_child_pid = -1;

static void sig_handler(int sig)
{
    (void)sig;
    g_quit = 1;
    if (g_child_pid > 0)
        kill(g_child_pid, SIGINT);
}

int main(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <tcp_port> <num_ports>\n", argv[0]);
        return 1;
    }

    /* forward SIGINT/SIGTERM to child and exit */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sig_handler;
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    /* ignore SIGCHLD — we use waitpid() */
    signal(SIGCHLD, SIG_DFL);

    int crashes = 0;

    printf("[supervisor] starting switch %s %s\n", argv[1], argv[2]);

    while (!g_quit) {
        time_t start = time(NULL);

        pid_t pid = fork();
        if (pid < 0) {
            perror("fork");
            return 1;
        }

        if (pid == 0) {
            /* child: exec the switch */
            execl(SWITCH_BIN, SWITCH_BIN, argv[1], argv[2], NULL);
            perror("execl");
            _exit(1);
        }

        g_child_pid = pid;
        printf("[supervisor] switch started (pid %d)\n", pid);

        int status;
        if (waitpid(pid, &status, 0) < 0) {
            if (errno == EINTR) {
                /* signal received — g_quit is set, loop will exit */
                continue;
            }
            perror("waitpid");
            break;
        }
        g_child_pid = -1;

        if (g_quit) break;

        time_t elapsed = time(NULL) - start;

        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            /* clean exit — don't restart */
            printf("[supervisor] switch exited cleanly\n");
            break;
        }

        if (WIFSIGNALED(status)) {
            printf("[supervisor] switch killed by signal %d\n",
                   WTERMSIG(status));
        } else {
            printf("[supervisor] switch exited with status %d\n",
                   WEXITSTATUS(status));
        }

        if (elapsed < CRASH_WINDOW_SECS) {
            crashes++;
            printf("[supervisor] crash #%d (died after %lds)\n",
                   crashes, (long)elapsed);
            if (crashes >= MAX_CRASHES) {
                printf("[supervisor] too many crashes, giving up\n");
                break;
            }
            /* brief back-off before restarting */
            sleep(1);
        } else {
            /* ran long enough — reset crash counter */
            crashes = 0;
        }

        printf("[supervisor] restarting switch...\n");
    }

    printf("[supervisor] exiting\n");
    return 0;
}