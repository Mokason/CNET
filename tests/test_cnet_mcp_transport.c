/* Shared MCP client transport regression gate.
 * RED marker: MCP_CLIENT_TRANSPORT_RED
 */
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "../include/cnet_mcp_client.h"

static int fails;
static int shorten_next_write;

ssize_t __real_write(int fd, const void *buf, size_t count);

ssize_t __wrap_write(int fd, const void *buf, size_t count) {
    if (shorten_next_write && count > 8) {
        shorten_next_write = 0;
        count = 8;
    }
    return __real_write(fd, buf, count);
}

static void check(int ok, const char *name) {
    printf("  %-58s %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int start_listener(const char *path) {
    struct sockaddr_un address;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || strlen(path) >= sizeof address.sun_path) {
        if (fd >= 0) close(fd);
        return -1;
    }
    memset(&address, 0, sizeof address);
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path, strlen(path) + 1);
    unlink(path);
    if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        unlink(path);
        return -1;
    }
    return fd;
}

static int send_all(int fd, const char *data, size_t size) {
    size_t sent = 0;
    while (sent < size) {
        ssize_t count = send(fd, data + sent, size - sent, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        sent += (size_t)count;
    }
    return 0;
}

static void partial_write_server(int listener) {
    char request[4096];
    size_t used = 0;
    int client;
    int complete = 0;
    static const char response[] =
        "{\"result\":{\"content\":[{\"type\":\"text\","
        "\"text\":\"complete request\"}]}}\n";

    alarm(3);
    client = accept(listener, NULL, NULL);
    close(listener);
    if (client < 0) _exit(10);
    while (used + 1 < sizeof request) {
        ssize_t count = recv(client, request + used, sizeof request - used - 1, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        used += (size_t)count;
        request[used] = '\0';
        if (memchr(request, '\n', used)) break;
    }
    if (used < sizeof request) request[used] = '\0';
    complete = strstr(request, "partial_write_probe") != NULL &&
               memchr(request, '\n', used) != NULL;
    if (complete)
        (void)send_all(client, response, sizeof response - 1);
    close(client);
    _exit(complete ? 0 : 11);
}

static void stalled_server(int listener) {
    struct timespec pause_time = {0, 350 * 1000 * 1000};
    int client;
    alarm(3);
    client = accept(listener, NULL, NULL);
    close(listener);
    if (client < 0) _exit(20);
    nanosleep(&pause_time, NULL);
    close(client);
    _exit(0);
}

static int finish_child(pid_t child) {
    int status = 0;
    if (waitpid(child, &status, 0) != child) return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

int main(void) {
    char directory[] = "/tmp/cnet_mcp_transport_XXXXXX";
    char socket_path[108];
    char answer[128];
    int listener;
    pid_t child;

    if (!mkdtemp(directory)) {
        perror("mkdtemp");
        return 1;
    }
    if (snprintf(socket_path, sizeof socket_path, "%s/mcp.sock", directory) >=
        (int)sizeof socket_path) {
        rmdir(directory);
        return 1;
    }
    setenv("CNET_MCP_CLIENT", "1", 1);
    setenv("CNET_MCP_SHARED_SOCK", socket_path, 1);

    listener = start_listener(socket_path);
    check(listener >= 0, "create partial-write fake MCP server");
    child = listener >= 0 ? fork() : -1;
    check(child >= 0, "fork partial-write fake MCP server");
    if (child == 0) partial_write_server(listener);
    if (child > 0) {
        int result;
        shorten_next_write = 1;
        result = cnet_mcp_call("partial_write_probe", "{}",
                               answer, sizeof answer);
        shorten_next_write = 0;
        check(result > 0 && strcmp(answer, "complete request") == 0,
              "short first write is completed before reading reply");
        check(finish_child(child), "server received the complete JSON-RPC request");
    }
    if (listener >= 0) close(listener);
    unlink(socket_path);

    listener = start_listener(socket_path);
    check(listener >= 0, "create stalled fake MCP server");
    child = listener >= 0 ? fork() : -1;
    check(child >= 0, "fork stalled fake MCP server");
    if (child == 0) stalled_server(listener);
    if (child > 0) {
        double start;
        double elapsed;
        int result;
        setenv("CNET_MCP_TIMEOUT_MS", "60", 1);
        start = now_ms();
        result = cnet_mcp_call("timeout_probe", "{}", answer, sizeof answer);
        elapsed = now_ms() - start;
        check(result == 0, "stalled MCP response fails closed");
        check(elapsed >= 30.0 && elapsed < 200.0,
              "one configured deadline bounds the MCP exchange");
        check(finish_child(child), "stalled fake MCP server exits cleanly");
    }
    if (listener >= 0) close(listener);
    unlink(socket_path);
    rmdir(directory);

    if (fails) {
        printf("MCP_CLIENT_TRANSPORT_FAIL fails=%d\n", fails);
        return 1;
    }
    printf("MCP_CLIENT_TRANSPORT_PASS full_write bounded_exchange\n");
    return 0;
}
