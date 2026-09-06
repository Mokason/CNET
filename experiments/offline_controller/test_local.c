#define main baseline_main
#include "local_baseline.c"
#undef main
int main(void) {
    assert(content_action("{\"content\":\"3\"}")==3);
    assert(content_action("{\"content\":\"8\",\"nested\":{\"content\":\"0\"}}")==8);
    const char *invalid[]={"{}","{\"content\":3}","{\"content\":\"9\"}",
        "{\"content\":\"3\",\"content\":\"1\"}","{\"content\":\"1 2\"}",
        "{\"content\":\"1\"} trailing","{\"nested\":{\"content\":\"1\"}}"};
    for (size_t i=0;i<sizeof invalid/sizeof *invalid;i++) assert(content_action(invalid[i])<0);
    assert(!close_range(3,~0u,0));
    int fd=socket(AF_INET,SOCK_STREAM,0); assert(fd>=0);
    assert(offline_seal(-1)<0); /* inherited socket refuses sealing */
    assert(offline_seal(fd)<0); /* unconnected descriptor is not an allowed peer */
    close(fd);
    assert(!offline_seal(-1) && !offline_negative_test());
    puts("CONTROLLER_LOCAL_BOUNDARY_PASS malformed=7 inherited_socket_refused=1 unconnected_refused=1 network_denied=1");
}
