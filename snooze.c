#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
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
 *  Why?  If we close() a socket that still contains unread
 *  *incoming* data (the remainder of the client’s HTTP request),
 *  Linux sends a TCP RST instead of a FIN.  Browsers then report
 *  ERR_CONTENT_LENGTH_MISMATCH even though we transmitted the
 *  whole body.  The fix is:
 *
 *    1.  shutdown(…, SHUT_WR)  – we’re done sending
 *    2.  Drain any leftover bytes with non-blocking recv()
 *    3.  close()
 *-----------------------------------------------------------*/
static void graceful_close(int sock)
{
    shutdown(sock, SHUT_WR);          /* half-close: no more data to send */

    char buf[256];
    for (;;) {
        ssize_t n = recv(sock, buf, sizeof(buf), MSG_DONTWAIT);
        if (n > 0)                      /* still reading – discard & loop */
            continue;
        if (n == 0)                     /* peer closed his side           */
            break;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;                      /* nothing left to read           */
        if (errno == EINTR)
            continue;                   /* interrupted – retry            */
        break;                          /* any other error – just quit    */
    }

    close(sock);
}

/**
 * Minimal HTTP response helper
 *
 *  • Handles arbitrary-length bodies (taken from MESSAGE env-var or CLI).
 *  • Sets an accurate Content-Length header.
 *  • Sends headers first, then body, using send_all() to survive partial
 *    writes on slow connections.
 *  • Keeps UTF-8 intact – no truncation.
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

    /* 1) send headers, 2) send body, 3) close politely         */
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
            /* If interrupted by a signal (e.g., Ctrl-C), break cleanly */
            if (errno == EINTR) break;
            perror("accept");
            continue;
        }

        /* Send minimal HTTP response and close */
        send_http_response(client_fd, message);   /* graceful_close() inside */
    }

    /* Clean up */
    close(server_fd);
    printf("snooze received stop signal; shutting down...\n");
    return 0;
}
