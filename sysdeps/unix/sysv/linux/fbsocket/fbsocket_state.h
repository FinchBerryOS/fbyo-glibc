#ifndef _FBOS_FBSOCKET_STATE_H
#define _FBOS_FBSOCKET_STATE_H 1

#include <pthread.h>
#include <stddef.h>
#include <sys/types.h>

#include "fbsocket.h"

enum __fbsocket_reader_kind
{
  __FBSOCKET_READER_NONE = 0,
  __FBSOCKET_READER_SIMPLE = 1,
  __FBSOCKET_READER_ACK = 2
};

struct __fbsocket_message_slot
{
  unsigned char *data;
  size_t len;

  int *fds;
  size_t fd_count;

  int occupied;
};

struct __fbsocket_state
{
  int fd;
  pid_t owner_pid;
  int refcnt;

  int closed;
  int connected;
  int is_listener;

  int waiting_for_ack;
  int ack_received;
  unsigned char ack_status;

  int callback_active;
  int ack_pending_send;
  int current_callback_ack_sent;

  int reader_running;
  int callback_running;

  pthread_t reader_thread;
  pthread_t callback_thread;

  pthread_mutex_t lock;
  pthread_cond_t ack_cv;
  pthread_cond_t slot_cv;

  struct __fbsocket_message_slot rx_slot;

  int reader_kind;
  fbsocket_reader_simple_fn simple_cb;
  fbsocket_reader_ack_fn ack_cb;
  void *userdata;
};

struct __fbsocket_callback_ctx
{
  struct __fbsocket_state *st;
  int active;
};

extern __thread struct __fbsocket_callback_ctx *__fbsocket_current_cb;

struct __fbsocket_state *__fbsocket_state_create (int fd);
void __fbsocket_state_ref (struct __fbsocket_state *st);
void __fbsocket_state_unref (struct __fbsocket_state *st);
int __fbsocket_state_check_pid (struct __fbsocket_state *st);

#endif