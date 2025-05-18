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
#include <time.h>
#include <sys/time.h>
#include <stdarg.h>
#include <ctype.h> // Add this for isdigit

#define DEFAULT_MESSAGE "Hello from snooze!\n"
#define DEFAULT_PORT    80

static volatile int keep_running = 1;

static void handle_signal(int sig) {
    (void)sig; // Unused parameter
    keep_running = 0;
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
static void log_msg(log_level_t lvl, const char *fmt, ...) {
    if (lvl > current_log_level) return;
    const char *lvl_str = (lvl == LOG_ERROR) ? "error" : (lvl == LOG_DEBUG) ? "debug" : "info";
    va_list args;
    va_start(args, fmt);
    fprintf((lvl == LOG_ERROR) ? stderr : stdout, "{\"level\":\"%s\",", lvl_str);
    vfprintf((lvl == LOG_ERROR) ? stderr : stdout, fmt, args);
    fprintf((lvl == LOG_ERROR) ? stderr : stdout, "}\n");
    va_end(args);
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
    const char *env_loglevel = NULL;
    const char *cli_loglevel = NULL;

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

    env_loglevel = getenv("LOG_LEVEL");
    if (env_loglevel != NULL) {
        current_log_level = parse_log_level(env_loglevel);
    }

    static const struct option long_opts[] = {
        { "message",  required_argument, NULL, 'm' },
        { "port",     required_argument, NULL, 'p' },
        { "loglevel", required_argument, NULL, 'l' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    // 3) Check command-line flags only if environment did NOT supply them
    while ((opt = getopt_long(argc, argv, "m:p:l:h", long_opts, NULL)) != -1) {
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
            case 'l':
                cli_loglevel = optarg;
                break;
            case 'h':
                printf("Usage: %s [OPTIONS]\n\n", argv[0]);
                printf("Options:\n");
                printf("  -m, --message=TEXT   Set the message to send\n");
                printf("  -p, --port=PORT      Set the port to listen on (default: 80)\n");
                printf("  -l, --loglevel=LEVEL Set log level (error, info, debug)\n");
                printf("  -h, --help           Show this help message\n");
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

// Helper to extract HTTP method from request
static void extract_method(const char *req, char *method, size_t len) {
    size_t i = 0;
    while (req[i] && req[i] != ' ' && i < len - 1) {
        method[i] = req[i];
        i++;
    }
    method[i] = '\0';
}

// Extracts the User-Agent header from the HTTP request buffer.
// Returns "unknown" if not found.
static void extract_user_agent(const char *reqbuf, char *agent, size_t agent_size) {
    const char *ua = reqbuf;
    const char *end = reqbuf + strlen(reqbuf);
    int found = 0;
    while (ua < end) {
        // Find the start of a line
        const char *line_end = strstr(ua, "\r\n");
        if (!line_end) line_end = end;
        // Case-insensitive search for User-Agent:
        if ((size_t)(line_end - ua) > 11 && strncasecmp(ua, "User-Agent:", 11) == 0) {
            ua += 11;
            while (*ua == ' ' || *ua == '\t') ua++;
            size_t i = 0;
            while (ua[i] && ua[i] != '\r' && ua[i] != '\n' && i < agent_size - 1) {
                agent[i] = ua[i];
                i++;
            }
            agent[i] = '\0';
            found = 1;
            break;
        }
        if (line_end == end) break;
        ua = line_end + 2; // Move to next line
    }
    if (!found) {
        strncpy(agent, "unknown", agent_size);
        agent[agent_size - 1] = '\0';
    }
}

// Extracts the path from the HTTP request line.
// Returns "/" if not found.
static void extract_path(const char *reqbuf, char *path, size_t path_size) {
    char *sp1 = strchr(reqbuf, ' ');
    if (sp1) {
        sp1++;
        char *sp2 = strchr(sp1, ' ');
        if (sp2 && (sp2 - sp1) < (int)path_size) {
            memcpy(path, sp1, sp2 - sp1);
            path[sp2 - sp1] = '\0';
            return;
        }
    }
    strncpy(path, "/", path_size);
    path[path_size - 1] = '\0';
}

// Helper to check if path matches /snooze/<number> and extract number
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

int main(int argc, char *argv[]) {
    setvbuf(stdout, NULL, _IONBF, 0); // Unbuffered output for stdout
    setvbuf(stderr, NULL, _IONBF, 0); // Unbuffered output for stderr
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
        log_msg(LOG_ERROR, "\"msg\":\"socket error: %s\"", strerror(errno));
        exit(EXIT_FAILURE);
    }

    // Allow reuse of the address
    int optval = 1;
    ret = setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    if (ret < 0) {
        log_msg(LOG_ERROR, "\"msg\":\"setsockopt error: %s\"", strerror(errno));
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
        log_msg(LOG_ERROR, "\"msg\":\"bind error: %s\"", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Listen for incoming connections
    if (listen(server_fd, 10) < 0) {
        log_msg(LOG_ERROR, "\"msg\":\"listen error: %s\"", strerror(errno));
        close(server_fd);
        exit(EXIT_FAILURE);
    }

    // Before main server loop
    time_t start_now = time(NULL);
    char start_timebuf[64];
    struct tm start_tm;
    localtime_r(&start_now, &start_tm);
    strftime(start_timebuf, sizeof(start_timebuf), "%Y-%m-%dT%H:%M:%S%z", &start_tm);

    log_msg(LOG_INFO, "\"msg\":\"snooze is listening on port %d\",\"ts\":\"%s\"", port, start_timebuf);

    // Main server loop
    while (keep_running) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            // If interrupted by signal, just break
            if (errno == EINTR) {
                break;
            }
            log_msg(LOG_ERROR, "\"msg\":\"accept error: %s\"", strerror(errno));
            continue;
        }

        struct timeval start, end;
        gettimeofday(&start, NULL);

        // Add this block to define and fill timebuf
        time_t now = time(NULL);
        char timebuf[64];
        struct tm tm;
        localtime_r(&now, &tm);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%dT%H:%M:%S%z", &tm);

        // Read HTTP request
        char reqbuf[1024] = {0}; // Increase buffer size
        size_t total = 0;
        ssize_t n;
        while ((n = recv(client_fd, reqbuf + total, sizeof(reqbuf) - 1 - total, 0)) > 0) {
            total += n;
            reqbuf[total] = '\0';
            if (strstr(reqbuf, "\r\n\r\n")) break; // End of headers
            if (total >= sizeof(reqbuf) - 1) break; // Buffer full
        }

        char method[16] = "UNKNOWN";
        if (n > 0) {
            extract_method(reqbuf, method, sizeof(method));
        }

        // Extract User-Agent header from request
        char agent[128];
        extract_user_agent(reqbuf, agent, sizeof(agent));

        // Extract path from request line
        char path[128];
        extract_path(reqbuf, path, sizeof(path));

        int snooze_sec = 0;
        if (parse_snooze_path(path, &snooze_sec) && snooze_sec > 0) {
            sleep(snooze_sec);
            gettimeofday(&end, NULL);
            double exec_len = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;
            log_msg(LOG_INFO,
                "\"method\":\"%s\",\"uri\":\"%s\",\"ts\":\"%s\",\"exec_time\":\"%.4f\",\"agent\":\"%s\"",
                method, path, timebuf, exec_len, agent);
            char snooze_msg[128];
            snprintf(snooze_msg, sizeof(snooze_msg), "Snoozed for %d seconds!\n", snooze_sec);
            send_http_response(client_fd, snooze_msg);
        } else {
            gettimeofday(&end, NULL);
            double exec_len = (end.tv_sec - start.tv_sec) + (end.tv_usec - start.tv_usec) / 1e6;
            log_msg(LOG_INFO,
                "\"method\":\"%s\",\"uri\":\"%s\",\"ts\":\"%s\",\"exec_time\":\"%.4f\",\"agent\":\"%s\"",
                method, path, timebuf, exec_len, agent);
            send_http_response(client_fd, message);
        }
        close(client_fd);
    }

    // Clean up
    time_t stop_now = time(NULL);
    char stop_timebuf[64];
    struct tm stop_tm;
    localtime_r(&stop_now, &stop_tm);
    strftime(stop_timebuf, sizeof(stop_timebuf), "%Y-%m-%dT%H:%M:%S%z", &stop_tm);

    log_msg(LOG_INFO, "\"msg\":\"snooze received stop signal; shutting down...\",\"ts\":\"%s\"", stop_timebuf);
    return 0;
}
