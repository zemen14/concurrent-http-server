#ifndef UTIL_H
#define UTIL_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

int parse_int(const char *str, bool *ok);
ssize_t send_all(int fd, const void *buf, size_t count);
ssize_t read_all(int fd, void *buf, size_t count);

#endif
