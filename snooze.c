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

static void handle_signal(int sig) {
    (void)sig; // Unused parameter
    keep_running = 0;
}

/**
 * Parses command-line arguments of the form:
 *   --port=XXXX
 *   --message=YYYY
 *
 * Respects environment variables first (PORT, MESSAGE),
 * then falls back to command-line flags if the
 * corresponding environment variable isn't set.
 * If neither is provided, defaults are used.
 */
static void parse_arguments(int argc, char *argv[], int *port, const char **message) {
    int opt;
    int env_p = 0;
    const char *env_message = NULL;
    const char *env_port = NULL;

    // 1) Start with defaults
    *port = DEFAULT_PORT;
    *message = DEFAULT_MESSAGE;

    // 2) Check environment variables (highest priority)
    env_port = getenv("PORT");
    if (env_port != NULL) {
        env_p = atoi(env_port);
        if (env_p > 0) {
            *port = env_p;
        }
    }

    env_message = getenv("MESSAGE");
    if (env_message != NULL) {
        *message = env_message;
    }

    /* Setup long-options */
    static const struct option long_opts[] = {
        { "message", required_argument, NULL, 'm' },
        { "port",    required_argument, NULL, 'p' },
        { "help",    no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    // 3) Check command-line flags only if environment did NOT supply them
    //    (environment variables override flags)
    while ((opt = getopt_long(argc, argv, "m:p:h", long_opts, NULL)) != -1) {
        switch (opt) {
            case 'm':
                if (env_message == NULL) {
                    *message = optarg;
                }
                break;
            case 'p':
                if (env_p == 0) {
                    *port = atoi(optarg);
                }
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


    // // 3) Check command-line flags only if environment did NOT supply them
    // //    (environment variables override flags)
    // for (int i = 1; i < argc; i++) {
    //     // Format: --port=XXXX
    //     if (strncmp(argv[i], "--port=", 7) == 0) {
    //         // Only apply if PORT env wasn't set
    //         if (!env_port) {
    //             const char *val = argv[i] + 7;
    //             int cli_p = atoi(val);
    //             if (cli_p > 0) {
    //                 *port = cli_p;
    //             }
    //         }
    //     }
    //     // Format: --message=XXXX
    //     else if (strncmp(argv[i], "--message=", 10) == 0) {
    //         // Only apply if MESSAGE env wasn't set
    //         if (!env_message) {
    //             const char *val = argv[i] + 10;
    //             if (*val) {
    //                 *message = val;
    //             }
    //         }
    //     }
    // }
}

// Minimal HTTP response helper
void send_http_response(int client_sock, const char *message) {
    // Prepare a trivial HTTP response
    char response[1024];
    int content_length = strlen(message);
    int response_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Server: snooze\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        content_length, message
    );

    // Send response to client
    if (response_len > 0) {
        send(client_sock, response, response_len, MSG_NOSIGNAL);
    }
}

int main(int argc, char *argv[]) {
    int ret;
    int port;
    const char *message;

    // Parse environment variables and CLI flags
    parse_arguments(argc, argv, &port, &message);

    // Set up signals
    struct sigaction sa;
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    // Create socket
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        exit(EXIT_FAILURE);
    }

    // Allow reuse of the address
    int optval = 1;
    ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (ret < 0) {
        perror("setsockopt");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Bind to the desired port on all interfaces
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    fprintf(stdout, "snooze is listening on port %d\n", port);

    // Main server loop
    while (keep_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            // If interrupted by signal, just break
            if (errno == EINTR) {
                break;
            }
            perror("accept");
            continue;
        }

        // Send minimal HTTP response
        send_http_response(client_fd, message);
        close(client_fd);
    }

    // Clean up
    close(server_fd);
    printf("snooze received stop signal; shutting down...\n");
    return 0;
}
