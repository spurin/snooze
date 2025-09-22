#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <getopt.h>

#define DEFAULT_MESSAGE "Hello from snooze!\n"
#define DEFAULT_PORT    80

static volatile int keep_running = 1;

/*------------------------------------------------------------
 *  Signal handling
 *-----------------------------------------------------------*/
static void handle_signal(int sig) {
    (void)sig;           /* parameter intentionally unused     */
    keep_running = 0;
}

/**
 * Parses command-line arguments of the form:
 *   --port=XXXX
 *   --message=YYYY
 *
 * Precedence order:
 *   1) Environment variables (PORT, MESSAGE) – highest
 *   2) Command-line flags (-p/-m)            – iff env var not set
 *   3) Built-in defaults
 *
 * On --help, prints usage and exits.
 */
static void parse_arguments(int argc, char *argv[],
                            int *port, const char **message)
{
    int opt, env_p = 0;
    const char *env_message = NULL, *env_port = NULL;

    /* 1) Start with defaults */
    *port    = DEFAULT_PORT;
    *message = DEFAULT_MESSAGE;

    /* 2) Environment overrides */
    env_port = getenv("PORT");
    if (env_port != NULL) {
        env_p = atoi(env_port);
        if (env_p > 0) *port = env_p;
    }
    env_message = getenv("MESSAGE");
    if (env_message != NULL) *message = env_message;

    /* 3) Command-line flags (only if env var did NOT override) */
    static const struct option long_opts[] = {
        { "message", required_argument, NULL, 'm' },
        { "port",    required_argument, NULL, 'p' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    while ((opt = getopt_long(argc, argv, "m:p:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'm':
                if (env_message == NULL) *message = optarg;
                break;
            case 'p':
                if (env_p == 0) *port = atoi(optarg);
                break;
            case 'h':
                printf("Usage: %s [OPTIONS]\n\n", argv[0]);
                printf("Options:\n");
                printf("  -m, --message=TEXT  Set the message to send\n");
                printf("  -p, --port=PORT     Set the port to listen on (default: 80)\n");
                printf("  -h, --help          Show this help message\n");
                exit(EXIT_SUCCESS);
            default:
                fprintf(stderr, "use -h or --help for help\n");
                exit(EXIT_FAILURE);
        }
    }
}

/*------------------------------------------------------------
 *  Helpers to read full HTTP request once and log it in ONE
 *  contiguous block. Logging is enabled by default; no limits.
 *
 *  Strategy:
 *    1) Read until "\r\n\r\n" (end of headers), blocking.
 *    2) If Content-Length is present, read exactly that many
 *       body bytes (blocking). Otherwise, log only the headers
 *       and any extra bytes already received.
 *    3) Print a single dump framed by "=== snooze request dump".
 *-----------------------------------------------------------*/
static size_t find_headers_end(const char *buf, size_t len) {
    if (len < 4) return (size_t)0;
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i+1] == '\n' && buf[i+2] == '\r' && buf[i+3] == '\n')
            return i + 4; /* index just past the CRLF CRLF */
    }
    return (size_t)0;
}

static size_t parse_content_length(const char *hdrs, size_t hdr_len) {
    /* simple, case-insensitive scan for "Content-Length:" */
    const char *p = hdrs;
    const char *end = hdrs + hdr_len;
    while (p < end) {
        const char *eol = memchr(p, '\n', (size_t)(end - p));
        if (!eol) eol = end;
        if ((size_t)(eol - p) >= 15 && strncasecmp(p, "Content-Length:", 15) == 0) {
            const char *v = p + 15;
            while (v < eol && (*v == ' ' || *v == '\t')) v++;
            return (size_t)strtoull(v, NULL, 10);
        }
        p = (eol < end) ? eol + 1 : end;
    }
    return 0;
}

static int recv_fully(int fd, char *buf, size_t want) {
    size_t got = 0;
    while (got < want) {
        ssize_t n = recv(fd, buf + got, want - got, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* peer closed early */
        got += (size_t)n;
    }
    return 0;
}

static void log_full_request_blocking(int sock)
{
    /* Step 0: capture peer info for banner */
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    char ip[INET_ADDRSTRLEN] = "unknown";
    int  port = 0;
    if (getpeername(sock, (struct sockaddr*)&peer, &plen) == 0) {
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        port = ntohs(peer.sin_port);
    }

    /* Step 1: read until end of headers */
    size_t cap = 8192;                         /* grows as needed */
    size_t len = 0;
    char *req = (char*)malloc(cap);
    if (!req) return;

    size_t hdr_end = 0;
    for (;;) {
        if (len == cap) {                      /* grow buffer */
            cap *= 2;
            char *tmp = (char*)realloc(req, cap);
            if (!tmp) { free(req); return; }
            req = tmp;
        }
        ssize_t n = recv(sock, req + len, cap - len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            free(req);
            return;
        }
        if (n == 0) break;                     /* peer closed */
        len += (size_t)n;
        hdr_end = find_headers_end(req, len);
        if (hdr_end) break;                    /* have full headers */
    }

    /* Step 2: if Content-Length present, read the rest of the body exactly */
    size_t body_len = 0;
    size_t already_body = 0;
    if (hdr_end) {
        body_len = parse_content_length(req, hdr_end);
        already_body = len > hdr_end ? (len - hdr_end) : 0;

        if (body_len > already_body) {
            size_t need = body_len - already_body;
            /* ensure capacity */
            if (len + need > cap) {
                size_t new_cap = cap;
                while (len + need > new_cap) new_cap *= 2;
                char *tmp = (char*)realloc(req, new_cap);
                if (!tmp) { free(req); return; }
                req = tmp; cap = new_cap;
            }
            if (recv_fully(sock, req + len, need) == -1) {
                /* couldn't complete body; log whatever we have */
                need = 0;
            }
            len += need;
        }
    }

    /* Step 3: single clean dump */
    fprintf(stderr, "=== snooze request dump from %s:%d ===\n", ip, port);
    (void)fwrite(req, 1, len, stderr);
    if (len == 0) fputc('\n', stderr);  /* ensure a blank line block if nothing */
    fprintf(stderr, "=== end request dump ===\n");
    fflush(stderr);

    free(req);
}

/*------------------------------------------------------------
 *  Robust send() helper
 *     – loops until every byte is written or an error occurs
 *-----------------------------------------------------------*/
static int send_all(int sock, const char *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(sock, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)                /* interrupted → retry   */
                continue;
            return -1;                         /* hard error            */
        }
        if (n == 0)                            /* peer closed early     */
            return -1;
        sent += (size_t)n;
    }
    return 0;                                  /* success               */
}

/*------------------------------------------------------------
 *  graceful_close()
 *
 *  Half-close for write, drain unread data quietly (no logging),
 *  then close. Avoids TCP RST/ERR_CONTENT_LENGTH_MISMATCH.
 *-----------------------------------------------------------*/
static void graceful_close(int sock)
{
    shutdown(sock, SHUT_WR);

    char buf[256];
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0) continue;
        if (n == 0) break;
        if (errno == EAGAIN || errno == EWOULDBLOCK) break;
        if (errno == EINTR) continue;
        break;
    }

    close(sock);
}

/**
 * Minimal HTTP response helper
 */
void send_http_response(int client_sock, const char *message)
{
    const size_t body_len = strlen(message);

    char header[256];
    int hdr_len = snprintf(header, sizeof(header),
        "HTTP/1.1 200 OK\r\n"
        "Server: snooze\r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        body_len);

    if (hdr_len < 0 || (size_t)hdr_len >= sizeof(header)) {
        fprintf(stderr, "header buffer too small\n");
        graceful_close(client_sock);
        return;
    }

    if (send_all(client_sock, header, (size_t)hdr_len) == -1 ||
        send_all(client_sock, message, body_len)        == -1) {
        graceful_close(client_sock);
        return;
    }

    graceful_close(client_sock);
}

/*------------------------------------------------------------
 *  Main server loop
 *-----------------------------------------------------------*/
int main(int argc, char *argv[])
{
    int port, ret;
    const char *message;

    /* Parse environment variables and CLI flags */
    parse_arguments(argc, argv, &port, &message);

    /* Set up signals */
    struct sigaction sa = { .sa_handler = handle_signal };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* Create listening socket */
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) { perror("socket"); exit(EXIT_FAILURE); }

    /* Allow immediate re-bind after restart */
    int optval = 1;
    ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR,
                     &optval, sizeof(optval));
    if (ret < 0) { perror("setsockopt"); close(server_fd); exit(EXIT_FAILURE); }

    /* Bind to all interfaces on the chosen port */
    struct sockaddr_in addr = {0};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); close(server_fd); exit(EXIT_FAILURE);
    }

    if (listen(server_fd, 10) < 0) {
        perror("listen"); close(server_fd); exit(EXIT_FAILURE);
    }

    printf("snooze is listening on port %d\n", port);

    /*--------------------------------------------------------
     *  Accept–loop: one connection at a time (trivial server)
     *-------------------------------------------------------*/
    while (keep_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) break;
            perror("accept");
            continue;
        }

        /* ONE clean block with the full request (headers + body if Content-Length). */
        log_full_request_blocking(client_fd);

        /* Respond and close. */
        send_http_response(client_fd, message);
    }

    /* Clean up */
    close(server_fd);
    printf("snooze received stop signal; shutting down...\n");
    return 0;
}
