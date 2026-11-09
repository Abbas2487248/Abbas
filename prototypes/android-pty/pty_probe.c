/*
 * Standalone Android PTY feasibility probe.
 *
 * Run only as a separate single-purpose process, never by forking from the
 * multithreaded Android app. Tests Bionic PTY setup, input/output, runtime
 * resize, Ctrl-C delivery to the foreground process, Ctrl-D/EOF, and reaping.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void stop_child(pid_t child, int master) {
    kill(child, SIGKILL);
    (void)waitpid(child, NULL, 0);
    if (master >= 0) close(master);
}

static int read_until(int master, pid_t child, char *output, size_t capacity,
                      size_t *used, const char *marker) {
    struct pollfd pfd = {.fd = master, .events = POLLIN | POLLHUP};
    while (strstr(output, marker) == NULL) {
        int ready = poll(&pfd, 1, 7000);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) fail("poll");
        if (ready == 0) {
            fprintf(stderr, "FAIL: timed out waiting for marker: %s\n", marker);
            stop_child(child, master);
            return 0;
        }
        char chunk[512];
        ssize_t n = read(master, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EIO || errno == EAGAIN)) continue;
        if (n <= 0) {
            fprintf(stderr, "FAIL: PTY closed before marker: %s\n", marker);
            stop_child(child, master);
            return 0;
        }
        (void)write(STDOUT_FILENO, chunk, (size_t)n);
        size_t copy = (size_t)n;
        if (copy > capacity - 1 - *used) {
            fprintf(stderr, "FAIL: output buffer limit reached\n");
            stop_child(child, master);
            return 0;
        }
        memcpy(output + *used, chunk, copy);
        *used += copy;
        output[*used] = '\0';
    }
    return 1;
}

static void send_bytes(int master, const char *bytes, size_t length,
                       pid_t child) {
    ssize_t n = write(master, bytes, length);
    if (n != (ssize_t)length) {
        stop_child(child, master);
        fail("write PTY input");
    }
}

int main(void) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0) fail("posix_openpt");
    if (grantpt(master) != 0) fail("grantpt");
    if (unlockpt(master) != 0) fail("unlockpt");

    char slave_name[256];
    if (ptsname_r(master, slave_name, sizeof(slave_name)) != 0)
        fail("ptsname_r");

    struct winsize ws = {.ws_row = 40, .ws_col = 100};
    if (ioctl(master, TIOCSWINSZ, &ws) != 0) fail("initial TIOCSWINSZ");

    pid_t child = fork();
    if (child < 0) fail("fork");

    if (child == 0) {
        close(master);
        if (setsid() < 0) _exit(111);
        int slave = open(slave_name, O_RDWR);
        if (slave < 0) _exit(112);
        if (ioctl(slave, TIOCSCTTY, 0) != 0) _exit(113);
        if (dup2(slave, STDIN_FILENO) < 0) _exit(114);
        if (dup2(slave, STDOUT_FILENO) < 0) _exit(115);
        if (dup2(slave, STDERR_FILENO) < 0) _exit(116);
        if (slave > STDERR_FILENO) close(slave);

        char *const argv[] = {
            (char *)"/system/bin/sh", (char *)"-c",
            (char *)"printf 'READY\\n'; sleep 30; printf 'AFTER_INT\\n'; "
                    "IFS= read -r line; printf 'INPUT_OK:%s\\n' \"$line\"; "
                    "stty size",
            NULL
        };
        execv(argv[0], argv);
        _exit(127);
    }

    fprintf(stderr, "PTY slave: %s\n", slave_name);
    char output[4096] = {0};
    size_t used = 0;

    if (!read_until(master, child, output, sizeof(output), &used, "READY"))
        return 2;

    /* Resize while the session is live. */
    ws.ws_row = 50;
    ws.ws_col = 120;
    if (ioctl(master, TIOCSWINSZ, &ws) != 0) {
        stop_child(child, master);
        fail("runtime TIOCSWINSZ");
    }

    /* VINTR (Ctrl-C) should interrupt the foreground sleep process group. */
    const char interrupt = 3;
    send_bytes(master, &interrupt, 1, child);
    if (!read_until(master, child, output, sizeof(output), &used, "AFTER_INT"))
        return 3;

    static const char input[] = "fesTerm-after-interrupt\n";
    send_bytes(master, input, sizeof(input) - 1, child);
    if (!read_until(master, child, output, sizeof(output), &used,
                    "INPUT_OK:fesTerm-after-interrupt"))
        return 4;
    if (!read_until(master, child, output, sizeof(output), &used, "50 120"))
        return 5;

    /* VEOF (Ctrl-D) on an empty canonical input line should end the shell. */
    const char eof = 4;
    send_bytes(master, &eof, 1, child);
    close(master);

    int status = 0;
    if (waitpid(child, &status, 0) < 0) fail("waitpid");
    if (strstr(output, "INPUT_OK:fesTerm-after-interrupt") == NULL ||
        strstr(output, "50 120") == NULL || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: output checks or child exit failed (status=%d)\n",
                status);
        return 6;
    }

    puts("PASS: PTY input/output, Ctrl-C, resize, Ctrl-D/EOF and child reaping");
    return 0;
}
