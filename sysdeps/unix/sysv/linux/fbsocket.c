#include "fbsocket.h"
#include "fbsocket_state.h"
#include "fbsocket_table.h"
#include "fbsocket_reader.h"

#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

static int
__fbsocket_validate_managed (int fd, struct __fbsocket_state **out)
{
  struct __fbsocket_state *st;

  st = __fbsocket_get_state (fd);
  if (st == NULL)
    {
      __set_errno (ENOTSOCK);
      return -1;
    }

  if (__fbsocket_state_check_pid (st) < 0)
    return -1;

  if (out != NULL)
    *out = st;

  return 0;
}

int
fbsocket_open (int domain, int flags)
{
  int fd;
  int allowed_flags;

  if (domain != AF_UNIX)
    {
      __set_errno (EAFNOSUPPORT);
      return -1;
    }

  allowed_flags = SOCK_CLOEXEC | SOCK_NONBLOCK;
  if (flags & ~allowed_flags)
    {
      __set_errno (EINVAL);
      return -1;
    }

  fd = __socket (domain, SOCK_STREAM | flags, 0);
  if (fd < 0)
    return -1;

  if (__fbsocket_register_fd (fd) < 0)
    {
      __close (fd);
      return -1;
    }

  return fd;
}

int
fbsocket_upgrade (int fd)
{
  int type;
  struct sockaddr_storage ss;
  socklen_t slen;

  if (fd < 0)
    {
      __set_errno (EBADF);
      return -1;
    }

  if (__fbsocket_is_managed (fd))
    return 0;

  slen = sizeof (type);
  if (getsockopt (fd, SOL_SOCKET, SO_TYPE, &type, &slen) < 0)
    return -1;

  if (type != SOCK_STREAM)
    {
      __set_errno (ENOTSOCK);
      return -1;
    }

  slen = sizeof (ss);
  if (getsockname (fd, (struct sockaddr *) &ss, &slen) < 0)
    return -1;

  if (ss.ss_family != AF_UNIX)
    {
      __set_errno (EAFNOSUPPORT);
      return -1;
    }

  return __fbsocket_register_fd (fd);
}

int
fbsocket_connect (int fd, __CONST_SOCKADDR_ARG addr, socklen_t len)
{
  struct __fbsocket_state *st;

  if (__fbsocket_validate_managed (fd, &st) < 0)
    return -1;

  if (__connect (fd, addr, len) < 0)
    return -1;

  pthread_mutex_lock (&st->lock);
  st->connected = 1;
  st->is_listener = 0;
  pthread_mutex_unlock (&st->lock);

  if (__fbsocket_start_threads (st) < 0)
    return -1;

  return 0;
}

int
fbsocket_close (int fd)
{
  struct __fbsocket_state *st;

  st = __fbsocket_take_state (fd);
  if (st != NULL)
    {
      __fbsocket_stop_threads (st);
      __fbsocket_state_unref (st);
    }

  return __close (fd);
}

ssize_t
fbsocket_write (int fd, const void *buf, size_t len,
                const int *fds, size_t fd_count,
                unsigned char *ack_status)
{
  struct __fbsocket_state *st;

  if (__fbsocket_validate_managed (fd, &st) < 0)
    return -1;

  pthread_mutex_lock (&st->lock);
  if (!st->connected || st->closed)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (ENOTCONN);
      return -1;
    }

  if (st->waiting_for_ack)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (EALREADY);
      return -1;
    }

  st->waiting_for_ack = 1;
  st->ack_received = 0;
  st->ack_status = FBSOCKET_ACK_OK;
  pthread_mutex_unlock (&st->lock);

  if (__fbsocket_send_data_internal (st, buf, len, fds, fd_count) < 0)
    {
      pthread_mutex_lock (&st->lock);
      st->waiting_for_ack = 0;
      pthread_mutex_unlock (&st->lock);
      return -1;
    }

  if (__fbsocket_wait_ack (st, ack_status) < 0)
    {
      pthread_mutex_lock (&st->lock);
      st->waiting_for_ack = 0;
      pthread_mutex_unlock (&st->lock);
      return -1;
    }

  pthread_mutex_lock (&st->lock);
  st->waiting_for_ack = 0;
  pthread_mutex_unlock (&st->lock);

  return (ssize_t) len;
}

int
fbsocket_set_reader (int fd, fbsocket_reader_simple_fn cb,
                     void *userdata)
{
  struct __fbsocket_state *st;

  if (__fbsocket_validate_managed (fd, &st) < 0)
    return -1;

  pthread_mutex_lock (&st->lock);
  st->reader_kind = __FBSOCKET_READER_SIMPLE;
  st->simple_cb = cb;
  st->ack_cb = NULL;
  st->userdata = userdata;
  pthread_mutex_unlock (&st->lock);

  return 0;
}

int
fbsocket_set_reader_ack (int fd, fbsocket_reader_ack_fn cb,
                         void *userdata)
{
  struct __fbsocket_state *st;

  if (__fbsocket_validate_managed (fd, &st) < 0)
    return -1;

  pthread_mutex_lock (&st->lock);
  st->reader_kind = __FBSOCKET_READER_ACK;
  st->simple_cb = NULL;
  st->ack_cb = cb;
  st->userdata = userdata;
  pthread_mutex_unlock (&st->lock);

  return 0;
}

int
fbsocket_send_ack (int fd, unsigned char status)
{
  struct __fbsocket_callback_ctx *ctx = __fbsocket_current_cb;

  if (ctx == NULL || !ctx->active || ctx->st == NULL || ctx->st->fd != fd)
    {
      __set_errno (EPERM);
      return -1;
    }

  return __fbsocket_send_ack_internal (ctx->st, status);
}

int
fbsocket_get_state (int fd, struct fbsocket_state_info *out)
{
  struct __fbsocket_state *st;

  if (out == NULL)
    {
      __set_errno (EINVAL);
      return -1;
    }

  if (__fbsocket_validate_managed (fd, &st) < 0)
    return -1;

  pthread_mutex_lock (&st->lock);
  out->connected = st->connected;
  out->is_listener = st->is_listener;
  out->reader_running = st->reader_running;
  out->callback_running = st->callback_running;
  out->awaiting_ack = st->waiting_for_ack;
  out->current_rx_size = st->rx_slot.occupied ? st->rx_slot.len : 0;
  out->current_rx_fd_count = st->rx_slot.occupied ? st->rx_slot.fd_count : 0;
  pthread_mutex_unlock (&st->lock);

  return 0;
}