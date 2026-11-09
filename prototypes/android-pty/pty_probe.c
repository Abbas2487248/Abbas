/*
 * Standalone Android PTY feasibility probe.
 *
 * Run only as a separate single-purpose process, never by forking from the
 * multithreaded Android app. This tests Bionic PTY setup, shell input/output,
 * runtime terminal resizing, and child exit/reaping.
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
            (char *)"/system/bin/sh",
            (char *)"-c",
            (char *)"IFS= read -r line; printf 'INPUT_OK:%s\\n' \"$line\"; stty size",
            NULL
        };
        execv(argv[0], argv);
        _exit(127);
    }

    fprintf(stderr, "PTY slave: %s\n", slave_name);

    /* Change dimensions after the child starts, then send interactive input. */
    ws.ws_row = 50;
    ws.ws_col = 120;
    if (ioctl(master, TIOCSWINSZ, &ws) != 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        fail("runtime TIOCSWINSZ");
    }

    static const char input[] = "fesTerm-input-test\n";
    ssize_t written = write(master, input, sizeof(input) - 1);
    if (written != (ssize_t)(sizeof(input) - 1)) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        fail("write PTY input");
    }

    struct pollfd pfd = {.fd = master, .events = POLLIN | POLLHUP};
    char chunk[512];
    char output[4096];
    size_t used = 0;

    for (;;) {
        int pr = poll(&pfd, 1, 5000);
        if (pr < 0 && errno == EINTR) continue;
        if (pr < 0) fail("poll");
        if (pr == 0) {
            fprintf(stderr, "FAIL: timed out waiting for PTY output\n");
            kill(child, SIGKILL);
            waitpid(child, NULL, 0);
            close(master);
            return 2;
        }

        ssize_t n = read(master, chunk, sizeof(chunk));
        if (n > 0) {
            size_t copy = (size_t)n;
            if (copy > sizeof(output) - 1 - used)
                copy = sizeof(output) - 1 - used;
            memcpy(output + used, chunk, copy);
            used += copy;
            output[used] = '\0';
            (void)write(STDOUT_FILENO, chunk, (size_t)n);
            if (used == sizeof(output) - 1) {
                fprintf(stderr, "FAIL: output buffer limit reached\n");
                kill(child, SIGKILL);
                waitpid(child, NULL, 0);
                close(master);
                return 4;
            }
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EIO || errno == EAGAIN)) break;
        if (n == 0) break;
        if (n < 0) fail("read PTY master");
    }

    close(master);
    int status = 0;
    if (waitpid(child, &status, 0) < 0) fail("waitpid");

    int input_ok = strstr(output, "INPUT_OK:fesTerm-input-test") != NULL;
    int resize_ok = strstr(output, "50 120") != NULL;
    if (!input_ok || !resize_ok || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr,
                "FAIL: input_ok=%d resize_ok=%d child_status=%d\n",
                input_ok, resize_ok, status);
        return 3;
    }

    puts("PASS: PTY input, output, runtime resize, shell exit and waitpid");
    return 0;
}
