#include "util.h"

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/socket.h>

int parse_int(const char *str, bool *ok) {
    *ok = false;

    if (str == NULL || *str == '\0') {
        return 0;
    }

    errno = 0;
    char *endptr;
    long value = strtol(str, &endptr, 10);

    if (errno != 0 || *endptr != '\0'
        || value < INT_MIN || value > INT_MAX) {
        return 0;
    }

    *ok = true;
    return (int) value;
}

ssize_t send_all(int fd, const void *buf, size_t count) {
    size_t total = 0;
    const char *ptr = buf;

    while (total < count) {
        ssize_t n = send(fd, ptr + total, count - total, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (n == 0) {
            break;
        }

        total += (size_t) n;
    }

    return (ssize_t) total;
}

ssize_t read_all(int fd, void *buf, size_t count) {
    size_t total = 0;
    char *ptr = buf;

    while (total < count) {
        ssize_t n = recv(fd, ptr + total, count - total, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }

        if (n == 0) {
            break;
        }

        total += (size_t) n;
    }

    return (ssize_t) total;
}
