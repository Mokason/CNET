#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc != 3 || strlen(argv[1]) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return 2;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un addr = {0}; addr.sun_family = AF_UNIX;
    strcpy(addr.sun_path, argv[1]);
    struct timeval timeout = {5, 0};
    if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout) ||
        connect(fd, (struct sockaddr *)&addr, sizeof addr)) return 2;
    size_t n = strlen(argv[2]), sent = 0;
    while (sent < n) { ssize_t k = write(fd, argv[2] + sent, n - sent); if (k <= 0) return 2; sent += (size_t)k; }
    if (write(fd, "\n", 1) != 1) return 2;
    char line[32768]; size_t used = 0;
    while (used < sizeof line - 1) {
        ssize_t k = read(fd, line + used, sizeof line - 1 - used);
        if (k <= 0) return 2;
        used += (size_t)k;
        if (memchr(line, '\n', used)) { line[used] = 0; fputs(line, stdout); close(fd); return 0; }
    }
    return 2;
}
