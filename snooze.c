/*------------------------------------------------------------
 * Snooze - HTTP Server Implementation Details
 *
 * PURPOSE:
 * Minimal, single-threaded HTTP server useful for testing client-side
 * behavior under variable response latency. This header contains
 * implementation notes for developers. Usage and examples are in README.md.
 *
 * DESIGN HIGHLIGHTS:
 * - Single-threaded accept loop for simplicity and deterministic timing
 * - Single-pass header parsing to minimize allocations and copies
 * - Graceful socket shutdown to avoid TCP RST and browser errors
 * - Structured JSON logging for easy ingestion by logging systems
 *
 * OPERATION:
 * - Endpoint: GET /snooze/N  -> delays response by N seconds
 * - Any other path -> returns configured message
 *
 * LOGGING:
 * Logs are emitted as JSON objects with these base fields:
 *  - ts: ISO8601 timestamp
 *  - level: error|info|debug
 *  - subsystem: logical subsystem (e.g., "net", "app", "http")
 *  - exec_time: request processing time in seconds (numeric)
 * Additional fields for each log entry are appended to this base object.
 *
 * SAFETY & STYLE:
 * - Fixed-size buffers are used with careful bounds checks
 * - No code snippets should be embedded in comments; see README.md for
 *   usage/docs, and this file for implementation details.
 *
 *------------------------------------------------------------*/

/* System headers */
#include <sys/socket.h>  // socket, bind, listen, accept
#include <netinet/in.h>  // sockaddr_in
#include <unistd.h>      // close, read, write
#include <signal.h>      // sigaction

/* Standard C headers */
#include <stdlib.h>      // exit, atoi
#include <string.h>      // str* functions
#include <stdio.h>       // printf, fprintf
#include <getopt.h>      // getopt_long
#include <time.h>        // time, localtime
#include <sys/time.h>    // gettimeofday
#include <stdarg.h>      // va_list
#include <ctype.h>       // isdigit
#include <errno.h>       // errno

#define DEFAULT_MESSAGE "Hello from snooze!\n"
#define DEFAULT_PORT    80
#define MAX_HEADERS_SIZE 1536
#define MAX_METHOD_SIZE 16
#define MAX_PATH_SIZE 128
#define MAX_AGENT_SIZE 128
#define MAX_REQ_SIZE 1024
#define MAX_TIME_SIZE 64

static volatile int keep_running = 1;

/*------------------------------------------------------------
 * Signal handling
 *-----------------------------------------------------------*/
static void handle_signal(int sig) {
    (void)sig;           /* parameter intentionally unused     */
    keep_running = 0;
}

// Function declarations
static const char* format_log_time(void);

/* Program start time (seconds since epoch, with microsecond precision) */
static double program_start = 0.0;

/* Shutdown request time */
static double shutdown_start = 0.0;

/* Return current time in seconds as double */
static double now_seconds(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
}

// 1. Define log levels
typedef enum { LOG_ERROR = 0, LOG_INFO = 1, LOG_DEBUG = 2 } log_level_t;

static log_level_t current_log_level = LOG_INFO;

// 2. Log level string to enum
static log_level_t parse_log_level(const char *lvl) {
    if (!lvl) return LOG_INFO;
    if (strcasecmp(lvl, "debug") == 0) return LOG_DEBUG;
    if (strcasecmp(lvl, "info") == 0) return LOG_INFO;
    if (strcasecmp(lvl, "error") == 0) return LOG_ERROR;
    return LOG_INFO;
}

// 3. Logging function
static void log_msg(log_level_t lvl, const char *subsystem, double exec_time, const char *fmt, ...) {
    if (lvl > current_log_level) return;
    FILE* out = (lvl == LOG_ERROR) ? stderr : stdout;
    const char *level = (lvl == LOG_ERROR) ? "error" : (lvl == LOG_DEBUG) ? "debug" : "info";

    va_list args;
    va_start(args, fmt);

    // Emit fixed prefix: timestamp, level, subsystem, exec_time (numeric)
    // exec_time formatted as string with 4 decimal places (seconds.microseconds)
    fprintf(out, "{\"ts\":\"%s\",\"level\":\"%s\",\"subsystem\":\"%s\",\"exec_time\":\"%.4f\"",
            format_log_time(), level, subsystem, exec_time);

    if (fmt && *fmt) {
        fprintf(out, ",");
        vfprintf(out, fmt, args);
    }

    fprintf(out, "}\n");
    va_end(args);
}

/**
 * Parses command-line arguments of the form:
 *   --port=XXXX
 *   --message=YYYY
 *
 * Precedence order:
 * 1) Environment variables (PORT, MESSAGE) - highest
 * 2) Command-line flags (-p/-m) - if env var not set
 * 3) Built-in defaults
 *
 * On --help, prints usage and exits.
 */
static void parse_arguments(int argc, char *argv[],
                           int *port, const char **message)
{
    int opt, env_p = 0;
    const char *env_message = NULL, *env_port = NULL;
    const char *env_loglevel = NULL;
    const char *cli_loglevel = NULL;

    /* 1) Start with defaults */
    *port = DEFAULT_PORT;
    *message = DEFAULT_MESSAGE;

    /* 2) Environment overrides */
    env_port = getenv("PORT");
    if (env_port != NULL) {
        env_p = atoi(env_port);
        if (env_p > 0) *port = env_p;
    }

    env_message = getenv("MESSAGE");
    if (env_message != NULL) *message = env_message;

    env_loglevel = getenv("LOG_LEVEL");
    if (env_loglevel != NULL) {
        current_log_level = parse_log_level(env_loglevel);
    }

    /* 3) Command-line flags (only if env var did NOT override) */
    static const struct option long_opts[] = {
        { "message", required_argument, NULL, 'm' },
        { "port", required_argument, NULL, 'p' },
        { "loglevel", required_argument, NULL, 'l' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "m:p:l:h", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'm':
            if (env_message == NULL) *message = optarg;
            break;
        case 'p':
            if (env_p == 0) *port = atoi(optarg);
            break;
        case 'l':
            cli_loglevel = optarg;
            break;
        case 'h':
            printf("Usage: %s [OPTIONS]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -m, --message=TEXT    Set the message to send\n");
            printf("  -p, --port=PORT       Set the port to listen on (default: 80)\n");
            printf("  -l, --loglevel=LEVEL  Set log level (error, info, debug)\n");
            printf("  -h, --help            Show this help message\n");
            exit(EXIT_SUCCESS);
        default:
            fprintf(stderr, "use -h or --help for help\n");
            exit(EXIT_FAILURE);
        }
    }

    // CLI loglevel overrides env
    if (cli_loglevel) {
        current_log_level = parse_log_level(cli_loglevel);
    }
}

/*------------------------------------------------------------
 * Robust send() helper
 * - loops until every byte is written or an error occurs
 *-----------------------------------------------------------*/
static int send_all(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) /* interrupted â†’ retry */
                continue;
            return -1; /* hard error */
        }
        if (n == 0) /* peer closed early */
            return -1;
        sent += (size_t)n;
    }
    return 0; /* success */
}

/*------------------------------------------------------------
 * graceful_close()
 *
 * Why? If we close() a socket that still contains unread
 * *incoming* data (the remainder of the client's HTTP request),
 * Linux sends a TCP RST instead of a FIN. Browsers then report
 * ERR_CONTENT_LENGTH_MISMATCH even though we transmitted the
 * whole body. The fix is:
 *
 * 1. shutdown(…, SHUT_WR) - we're done sending
 * 2. Drain any leftover bytes with non-blocking recv()
 * 3. close()
 *-----------------------------------------------------------*/
static void graceful_close(int sock)
{
    shutdown(sock, SHUT_WR); /* half-close: no more data to send */

    char buf[256];
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) /* still reading - discard & loop */
            continue;
        if (n == 0) /* peer closed his side */
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break; /* nothing left to read */
        if (errno == EINTR)
            continue; /* interrupted - retry */
        break; /* any other error - just quit */
    }

    close(sock);
}

/**
 * Minimal HTTP response helper
 *
 * - Handles arbitrary-length bodies (taken from MESSAGE env-var or CLI).
 * - Sets an accurate Content-Length header.
 * - Sends headers first, then body, using send_all() to survive partial
 *   writes on slow connections.
 * - Keeps UTF-8 intact - no truncation.
 */
void send_http_response(int client_sock, const char *message)
{
    const size_t body_len = strlen(message);

    /* Build ONLY the header in a small stack buffer */
    char header[256];
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Server: snooze\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);

    /* snprintf() returns the would-be length (excluding \0). */
    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(header)) {
        fprintf(stderr, "header buffer too small\n");
        graceful_close(client_sock);
        return;
    }

    /* 1) send headers, 2) send body, 3) close politely */
    if (send_all(client_sock, header, (size_t)hdr_len) == -1 ||
        send_all(client_sock, message, body_len) == -1) {
        graceful_close(client_sock);
        return;
    }

    graceful_close(client_sock);
}

// HTTP request structure to consolidate parsing results
typedef struct {
    char method[MAX_METHOD_SIZE];
    char path[MAX_PATH_SIZE];
    char user_agent[MAX_AGENT_SIZE];
    char additional_headers[MAX_HEADERS_SIZE];
    int snooze_seconds;
    double exec_time;
} http_request_t;

/* Function declarations */
static void parse_headers(const char *reqbuf, http_request_t *req);
static int parse_snooze_path(const char *path, int *timesec);
static void log_request(const http_request_t *req);

// Common log message formatter
static void log_request(const http_request_t *req) {
    char attrs[MAX_HEADERS_SIZE + 256];
    snprintf(attrs, sizeof(attrs),
        "\"method\":\"%s\",\"path\":\"%s\",\"agent\":\"%s\"%s%s",
        req->method, req->path, req->user_agent,
        req->additional_headers[0] ? "," : "",
        req->additional_headers[0] ? req->additional_headers : "");

    // Pass exec_time so log_msg includes it in the fixed prefix
    log_msg(LOG_INFO, "http", req->exec_time, "%s", attrs);
}

/*------------------------------------------------------------
 * extract_request_info()
 *
 * Consolidated request parsing:
 * - HTTP method
 * - Path
 * - User-Agent
 * - Additional headers
 * - Snooze duration (if /snooze/ endpoint was hit)
 *-----------------------------------------------------------*/
static void extract_request_info(const char *reqbuf, http_request_t *req) {
    // Initialize request structure
    memset(req, 0, sizeof(http_request_t));
    req->snooze_seconds = 0; // Default to no snooze

    // Parse all headers and request line in one pass
    parse_headers(reqbuf, req);

    // Check for snooze path
    parse_snooze_path(req->path, &req->snooze_seconds);
}

/*------------------------------------------------------------
 * handle_request()
 *
 * Central request handler:
 * - Parses request
 * - Logs request details
 * - Sends appropriate response
 *-----------------------------------------------------------*/
static void handle_request(int client_fd, const http_request_t *req, const char *default_message) {
    if (req->snooze_seconds > 0) {
        // Handle snooze request
        sleep(req->snooze_seconds);

        char snooze_msg[128];
        snprintf(snooze_msg, sizeof(snooze_msg), "Snoozed for %d seconds!\n", req->snooze_seconds);
        send_http_response(client_fd, snooze_msg);
    } else {
        // Handle normal request
        send_http_response(client_fd, default_message);
    }
}

/*------------------------------------------------------------
 * Single-pass HTTP header parser
 * Extracts method, path, user-agent and other headers in one go
 *-----------------------------------------------------------*/
static void parse_headers(const char *reqbuf, http_request_t *req) {
    const char *cur = reqbuf;
    const char *end = reqbuf + strlen(reqbuf);
    char temp_buffer[2048] = {0};
    size_t json_pos = 0;
    int is_first_line = 1;

    while (cur < end) {
        const char *line_end = strstr(cur, "\r\n");
        if (!line_end) line_end = end;
        if (line_end == cur) break; // Empty line means end of headers

        if (is_first_line) {
            // First line format: METHOD /path HTTP/1.1
            const char *first_space = memchr(cur, ' ', line_end - cur);
            const char *second_space = first_space ? memchr(first_space + 1, ' ', line_end - first_space - 1) : NULL;

            if (first_space && second_space) {
                size_t method_len = first_space - cur;
                size_t path_len = second_space - (first_space + 1);

                if (method_len < sizeof(req->method)) {
                    memcpy(req->method, cur, method_len);
                    req->method[method_len] = '\0';
                }
                if (path_len < sizeof(req->path)) {
                    memcpy(req->path, first_space + 1, path_len);
                    req->path[path_len] = '\0';
                }
            }
            is_first_line = 0;

        } else {
            // Parse headers - format: Name: Value
            const char *colon = memchr(cur, ':', line_end - cur);
            if (colon && colon < line_end) {
                const size_t name_len = colon - cur;
                const char *value_start = colon + 1;
                while (value_start < line_end && (*value_start == ' ' || *value_start == '\t'))
                    value_start++;
                const size_t value_len = line_end - value_start;

                // Process header based on name
                if (name_len == 10 && strncasecmp(cur, "User-Agent", 10) == 0) {
                    if (value_len < sizeof(req->user_agent)) {
                        memcpy(req->user_agent, value_start, value_len);
                        req->user_agent[value_len] = '\0';
                    }
                } else {
                    // Add to JSON buffer
                    if (json_pos > 0 && json_pos < sizeof(temp_buffer) - 1) {
                        temp_buffer[json_pos++] = ',';
                    }

                    json_pos += snprintf(temp_buffer + json_pos,
                        sizeof(temp_buffer) - json_pos,
                        "\"%.*s\":\"%.*s\"",
                        (int)name_len, cur,
                        (int)value_len, value_start);

                    if (json_pos >= sizeof(temp_buffer) - 100) break;
                }
            }
        }

        if (line_end == end) break;
        cur = line_end + 2;
    }

    // Set defaults and finalize
    if (!req->method[0]) strncpy(req->method, "GET", sizeof(req->method) - 1);
    if (!req->path[0]) strncpy(req->path, "/", sizeof(req->path) - 1);
    if (!req->user_agent[0]) strncpy(req->user_agent, "unknown", sizeof(req->user_agent) - 1);
    strncpy(req->additional_headers, temp_buffer, sizeof(req->additional_headers) - 1);
}

// Helper to check if path matches /snooze/ and extract number
static int parse_snooze_path(const char *path, int *timesec) {
    if (strncmp(path, "/snooze/", 8) == 0) {
        const char *numstr = path + 8;
        if (*numstr) {
            // Ensure all chars are digits
            for (const char *p = numstr; *p; ++p) {
                if (!isdigit((unsigned char)*p)) return 0;
            }
            *timesec = atoi(numstr);
            return 1;
        }
    }
    return 0;
}

// Format time for logs in ISO8601 format
static const char* format_log_time(void) {
    static char timebuf[64];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S%z", &tm);
    return timebuf;
}

/*------------------------------------------------------------
 * Helper function to extract HTTP header value
 *-----------------------------------------------------------*/
static int extract_header_value(const char *reqbuf, const char *header_name, char *value, size_t value_size) {
    const char *line = reqbuf;
    const char *end = reqbuf + strlen(reqbuf);
    size_t header_len = strlen(header_name);

    while (line < end) {
        const char *line_end = strstr(line, "\r\n");
        if (!line_end) line_end = end;

        if ((size_t)(line_end - line) > header_len &&
            strncasecmp(line, header_name, header_len) == 0) {
            const char *val_start = line + header_len;
            while (val_start < line_end && (*val_start == ' ' || *val_start == '\t'))
                val_start++;

            size_t val_len = line_end - val_start;
            if (val_len >= value_size) val_len = value_size - 1;
            memcpy(value, val_start, val_len);
            value[val_len] = '\0';
            return 1;
        }

        if (line_end == end) break;
        line = line_end + 2;
    }

    return 0;
}

/*------------------------------------------------------------
 * Main server loop
 *-----------------------------------------------------------*/
int main(int argc, char *argv[])
{
    setvbuf(stdout, NULL, _IONBF, 0); // Unbuffered output for stdout
    setvbuf(stderr, NULL, _IONBF, 0); // Unbuffered output for stderr

    // Record program start time used for exec_time calculation
    program_start = now_seconds();

    int port, ret;
    const char *message;

    /* Parse environment variables and CLI flags */
    parse_arguments(argc, argv, &port, &message);

    /* Set up signals */
    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Create listening socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        log_msg(LOG_ERROR, "net", now_seconds() - program_start, "\"op\":\"socket\",\"error\":\"%s\"", strerror(errno));
        exit(EXIT_FAILURE);
    }

    /* Allow immediate re-bind after restart */
    int optval = 1;
    ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                     &optval, sizeof(optval));
    if (ret < 0) {
        log_msg(LOG_ERROR, "net", now_seconds() - program_start, "\"op\":\"setsockopt\",\"error\":\"%s\"", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    /* Bind to all interfaces on the chosen port */
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        log_msg(LOG_ERROR, "net", now_seconds() - program_start, "\"op\":\"bind\",\"error\":\"%s\"", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        log_msg(LOG_ERROR, "net", now_seconds() - program_start, "\"op\":\"listen\",\"error\":\"%s\"", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Before main server loop
    log_msg(LOG_INFO, "app", now_seconds() - program_start, "\"op\":\"start\",\"port\":%d", port);

    /*--------------------------------------------------------
     * Accept-loop: one connection at a time (trivial server)
     *-------------------------------------------------------*/
    int shutdown_logged = 0;
    for (;;) {
        if (!keep_running) {
            if (!shutdown_logged) {
                // Record time of shutdown request
                shutdown_start = now_seconds();
                // Log the moment shutdown was requested (elapsed since program start)
                log_msg(LOG_INFO, "app", shutdown_start - program_start, "\"op\":\"shutdown_requested\"");
                shutdown_logged = 1;
            }
            break;
        }

        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            /* If interrupted by a signal (e.g., Ctrl-C), continue to loop so we can detect shutdown */
            if (errno == EINTR) continue;
            log_msg(LOG_ERROR, "net", now_seconds() - program_start, "\"op\":\"accept\",\"error\":\"%s\"", strerror(errno));
            continue;
        }

        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Read HTTP request
        char reqbuf[MAX_REQ_SIZE] = {0};
        size_t total = 0;
        ssize_t n;

        while ((n = recv(client_fd, reqbuf + total, sizeof(reqbuf) - 1 - total, 0)) > 0) {
            total += n;
            reqbuf[total] = '\0';
            if (strstr(reqbuf, "\r\n\r\n")) break; // End of headers
            if (total >= sizeof(reqbuf) - 1) break; // Buffer full
        }

        http_request_t request;
        extract_request_info(reqbuf, &request);

        // Handle the request (includes any snooze delay)
        handle_request(client_fd, &request, message);

        // Calculate total execution time (seconds.microseconds)
        gettimeofday(&end, NULL);
        request.exec_time = (double)(end.tv_sec - start.tv_sec) + (double)(end.tv_usec - start.tv_usec) / 1000000.0;
        log_request(&request);

        /* Clean up */
        close(client_fd);
    }

    /* Clean up */
    close(server_fd);

    if (shutdown_start != 0.0) {
        // Log final shutdown completed with duration to handle shutdown
        double shutdown_duration = now_seconds() - shutdown_start;
        log_msg(LOG_INFO, "app", shutdown_duration, "\"op\":\"shutdown\"",
                NULL);
    }
    return 0;
}