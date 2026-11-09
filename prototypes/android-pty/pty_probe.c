/*
 * Standalone Android PTY feasibility probe.
 * Run only as a separate single-purpose process, never by forking from the
 * multithreaded Android app. Tests Bionic PTY setup, VINTR signal delivery,
 * input/output, runtime resize, VEOF and child reaping.
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

static volatile sig_atomic_t got_int = 0;

static void on_int(int signo) {
    (void)signo;
    got_int = 1;
}

static void fail(const char *what) {
    fprintf(stderr, "FAIL: %s: %s\n", what, strerror(errno));
    exit(1);
}

static void stop_child(pid_t child, int master) {
    (void)kill(child, SIGKILL);
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
        if (tcsetpgrp(slave, getpid()) != 0) _exit(114);
        if (dup2(slave, STDIN_FILENO) < 0) _exit(115);
        if (dup2(slave, STDOUT_FILENO) < 0) _exit(116);
        if (dup2(slave, STDERR_FILENO) < 0) _exit(117);
        if (slave > STDERR_FILENO) close(slave);

        struct sigaction action;
        memset(&action, 0, sizeof(action));
        action.sa_handler = on_int;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGINT, &action, NULL) != 0) _exit(118);

        dprintf(STDOUT_FILENO, "READY\n");
        while (!got_int) usleep(10000);
        dprintf(STDOUT_FILENO, "SIGNAL_OK\n");

        char line[256];
        ssize_t n = read(STDIN_FILENO, line, sizeof(line) - 1);
        if (n <= 0) _exit(119);
        line[n] = '\0';
        line[strcspn(line, "\r\n")] = '\0';
        dprintf(STDOUT_FILENO, "INPUT_OK:%s\n", line);

        struct winsize current;
        if (ioctl(STDIN_FILENO, TIOCGWINSZ, &current) != 0) _exit(120);
        dprintf(STDOUT_FILENO, "%u %u\n", current.ws_row, current.ws_col);

        n = read(STDIN_FILENO, line, sizeof(line));
        if (n != 0) _exit(121);
        dprintf(STDOUT_FILENO, "EOF_OK\n");
        _exit(0);
    }

    fprintf(stderr, "PTY slave: %s\n", slave_name);
    char output[4096] = {0};
    size_t used = 0;
    if (!read_until(master, child, output, sizeof(output), &used, "READY"))
        return 2;

    const char interrupt = 3;
    send_bytes(master, &interrupt, 1, child);
    if (!read_until(master, child, output, sizeof(output), &used, "SIGNAL_OK"))
        return 3;

    ws.ws_row = 50;
    ws.ws_col = 120;
    if (ioctl(master, TIOCSWINSZ, &ws) != 0) {
        stop_child(child, master);
        fail("runtime TIOCSWINSZ");
    }

    static const char input[] = "fesTerm-after-interrupt\n";
    send_bytes(master, input, sizeof(input) - 1, child);
    if (!read_until(master, child, output, sizeof(output), &used,
                    "INPUT_OK:fesTerm-after-interrupt"))
        return 4;
    if (!read_until(master, child, output, sizeof(output), &used, "50 120"))
        return 5;

    const char eof = 4;
    send_bytes(master, &eof, 1, child);
    if (!read_until(master, child, output, sizeof(output), &used, "EOF_OK"))
        return 6;
    close(master);

    int status = 0;
    if (waitpid(child, &status, 0) < 0) fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: child exit status=%d\n", status);
        return 7;
    }
    puts("PASS: PTY input/output, Ctrl-C signal, resize, Ctrl-D/EOF and child reaping");
    return 0;
}
