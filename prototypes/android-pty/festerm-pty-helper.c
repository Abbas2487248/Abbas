/*
 * fesTerm isolated Android PTY helper - protocol v1.
 *
 * This executable is intended to run as a dedicated process. Do not link it
 * into the Android UI library and do not call its fork/exec path from the UI.
 *
 * Usage: festerm-pty-helper SOCKET_PATH APP_PRIVATE_HOME
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#define MAX_PAYLOAD 65536u
#define TYPE_INPUT  0x01
#define TYPE_RESIZE 0x02
#define TYPE_CLOSE  0x03
#define TYPE_OUTPUT 0x81
#define TYPE_EXIT   0x82

static int read_full(int fd, void *buf, size_t len) {
    unsigned char *p = (unsigned char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, p + done, len - done);
        if (n == 0) return done == 0 ? 0 : -1;
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -1;
        done += (size_t)n;
    }
    return 1;
}

static int write_full(int fd, const void *buf, size_t len) {
    const unsigned char *p = (const unsigned char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        done += (size_t)n;
    }
    return 0;
}

static uint32_t get_u32be(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void put_u32be(unsigned char *p, uint32_t v) {
    p[0] = (unsigned char)(v >> 24);
    p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8);
    p[3] = (unsigned char)v;
}

static int send_frame(int fd, unsigned char type,
                      const void *payload, uint32_t length) {
    unsigned char header[5];
    header[0] = type;
    put_u32be(header + 1, length);
    if (write_full(fd, header, sizeof(header)) != 0) return -1;
    return length ? write_full(fd, payload, length) : 0;
}

static int terminate_child(pid_t child, int *status) {
    if (child <= 0) return 0;
    (void)kill(-child, SIGHUP);
    for (int i = 0; i < 20; ++i) {
        pid_t r = waitpid(child, status, WNOHANG);
        if (r == child || (r < 0 && errno == ECHILD)) return 1;
        if (r < 0 && errno != EINTR) return 0;
        usleep(50000);
    }
    (void)kill(-child, SIGKILL);
    pid_t r;
    do { r = waitpid(child, status, 0); } while (r < 0 && errno == EINTR);
    return r == child || (r < 0 && errno == ECHILD);
}

static int prepare_socket_path(const struct sockaddr_un *address) {
    struct stat before;
    if (lstat(address->sun_path, &before) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(before.st_mode)) {
        errno = EEXIST;
        return -1;
    }

    int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (probe < 0) return -1;
    int connected = connect(probe, (const struct sockaddr *)address,
                            sizeof(*address));
    int connect_error = errno;
    close(probe);
    if (connected == 0) {
        errno = EADDRINUSE;
        return -1;
    }
    if (connect_error != ECONNREFUSED && connect_error != ENOENT) {
        errno = connect_error;
        return -1;
    }

    struct stat after;
    if (lstat(address->sun_path, &after) != 0)
        return errno == ENOENT ? 0 : -1;
    if (!S_ISSOCK(after.st_mode) || before.st_dev != after.st_dev ||
        before.st_ino != after.st_ino) {
        errno = EAGAIN;
        return -1;
    }
    return unlink(address->sun_path);
}

static int run_session(int client, const char *home_path) {
    int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (master < 0 || grantpt(master) != 0 || unlockpt(master) != 0) {
        if (master >= 0) close(master);
        return 10;
    }

    char slave_path[256];
    if (ptsname_r(master, slave_path, sizeof(slave_path)) != 0) {
        close(master);
        return 11;
    }

    struct winsize initial = {.ws_row = 24, .ws_col = 80};
    (void)ioctl(master, TIOCSWINSZ, &initial);

    pid_t child = fork();
    if (child < 0) {
        close(master);
        return 12;
    }
    if (child == 0) {
        close(client);
        close(master);
        if (setsid() < 0) _exit(120);
        int slave = open(slave_path, O_RDWR);
        if (slave < 0) _exit(121);
        if (ioctl(slave, TIOCSCTTY, 0) != 0) _exit(122);
        (void)tcsetpgrp(slave, getpid());
        if (dup2(slave, STDIN_FILENO) < 0 ||
            dup2(slave, STDOUT_FILENO) < 0 ||
            dup2(slave, STDERR_FILENO) < 0) _exit(123);
        if (slave > STDERR_FILENO) close(slave);
        (void)setenv("TERM", "xterm-256color", 1);
        (void)setenv("HOME", home_path, 1);
        char *const argv[] = {(char *)"/system/bin/sh", (char *)"-i", NULL};
        execv(argv[0], argv);
        _exit(127);
    }

    int done = 0;
    int child_status = 0;
    int child_reaped = 0;
    unsigned char output[8192];
    while (!done) {
        struct pollfd fds[2] = {
            {.fd = client, .events = POLLIN | POLLHUP | POLLERR},
            {.fd = master, .events = POLLIN | POLLHUP | POLLERR}
        };
        int ready = poll(fds, 2, 250);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) break;

        if (fds[0].revents & POLLIN) {
            unsigned char header[5];
            int rr = read_full(client, header, sizeof(header));
            if (rr <= 0) break;
            uint32_t length = get_u32be(header + 1);
            if (length > MAX_PAYLOAD) break;
            unsigned char payload[MAX_PAYLOAD];
            if (length && read_full(client, payload, length) != 1) break;
            if (header[0] == TYPE_INPUT) {
                if (length && write_full(master, payload, length) != 0) break;
            } else if (header[0] == TYPE_RESIZE && length == 4) {
                unsigned int rows = ((unsigned int)payload[0] << 8) | payload[1];
                unsigned int cols = ((unsigned int)payload[2] << 8) | payload[3];
                if (!rows || !cols) break;
                struct winsize ws = {.ws_row = (unsigned short)rows,
                                     .ws_col = (unsigned short)cols};
                if (ioctl(master, TIOCSWINSZ, &ws) != 0) break;
                (void)kill(-child, SIGWINCH);
            } else if (header[0] == TYPE_CLOSE && length == 0) {
                break;
            } else {
                break;
            }
        }
        if (fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) break;

        if (fds[1].revents & POLLIN) {
            ssize_t n = read(master, output, sizeof(output));
            if (n > 0) {
                if (send_frame(client, TYPE_OUTPUT, output, (uint32_t)n) != 0)
                    break;
            } else if (n < 0 && errno != EINTR && errno != EAGAIN &&
                       errno != EIO) {
                break;
            }
        }
        if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) {
            /* PTY closure is normal when the shell exits. */
            done = 1;
        }

        if (!child_reaped) {
            pid_t w = waitpid(child, &child_status, WNOHANG);
            if (w == child) {
                child_reaped = 1;
                done = 1;
            } else if (w < 0 && errno == ECHILD) {
                child_reaped = 1;
                done = 1;
            }
        }
    }

    if (!child_reaped)
        child_reaped = terminate_child(child, &child_status);
    if (child_reaped) {
        int32_t status = (int32_t)child_status;
        unsigned char encoded[4];
        put_u32be(encoded, (uint32_t)status);
        (void)send_frame(client, TYPE_EXIT, encoded, sizeof(encoded));
    }
    close(master);
    return child_reaped ? 0 : 13;
}

int main(int argc, char **argv) {
    const char *home_path = argc == 3 ? argv[2] : getenv("HOME");
    if ((argc != 2 && argc != 3) || argv[1][0] != '/' ||
        !home_path || home_path[0] != '/' ||
        strlen(argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
        fprintf(stderr, "usage: festerm-pty-helper SOCKET_PATH [APP_PRIVATE_HOME]\n");
        return 2;
    }
    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, argv[1], strlen(argv[1]) + 1);

    (void)signal(SIGPIPE, SIG_IGN);
    int server = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) return 3;
    if (prepare_socket_path(&address) != 0 ||
        bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        chmod(address.sun_path, 0600) != 0 || listen(server, 1) != 0) {
        close(server);
        (void)unlink(address.sun_path);
        return 4;
    }
    int client;
    do { client = accept4(server, NULL, NULL, SOCK_CLOEXEC); }
    while (client < 0 && errno == EINTR);
    int result = client < 0 ? 5 : run_session(client, home_path);
    if (client >= 0) close(client);
    close(server);
    (void)unlink(address.sun_path);
    return result;
}
