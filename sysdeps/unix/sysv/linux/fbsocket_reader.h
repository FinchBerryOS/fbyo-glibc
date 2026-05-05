#ifndef _FBOS_FBSOCKET_READER_H
#define _FBOS_FBSOCKET_READER_H 1

#include "fbsocket_state.h"

int __fbsocket_start_threads (struct __fbsocket_state *st);
void __fbsocket_stop_threads (struct __fbsocket_state *st);

int __fbsocket_wait_ack (struct __fbsocket_state *st,
                         unsigned char *ack_status);

int __fbsocket_send_ack_internal (struct __fbsocket_state *st,
                                  unsigned char status);

int __fbsocket_send_data_internal (struct __fbsocket_state *st,
                                   const void *buf, size_t len,
                                   const int *fds, size_t fd_count);

#endif