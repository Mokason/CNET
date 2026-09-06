#ifndef CONTROLLER_OFFLINE_H
#define CONTROLLER_OFFLINE_H
/* Linux x86-64 experiment boundary. An optional preconnected loopback:8092
 * descriptor may remain usable via read/write; all other sockets are refused.
 * The existing model server is outside this client-side boundary. */
int offline_seal(int model_fd);
int offline_negative_test(void);
#endif
