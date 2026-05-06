#ifndef _FBOS_FBSOCKET_H
#define _FBOS_FBSOCKET_H 1

#include <stddef.h>
#include <sys/socket.h>
#include <sys/types.h>

#define FBSOCKET_ACK_OK           0
#define FBSOCKET_ACK_RESERVED_MIN 1
#define FBSOCKET_ACK_RESERVED_MAX 127
#define FBSOCKET_ACK_USER_MIN     128
#define FBSOCKET_ACK_USER_MAX     255

#define FBSOCKET_MAX_MESSAGE_SIZE (64ULL * 1024ULL * 1024ULL)
#define FBSOCKET_MAX_FD_COUNT     253

struct fbsocket_state_info
{
  int connected;
  int is_listener;
  int reader_running;
  int callback_running;
  int awaiting_ack;
  size_t current_rx_size;
  size_t current_rx_fd_count;
};

typedef int (*fbsocket_send_ack_fn) (unsigned char status);

typedef int (*fbsocket_reader_simple_fn) (const void *data, size_t len,
                                          const int *fds, size_t fd_count,
                                          void *userdata);

typedef int (*fbsocket_reader_ack_fn) (const void *data, size_t len,
                                       const int *fds, size_t fd_count,
                                       fbsocket_send_ack_fn send_ack,
                                       void *userdata);

int fbsocket_open (int domain, int flags);
int fbsocket_upgrade (int fd);
int fbsocket_connect (int fd, __CONST_SOCKADDR_ARG addr, socklen_t len);
int fbsocket_close (int fd);

ssize_t fbsocket_write (int fd, const void *buf, size_t len,
                        const int *fds, size_t fd_count,
                        unsigned char *ack_status);

int fbsocket_set_reader (int fd, fbsocket_reader_simple_fn cb,
                         void *userdata);

int fbsocket_set_reader_ack (int fd, fbsocket_reader_ack_fn cb,
                             void *userdata);

int fbsocket_send_ack (int fd, unsigned char status);
int fbsocket_get_state (int fd, struct fbsocket_state_info *out);

#endif