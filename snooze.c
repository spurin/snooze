#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

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
    // 1) Start with defaults
    *port = 80;
    *message = "Hello from snooze!";

    // 2) Check environment variables (highest priority)
    const char *env_port = getenv("PORT");
    if (env_port && *env_port) {
        int env_p = atoi(env_port);
        if (env_p > 0) {
            *port = env_p;
        }
    }
    const char *env_message = getenv("MESSAGE");
    if (env_message && *env_message) {
        *message = env_message;
    }

    // 3) Check command-line flags only if environment did NOT supply them
    //    (environment variables override flags)
    for (int i = 1; i < argc; i++) {
        // Format: --port=XXXX
        if (strncmp(argv[i], "--port=", 7) == 0) {
            // Only apply if PORT env wasn't set
            if (!env_port) {
                const char *val = argv[i] + 7;
                int cli_p = atoi(val);
                if (cli_p > 0) {
                    *port = cli_p;
                }
            }
        }
        // Format: --message=XXXX
        else if (strncmp(argv[i], "--message=", 10) == 0) {
            // Only apply if MESSAGE env wasn't set
            if (!env_message) {
                const char *val = argv[i] + 10;
                if (*val) {
                    *message = val;
                }
            }
        }
    }
}

// Minimal HTTP response helper
void send_http_response(int client_sock, const char *message) {
    // Prepare a trivial HTTP response
    char response[1024];
    int content_length = strlen(message);
    int response_len = snprintf(response, sizeof(response),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n"
        "%s",
        content_length, message
    );

    // Send response to client
    if (response_len > 0) {
        write(client_sock, response, response_len);
    }
}

int main(int argc, char *argv[]) {
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
        return 1;
    }

    // Allow reuse of the address
    int optval = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // Bind to the desired port on all interfaces
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return 1;
    }

    // Listen for incoming connections
    if (listen(server_fd, 10) < 0) {
        perror("listen");
        close(server_fd);
        return 1;
    }

    printf("snooze is listening on port %d\n", port);

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
