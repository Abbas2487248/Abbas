/*
 * Standalone Android PTY feasibility probe.
 *
 * This intentionally runs as a separate process, not inside the multithreaded
 * Android app. It tests Bionic PTY allocation, controlling-TTY setup, shell
 * execution, PTY output, and terminal sizing without changing app dependencies.
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
    if (ioctl(master, TIOCSWINSZ, &ws) != 0) fail("TIOCSWINSZ");

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
            (char *)"printf 'FESTERM_PTY_OK\\n'; stty size",
            NULL
        };
        execv(argv[0], argv);
        _exit(127);
    }

    fprintf(stderr, "PTY slave: %s\n", slave_name);
    struct pollfd pfd = {.fd = master, .events = POLLIN | POLLHUP};
    char buffer[1024];
    int saw_marker = 0;

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

        ssize_t n = read(master, buffer, sizeof(buffer));
        if (n > 0) {
            if (memmem(buffer, (size_t)n, "FESTERM_PTY_OK", 13))
                saw_marker = 1;
            (void)write(STDOUT_FILENO, buffer, (size_t)n);
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

    if (!saw_marker || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FAIL: marker=%d child_status=%d\n",
                saw_marker, status);
        return 3;
    }

    puts("PASS: Bionic PTY allocation, controlling TTY, shell exec, output");
    puts("NOTE: verify the reported size is 40 100.");
    return 0;
}
