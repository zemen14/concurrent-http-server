#define _POSIX_C_SOURCE 200809L

#include "http.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_REQUEST_LINE 2048
#define MAX_METHOD_LEN 8
#define MAX_URI_LEN 63
#define MAX_VERSION_LEN 16
#define MAX_HEADER_KEY 128
#define MAX_HEADER_VAL 128

typedef struct {
    char method[MAX_METHOD_LEN + 1];
    char uri[MAX_URI_LEN + 2];
    char version[MAX_VERSION_LEN];

    long content_length;
    long long request_id;

    bool has_content_length;
    bool valid;
    bool method_supported;
    bool is_get;
    bool is_put;

    char *body_buffer;
    size_t body_length;
} http_request_t;

static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

#define FILE_LOCK_BUCKETS 128

static pthread_mutex_t file_locks[FILE_LOCK_BUCKETS];
static pthread_once_t file_locks_once = PTHREAD_ONCE_INIT;

static bool read_and_parse_request(int client_fd, http_request_t *req);
static int handle_request(int client_fd, http_request_t *req);
static int handle_get(int client_fd, http_request_t *req);
static int handle_put(int client_fd, http_request_t *req);

static int send_simple_response(
    int client_fd, int status, const char *phrase,
    const char *body, size_t body_len);

static void write_audit_log(const http_request_t *req, int status);

static bool is_alpha_str(const char *s);
static bool is_valid_method(const char *m);
static bool is_valid_uri(const char *uri);
static bool is_valid_version(const char *v);
static bool is_valid_header_key(const char *key);
static bool is_valid_header_value(const char *value);

static void init_file_locks(void) {
    for (int i = 0; i < FILE_LOCK_BUCKETS; i++) {
        pthread_mutex_init(&file_locks[i], NULL);
    }
}

static unsigned long uri_hash(const char *uri) {
    unsigned long h = 5381;
    const unsigned char *p = (const unsigned char *) uri;

    while (*p) {
        h = ((h << 5) + h) + *p;
        p++;
    }

    return h % FILE_LOCK_BUCKETS;
}

void http_process_connection(int client_fd) {
    http_request_t req;
    int status = 0;

    bool parsed = read_and_parse_request(client_fd, &req);

    if (!parsed) {
        const char *body = "Bad Request\n";
        status = 400;
        (void) send_simple_response(
            client_fd, status, "Bad Request", body, strlen(body));
    } else {
        status = handle_request(client_fd, &req);
    }

    write_audit_log(&req, status);

    if (req.body_buffer) {
        free(req.body_buffer);
        req.body_buffer = NULL;
        req.body_length = 0;
    }
}

static bool read_and_parse_request(int client_fd, http_request_t *req) {
    char buffer[MAX_REQUEST_LINE + 1];
    size_t used = 0;
    bool header_done = false;

    memset(req, 0, sizeof(*req));

    while (!header_done && used < MAX_REQUEST_LINE) {
        ssize_t n = recv(
            client_fd, buffer + used, MAX_REQUEST_LINE - used, 0);

        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }

        if (n == 0) {
            return false;
        }

        used += (size_t) n;

        if (used >= 4) {
            for (size_t i = 3; i < used; i++) {
                if (buffer[i - 3] == '\r'
                    && buffer[i - 2] == '\n'
                    && buffer[i - 1] == '\r'
                    && buffer[i] == '\n') {
                    header_done = true;
                    break;
                }
            }
        }
    }

    if (!header_done) {
        return false;
    }

    buffer[used] = '\0';

    char *header_end = strstr(buffer, "\r\n\r\n");

    if (!header_end) {
        return false;
    }

    header_end += 4;

    size_t header_len = (size_t) (header_end - buffer);
    size_t body_in_buf = used - header_len;

    char *line_end = strstr(buffer, "\r\n");

    if (!line_end) {
        return false;
    }

    *line_end = '\0';

    char *saveptr = NULL;
    char *method = strtok_r(buffer, " ", &saveptr);
    char *uri = NULL;
    char *version = NULL;

    if (method) {
        uri = strtok_r(NULL, " ", &saveptr);
    }

    if (uri) {
        version = strtok_r(NULL, " ", &saveptr);
    }

    if (!method || !uri || !version) {
        return false;
    }

    if (!is_valid_method(method)
        || !is_valid_uri(uri)
        || strlen(version) >= MAX_VERSION_LEN) {
        return false;
    }

    strncpy(req->method, method, MAX_METHOD_LEN);
    req->method[MAX_METHOD_LEN] = '\0';

    strncpy(req->uri, uri, sizeof(req->uri) - 1);
    req->uri[sizeof(req->uri) - 1] = '\0';

    strncpy(req->version, version, sizeof(req->version) - 1);
    req->version[sizeof(req->version) - 1] = '\0';

    if (strcmp(req->method, "GET") == 0) {
        req->method_supported = true;
        req->is_get = true;
    } else if (strcmp(req->method, "PUT") == 0) {
        req->method_supported = true;
        req->is_put = true;
    }

    char *cur = line_end + 2;

    while (cur < header_end - 2) {
        char *eol = strstr(cur, "\r\n");

        if (!eol || eol == cur) {
            break;
        }

        *eol = '\0';

        char *colon = strchr(cur, ':');

        if (!colon) {
            return false;
        }

        *colon = '\0';

        char *key = cur;
        char *value = colon + 1;

        while (*value == ' ') {
            value++;
        }

        if (*value == '\0') {
            return false;
        }

        if (!is_valid_header_key(key)
            || !is_valid_header_value(value)) {
            return false;
        }

        if (strcasecmp(key, "Content-Length") == 0) {
            errno = 0;
            char *endptr = NULL;
            long cl = strtol(value, &endptr, 10);

            if (errno != 0 || endptr == value
                || *endptr != '\0' || cl < 0) {
                return false;
            }

            req->content_length = cl;
            req->has_content_length = true;
        } else if (strcasecmp(key, "Request-Id") == 0) {
            errno = 0;
            char *endptr = NULL;
            long long rid = strtoll(value, &endptr, 10);

            if (errno == 0 && endptr != value
                && *endptr == '\0') {
                req->request_id = rid;
            }
        }

        cur = eol + 2;
    }

    if (req->is_put && req->has_content_length
        && req->content_length > 0) {
        size_t total_needed = (size_t) req->content_length;

        req->body_buffer = malloc(total_needed);

        if (!req->body_buffer) {
            return false;
        }

        size_t have = 0;
        size_t to_copy = body_in_buf;

        if (to_copy > total_needed) {
            to_copy = total_needed;
        }

        if (to_copy > 0) {
            memcpy(req->body_buffer, header_end, to_copy);
            have = to_copy;
        }

        while (have < total_needed) {
            ssize_t n = read_all(
                client_fd,
                req->body_buffer + have,
                total_needed - have);

            if (n <= 0) {
                free(req->body_buffer);
                req->body_buffer = NULL;
                return false;
            }

            have += (size_t) n;
        }

        req->body_length = total_needed;
    }

    req->valid = true;
    return true;
}

static int send_simple_response(
    int client_fd, int status, const char *phrase,
    const char *body, size_t body_len) {
    char header[256];

    int n = snprintf(
        header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Content-Length: %zu\r\n"
        "\r\n",
        status, phrase, body_len);

    if (n < 0 || (size_t) n >= sizeof(header)) {
        return -1;
    }

    if (send_all(client_fd, header, (size_t) n) < 0) {
        return -1;
    }

    if (body_len > 0
        && send_all(client_fd, body, body_len) < 0) {
        return -1;
    }

    return 0;
}

static int handle_get(int client_fd, http_request_t *req) {
    if (!is_valid_uri(req->uri)) {
        const char *body = "Bad Request\n";
        (void) send_simple_response(
            client_fd, 400, "Bad Request", body, strlen(body));
        return 400;
    }

    const char *path = req->uri + 1;

    pthread_once(&file_locks_once, init_file_locks);

    int lock_index = (int) uri_hash(req->uri);
    pthread_mutex_lock(&file_locks[lock_index]);

    struct stat st;

    if (stat(path, &st) < 0) {
        int status;
        const char *phrase;
        const char *body;

        if (errno == ENOENT) {
            status = 404;
            phrase = "Not Found";
            body = "Not Found\n";
        } else if (errno == EACCES) {
            status = 403;
            phrase = "Forbidden";
            body = "Forbidden\n";
        } else {
            status = 500;
            phrase = "Internal Server Error";
            body = "Internal Server Error\n";
        }

        (void) send_simple_response(
            client_fd, status, phrase, body, strlen(body));

        pthread_mutex_unlock(&file_locks[lock_index]);
        return status;
    }

    if (!S_ISREG(st.st_mode)) {
        const char *body = "Forbidden\n";
        (void) send_simple_response(
            client_fd, 403, "Forbidden", body, strlen(body));

        pthread_mutex_unlock(&file_locks[lock_index]);
        return 403;
    }

    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        int status = errno == EACCES ? 403 : 500;
        const char *phrase =
            status == 403 ? "Forbidden" : "Internal Server Error";
        const char *body =
            status == 403 ? "Forbidden\n" : "Internal Server Error\n";

        (void) send_simple_response(
            client_fd, status, phrase, body, strlen(body));

        pthread_mutex_unlock(&file_locks[lock_index]);
        return status;
    }

    if (st.st_size < 0) {
        close(fd);
        pthread_mutex_unlock(&file_locks[lock_index]);

        const char *body = "Internal Server Error\n";
        (void) send_simple_response(
            client_fd, 500, "Internal Server Error",
            body, strlen(body));
        return 500;
    }

    size_t len = (size_t) st.st_size;
    char *buf = NULL;

    if (len > 0) {
        buf = malloc(len);

        if (!buf) {
            close(fd);
            pthread_mutex_unlock(&file_locks[lock_index]);

            const char *body = "Internal Server Error\n";
            (void) send_simple_response(
                client_fd, 500, "Internal Server Error",
                body, strlen(body));
            return 500;
        }

        size_t have = 0;

        while (have < len) {
            ssize_t r = read(fd, buf + have, len - have);

            if (r < 0) {
                if (errno == EINTR) {
                    continue;
                }

                free(buf);
                close(fd);
                pthread_mutex_unlock(&file_locks[lock_index]);

                const char *body = "Internal Server Error\n";
                (void) send_simple_response(
                    client_fd, 500, "Internal Server Error",
                    body, strlen(body));
                return 500;
            }

            if (r == 0) {
                break;
            }

            have += (size_t) r;
        }

        len = have;
    }

    close(fd);
    pthread_mutex_unlock(&file_locks[lock_index]);

    int status = 200;

    if (send_simple_response(
            client_fd, status, "OK",
            buf ? buf : "", len) < 0) {
        status = 500;
    }

    free(buf);
    return status;
}

static int handle_put(int client_fd, http_request_t *req) {
    if (!is_valid_uri(req->uri)) {
        const char *body = "Bad Request\n";
        (void) send_simple_response(
            client_fd, 400, "Bad Request", body, strlen(body));
        return 400;
    }

    const char *path = req->uri + 1;

    pthread_once(&file_locks_once, init_file_locks);

    int lock_index = (int) uri_hash(req->uri);
    pthread_mutex_lock(&file_locks[lock_index]);

    bool existed = access(path, F_OK) == 0;

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);

    if (fd < 0) {
        int status = (errno == EACCES || errno == EISDIR)
            ? 403 : 500;

        pthread_mutex_unlock(&file_locks[lock_index]);

        const char *phrase =
            status == 403 ? "Forbidden" : "Internal Server Error";
        const char *body =
            status == 403 ? "Forbidden\n" : "Internal Server Error\n";

        (void) send_simple_response(
            client_fd, status, phrase, body, strlen(body));

        return status;
    }

    size_t total_to_write =
        req->has_content_length ? req->body_length : 0;

    size_t written = 0;

    while (written < total_to_write) {
        ssize_t w = write(
            fd,
            req->body_buffer + written,
            total_to_write - written);

        if (w < 0) {
            if (errno == EINTR) {
                continue;
            }

            close(fd);
            pthread_mutex_unlock(&file_locks[lock_index]);

            const char *body = "Internal Server Error\n";
            (void) send_simple_response(
                client_fd, 500, "Internal Server Error",
                body, strlen(body));

            return 500;
        }

        if (w == 0) {
            break;
        }

        written += (size_t) w;
    }

    close(fd);
    pthread_mutex_unlock(&file_locks[lock_index]);

    if (existed) {
        const char *body = "OK\n";
        (void) send_simple_response(
            client_fd, 200, "OK", body, strlen(body));
        return 200;
    }

    const char *body = "Created\n";
    (void) send_simple_response(
        client_fd, 201, "Created", body, strlen(body));

    return 201;
}

static int handle_request(int client_fd, http_request_t *req) {
    if (!is_valid_version(req->version)) {
        const char *body = "Bad Request\n";
        (void) send_simple_response(
            client_fd, 400, "Bad Request", body, strlen(body));
        return 400;
    }

    if (strcmp(req->version, "HTTP/1.1") != 0) {
        const char *body = "Version Not Supported\n";
        (void) send_simple_response(
            client_fd, 505, "Version Not Supported",
            body, strlen(body));
        return 505;
    }

    if (!req->method_supported) {
        const char *body = "Not Implemented\n";
        (void) send_simple_response(
            client_fd, 501, "Not Implemented", body, strlen(body));
        return 501;
    }

    if (req->is_get) {
        return handle_get(client_fd, req);
    }

    if (req->is_put) {
        return handle_put(client_fd, req);
    }

    const char *body = "Not Implemented\n";
    (void) send_simple_response(
        client_fd, 501, "Not Implemented", body, strlen(body));

    return 501;
}

static void write_audit_log(const http_request_t *req, int status) {
    const char *method =
        (req && req->valid && req->method[0])
            ? req->method : "UNKNOWN";

    const char *uri =
        (req && req->valid && req->uri[0])
            ? req->uri : "/";

    long long rid = (req && req->valid)
        ? req->request_id : 0;

    pthread_mutex_lock(&log_mutex);

    fprintf(stderr, "%s,%s,%d,%lld\n",
        method, uri, status, rid);

    fflush(stderr);

    pthread_mutex_unlock(&log_mutex);
}

static bool is_alpha_str(const char *s) {
    if (!s || *s == '\0') {
        return false;
    }

    for (const char *p = s; *p; p++) {
        if ((*p < 'A' || *p > 'Z')
            && (*p < 'a' || *p > 'z')) {
            return false;
        }
    }

    return true;
}

static bool is_valid_method(const char *m) {
    size_t len = strlen(m);

    if (len == 0 || len > MAX_METHOD_LEN) {
        return false;
    }

    return is_alpha_str(m);
}

static bool is_valid_uri(const char *uri) {
    size_t len = strlen(uri);

    if (len < 2 || uri[0] != '/'
        || len - 1 > MAX_URI_LEN) {
        return false;
    }

    for (size_t i = 1; i < len; i++) {
        char c = uri[i];

        if (!((c >= 'A' && c <= 'Z')
              || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9')
              || c == '.' || c == '-')) {
            return false;
        }
    }

    return true;
}

static bool is_valid_version(const char *v) {
    if (strncmp(v, "HTTP/", 5) != 0) {
        return false;
    }

    const char *p = v + 5;

    if (*p < '0' || *p > '9') {
        return false;
    }

    p++;

    if (*p != '.') {
        return false;
    }

    p++;

    if (*p < '0' || *p > '9') {
        return false;
    }

    p++;

    return *p == '\0';
}

static bool is_valid_header_key(const char *key) {
    size_t len = strlen(key);

    if (len == 0 || len > MAX_HEADER_KEY) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        char c = key[i];

        if (!((c >= 'A' && c <= 'Z')
              || (c >= 'a' && c <= 'z')
              || (c >= '0' && c <= '9')
              || c == '.' || c == '-')) {
            return false;
        }
    }

    return true;
}

static bool is_valid_header_value(const char *value) {
    size_t len = strlen(value);

    if (len == 0 || len > MAX_HEADER_VAL) {
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char) value[i];

        if (c < ' ' || c > '~') {
            return false;
        }
    }

    return true;
}
