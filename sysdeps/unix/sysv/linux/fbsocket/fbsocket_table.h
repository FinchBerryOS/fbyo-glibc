#ifndef _FBOS_FBSOCKET_TABLE_H
#define _FBOS_FBSOCKET_TABLE_H 1

struct __fbsocket_state;

struct __fbsocket_state *__fbsocket_get_state (int fd);
int __fbsocket_set_state (int fd, struct __fbsocket_state *st);
struct __fbsocket_state *__fbsocket_take_state (int fd);
int __fbsocket_register_fd (int fd);
int __fbsocket_is_managed (int fd);
void __fbsocket_forkproc (void);

#endif