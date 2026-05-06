#include "fbsocket_reader.h"
#include "fbsocket_proto.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void
__fbsocket_close_fd_array (int *fds, size_t fd_count)
{
  size_t i;

  if (fds == NULL)
    return;

  for (i = 0; i < fd_count; ++i)
    if (fds[i] >= 0)
      close (fds[i]);
}

static int
__fbsocket_send_frame (int fd,
                       const struct __fbsocket_frame_header *hdr,
                       const void *payload, size_t len,
                       const int *fds, size_t fd_count)
{
  struct iovec iov[2];
  struct msghdr msg;
  char cmsgbuf[CMSG_SPACE (FBSOCKET_MAX_FD_COUNT * sizeof (int))];
  struct cmsghdr *cmsg;
  size_t total_sent;
  ssize_t n;

  memset (&msg, 0, sizeof (msg));
  memset (cmsgbuf, 0, sizeof (cmsgbuf));

  iov[0].iov_base = (void *) hdr;
  iov[0].iov_len = sizeof (*hdr);
  iov[1].iov_base = (void *) payload;
  iov[1].iov_len = len;

  msg.msg_iov = iov;
  msg.msg_iovlen = 2;

  if (fd_count > 0)
    {
      msg.msg_control = cmsgbuf;
      msg.msg_controllen = CMSG_SPACE (fd_count * sizeof (int));

      cmsg = CMSG_FIRSTHDR (&msg);
      cmsg->cmsg_level = SOL_SOCKET;
      cmsg->cmsg_type = SCM_RIGHTS;
      cmsg->cmsg_len = CMSG_LEN (fd_count * sizeof (int));
      memcpy (CMSG_DATA (cmsg), fds, fd_count * sizeof (int));
    }

  total_sent = 0;

  while (total_sent < sizeof (*hdr) + len)
    {
      n = sendmsg (fd, &msg, MSG_NOSIGNAL);
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return -1;
        }

      if ((size_t) n >= iov[0].iov_len)
        {
          size_t remain = (size_t) n - iov[0].iov_len;
          iov[0].iov_base = (char *) iov[0].iov_base + iov[0].iov_len;
          iov[0].iov_len = 0;
          iov[1].iov_base = (char *) iov[1].iov_base + remain;
          iov[1].iov_len -= remain;
        }
      else
        {
          iov[0].iov_base = (char *) iov[0].iov_base + n;
          iov[0].iov_len -= n;
        }

      /* FDs nur beim ersten sendmsg mitsenden.  */
      msg.msg_control = NULL;
      msg.msg_controllen = 0;

      total_sent += (size_t) n;
    }

  return 0;
}

static int
__fbsocket_wait_readable (int fd, int timeout_ms)
{
  struct pollfd pfd;
  int rc;

  pfd.fd = fd;
  pfd.events = POLLIN;
  pfd.revents = 0;

  do
    rc = poll (&pfd, 1, timeout_ms);
  while (rc < 0 && errno == EINTR);

  if (rc == 0)
    {
      __set_errno (ETIMEDOUT);
      return -1;
    }

  if (rc < 0)
    return -1;

  if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
    {
      __set_errno (ECONNRESET);
      return -1;
    }

  return 0;
}

static int
__fbsocket_recv_exact (int fd, void *buf, size_t len)
{
  unsigned char *p = (unsigned char *) buf;
  size_t got = 0;
  ssize_t n;

  while (got < len)
    {
      n = recv (fd, p + got, len - got, MSG_WAITALL);
      if (n == 0)
        {
          __set_errno (ECONNRESET);
          return -1;
        }
      if (n < 0)
        {
          if (errno == EINTR)
            continue;
          return -1;
        }
      got += (size_t) n;
    }

  return 0;
}

static int
__fbsocket_recv_header_and_fds (int fd,
                                struct __fbsocket_frame_header *hdr,
                                int **fds_out, size_t *fd_count_out,
                                int timeout_ms)
{
  struct msghdr msg;
  struct iovec iov;
  char cmsgbuf[CMSG_SPACE (FBSOCKET_MAX_FD_COUNT * sizeof (int))];
  struct cmsghdr *cmsg;
  ssize_t n;
  int *fds = NULL;
  size_t fd_count = 0;

  if (timeout_ms >= 0 && __fbsocket_wait_readable (fd, timeout_ms) < 0)
    return -1;

  memset (&msg, 0, sizeof (msg));
  memset (&iov, 0, sizeof (iov));
  memset (hdr, 0, sizeof (*hdr));
  memset (cmsgbuf, 0, sizeof (cmsgbuf));

  iov.iov_base = hdr;
  iov.iov_len = sizeof (*hdr);

  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = cmsgbuf;
  msg.msg_controllen = sizeof (cmsgbuf);

  do
    n = recvmsg (fd, &msg, MSG_WAITALL | MSG_CMSG_CLOEXEC);
  while (n < 0 && errno == EINTR);

  if (n == 0)
    {
      __set_errno (ECONNRESET);
      return -1;
    }
  if (n < 0)
    return -1;

  if ((size_t) n != sizeof (*hdr))
    {
      __set_errno (EPROTO);
      return -1;
    }

  for (cmsg = CMSG_FIRSTHDR (&msg);
       cmsg != NULL;
       cmsg = CMSG_NXTHDR (&msg, cmsg))
    {
      if (cmsg->cmsg_level == SOL_SOCKET
          && cmsg->cmsg_type == SCM_RIGHTS)
        {
          fd_count = (cmsg->cmsg_len - CMSG_LEN (0)) / sizeof (int);
          if (fd_count > 0)
            {
              fds = malloc (fd_count * sizeof (int));
              if (fds == NULL)
                return -1;
              memcpy (fds, CMSG_DATA (cmsg), fd_count * sizeof (int));
            }
          break;
        }
    }

  *fds_out = fds;
  *fd_count_out = fd_count;
  return 0;
}

static int
__fbsocket_store_rx_message (struct __fbsocket_state *st,
                             unsigned char *data, size_t len,
                             int *fds, size_t fd_count)
{
  pthread_mutex_lock (&st->lock);

  if (st->rx_slot.occupied)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (EPROTO);
      return -1;
    }

  if (st->closed)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (ECONNRESET);
      return -1;
    }

  st->rx_slot.data = data;
  st->rx_slot.len = len;
  st->rx_slot.fds = fds;
  st->rx_slot.fd_count = fd_count;
  st->rx_slot.occupied = 1;

  pthread_cond_broadcast (&st->slot_cv);
  pthread_mutex_unlock (&st->lock);
  return 0;
}

static int
__fbsocket_handle_hello (struct __fbsocket_state *st,
                         const struct __fbsocket_frame_header *hdr)
{
  struct __fbsocket_hello_payload hello;

  if (hdr->fd_count != 0 || hdr->payload_len != sizeof (hello))
    {
      __set_errno (EPROTO);
      return -1;
    }

  if (__fbsocket_recv_exact (st->fd, &hello, sizeof (hello)) < 0)
    return -1;

  if (__fbsocket_checksum64 (&hello, sizeof (hello)) != hdr->checksum)
    {
      __set_errno (EBADMSG);
      return -1;
    }

  if (hello.max_payload == 0
      || hello.max_payload > __FBSOCKET_PROTO_MAX_MESSAGE_SIZE)
    {
      __set_errno (EMSGSIZE);
      return -1;
    }

  pthread_mutex_lock (&st->lock);
  st->peer_max_payload = hello.max_payload;
  st->effective_max_payload =
    (st->local_max_payload < st->peer_max_payload
     ? st->local_max_payload : st->peer_max_payload);
  st->hello_received = 1;
  pthread_cond_broadcast (&st->hello_cv);
  pthread_mutex_unlock (&st->lock);

  return 0;
}

static void *
__fbsocket_reader_main (void *arg)
{
  struct __fbsocket_state *st = (struct __fbsocket_state *) arg;
  struct __fbsocket_frame_header hdr;
  unsigned char *data = NULL;
  int *fds = NULL;
  size_t fd_count = 0;
  int timeout_ms = __FBSOCKET_HELLO_TIMEOUT_MS;

  for (;;)
    {
      free (data);
      data = NULL;
      free (fds);
      fds = NULL;
      fd_count = 0;

      if (__fbsocket_recv_header_and_fds (st->fd, &hdr, &fds, &fd_count,
                                          timeout_ms) < 0)
        break;

      if (hdr.magic != __FBSOCKET_PROTO_MAGIC
          || hdr.version != __FBSOCKET_PROTO_VERSION)
        {
          __set_errno (EPROTO);
          break;
        }

      if (!st->hello_received)
        {
          if (hdr.type != __FBSOCKET_FRAME_HELLO)
            {
              __set_errno (EPROTO);
              break;
            }

          if (fds != NULL || fd_count != 0)
            {
              __set_errno (EPROTO);
              break;
            }

          if (__fbsocket_handle_hello (st, &hdr) < 0)
            break;

          timeout_ms = -1;
          continue;
        }

      if (!(st->hello_sent && st->hello_received))
        {
          __set_errno (EPROTO);
          break;
        }

      if (hdr.type == __FBSOCKET_FRAME_ACK)
        {
          if (hdr.payload_len != 0 || hdr.fd_count != 0)
            {
              __set_errno (EPROTO);
              break;
            }

          pthread_mutex_lock (&st->lock);
          st->ack_received = 1;
          st->ack_status = (unsigned char) (hdr.reserved & 0xff);
          pthread_cond_broadcast (&st->ack_cv);
          pthread_mutex_unlock (&st->lock);

          free (fds);
          fds = NULL;
          continue;
        }

      if (hdr.type != __FBSOCKET_FRAME_DATA)
        {
          __set_errno (EPROTO);
          break;
        }

      if (hdr.payload_len > st->effective_max_payload
          || hdr.payload_len > __FBSOCKET_PROTO_MAX_MESSAGE_SIZE
          || hdr.fd_count > FBSOCKET_MAX_FD_COUNT
          || hdr.fd_count != fd_count)
        {
          __set_errno (EMSGSIZE);
          break;
        }

      data = malloc ((size_t) hdr.payload_len);
      if (data == NULL)
        break;

      if (__fbsocket_recv_exact (st->fd, data, (size_t) hdr.payload_len) < 0)
        break;

      if (__fbsocket_checksum64 (data, (size_t) hdr.payload_len) != hdr.checksum)
        {
          __set_errno (EBADMSG);
          break;
        }

      if (__fbsocket_store_rx_message (st, data, (size_t) hdr.payload_len,
                                       fds, fd_count) < 0)
        break;

      data = NULL;
      fds = NULL;
      fd_count = 0;
    }

  __fbsocket_close_fd_array (fds, fd_count);
  free (fds);
  free (data);

  pthread_mutex_lock (&st->lock);
  st->closed = 1;
  st->reader_running = 0;
  pthread_cond_broadcast (&st->ack_cv);
  pthread_cond_broadcast (&st->slot_cv);
  pthread_cond_broadcast (&st->hello_cv);
  pthread_mutex_unlock (&st->lock);

  return NULL;
}

static int
__fbsocket_bound_send_ack (unsigned char status)
{
  struct __fbsocket_callback_ctx *ctx = __fbsocket_current_cb;

  if (ctx == NULL || !ctx->active)
    {
      __set_errno (EPERM);
      return -1;
    }

  return __fbsocket_send_ack_internal (ctx->st, status);
}

static void *
__fbsocket_callback_main (void *arg)
{
  struct __fbsocket_state *st = (struct __fbsocket_state *) arg;
  struct __fbsocket_message_slot local;
  struct __fbsocket_callback_ctx ctx;
  int rc;

  memset (&local, 0, sizeof (local));
  memset (&ctx, 0, sizeof (ctx));

  for (;;)
    {
      pthread_mutex_lock (&st->lock);

      while (!st->rx_slot.occupied && !st->closed)
        pthread_cond_wait (&st->slot_cv, &st->lock);

      if (st->closed && !st->rx_slot.occupied)
        {
          st->callback_running = 0;
          pthread_mutex_unlock (&st->lock);
          return NULL;
        }

      local = st->rx_slot;
      memset (&st->rx_slot, 0, sizeof (st->rx_slot));

      st->callback_active = 1;
      st->ack_pending_send = 1;
      st->current_callback_ack_sent = 0;

      pthread_cond_broadcast (&st->slot_cv);
      pthread_mutex_unlock (&st->lock);

      ctx.st = st;
      ctx.active = 1;
      __fbsocket_current_cb = &ctx;

      rc = -1;
      if (st->reader_kind == __FBSOCKET_READER_SIMPLE && st->simple_cb != NULL)
        {
          rc = st->simple_cb (local.data, local.len,
                              local.fds, local.fd_count,
                              st->userdata);
          if (rc == 0)
            __fbsocket_send_ack_internal (st, FBSOCKET_ACK_OK);
        }
      else if (st->reader_kind == __FBSOCKET_READER_ACK && st->ack_cb != NULL)
        {
          st->ack_cb (local.data, local.len,
                      local.fds, local.fd_count,
                      __fbsocket_bound_send_ack,
                      st->userdata);
        }

      __fbsocket_current_cb = NULL;
      ctx.active = 0;

      pthread_mutex_lock (&st->lock);
      st->callback_active = 0;
      pthread_cond_broadcast (&st->slot_cv);
      pthread_mutex_unlock (&st->lock);

      free (local.fds);
      free (local.data);
      memset (&local, 0, sizeof (local));
    }
}

int
__fbsocket_send_hello_internal (struct __fbsocket_state *st)
{
  struct __fbsocket_frame_header hdr;
  struct __fbsocket_hello_payload hello;

  hello.max_payload = st->local_max_payload;

  memset (&hdr, 0, sizeof (hdr));
  hdr.magic = __FBSOCKET_PROTO_MAGIC;
  hdr.version = __FBSOCKET_PROTO_VERSION;
  hdr.type = __FBSOCKET_FRAME_HELLO;
  hdr.payload_len = sizeof (hello);
  hdr.checksum = __fbsocket_checksum64 (&hello, sizeof (hello));
  hdr.fd_count = 0;

  if (__fbsocket_send_frame (st->fd, &hdr, &hello, sizeof (hello), NULL, 0) < 0)
    return -1;

  pthread_mutex_lock (&st->lock);
  st->hello_sent = 1;
  pthread_cond_broadcast (&st->hello_cv);
  pthread_mutex_unlock (&st->lock);

  return 0;
}

int
__fbsocket_start_threads (struct __fbsocket_state *st)
{
  int ret;

  pthread_mutex_lock (&st->lock);

  if (st->reader_running || st->callback_running)
    {
      pthread_mutex_unlock (&st->lock);
      return 0;
    }

  st->reader_running = 1;
  st->callback_running = 1;
  pthread_mutex_unlock (&st->lock);

  ret = pthread_create (&st->reader_thread, NULL, __fbsocket_reader_main, st);
  if (ret != 0)
    {
      pthread_mutex_lock (&st->lock);
      st->reader_running = 0;
      st->callback_running = 0;
      pthread_mutex_unlock (&st->lock);
      __set_errno (ret);
      return -1;
    }

  ret = pthread_create (&st->callback_thread, NULL, __fbsocket_callback_main, st);
  if (ret != 0)
    {
      pthread_mutex_lock (&st->lock);
      st->closed = 1;
      st->reader_running = 0;
      st->callback_running = 0;
      pthread_cond_broadcast (&st->ack_cv);
      pthread_cond_broadcast (&st->slot_cv);
      pthread_cond_broadcast (&st->hello_cv);
      pthread_mutex_unlock (&st->lock);
      __set_errno (ret);
      return -1;
    }

  if (__fbsocket_send_hello_internal (st) < 0)
    {
      pthread_mutex_lock (&st->lock);
      st->closed = 1;
      pthread_cond_broadcast (&st->ack_cv);
      pthread_cond_broadcast (&st->slot_cv);
      pthread_cond_broadcast (&st->hello_cv);
      pthread_mutex_unlock (&st->lock);
      return -1;
    }

  return 0;
}

void
__fbsocket_stop_threads (struct __fbsocket_state *st)
{
  int join_reader;
  int join_callback;
  pthread_t reader;
  pthread_t callback;

  pthread_mutex_lock (&st->lock);
  st->closed = 1;
  pthread_cond_broadcast (&st->ack_cv);
  pthread_cond_broadcast (&st->slot_cv);
  pthread_cond_broadcast (&st->hello_cv);

  join_reader = st->reader_running;
  join_callback = st->callback_running;
  reader = st->reader_thread;
  callback = st->callback_thread;
  pthread_mutex_unlock (&st->lock);

  shutdown (st->fd, SHUT_RDWR);

  if (join_reader)
    pthread_join (reader, NULL);
  if (join_callback)
    pthread_join (callback, NULL);
}

int
__fbsocket_wait_hello (struct __fbsocket_state *st)
{
  pthread_mutex_lock (&st->lock);

  while (!(st->hello_sent && st->hello_received) && !st->closed)
    pthread_cond_wait (&st->hello_cv, &st->lock);

  if (st->closed)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (ECONNRESET);
      return -1;
    }

  pthread_mutex_unlock (&st->lock);
  return 0;
}

int
__fbsocket_wait_ack (struct __fbsocket_state *st,
                     unsigned char *ack_status)
{
  pthread_mutex_lock (&st->lock);

  while (!st->ack_received && !st->closed)
    pthread_cond_wait (&st->ack_cv, &st->lock);

  if (st->closed)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (ECONNRESET);
      return -1;
    }

  if (ack_status != NULL)
    *ack_status = st->ack_status;

  st->ack_received = 0;
  pthread_mutex_unlock (&st->lock);
  return 0;
}

int
__fbsocket_send_ack_internal (struct __fbsocket_state *st,
                              unsigned char status)
{
  struct __fbsocket_frame_header hdr;

  pthread_mutex_lock (&st->lock);

  if (!(st->hello_sent && st->hello_received))
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (EPERM);
      return -1;
    }

  if (!st->callback_active || !st->ack_pending_send)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (EPERM);
      return -1;
    }

  if (st->current_callback_ack_sent)
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (EALREADY);
      return -1;
    }

  st->current_callback_ack_sent = 1;
  st->ack_pending_send = 0;
  pthread_mutex_unlock (&st->lock);

  memset (&hdr, 0, sizeof (hdr));
  hdr.magic = __FBSOCKET_PROTO_MAGIC;
  hdr.version = __FBSOCKET_PROTO_VERSION;
  hdr.type = __FBSOCKET_FRAME_ACK;
  hdr.payload_len = 0;
  hdr.checksum = 0;
  hdr.fd_count = 0;
  hdr.reserved = status;

  return __fbsocket_send_frame (st->fd, &hdr, NULL, 0, NULL, 0);
}

int
__fbsocket_send_data_internal (struct __fbsocket_state *st,
                               const void *buf, size_t len,
                               const int *fds, size_t fd_count)
{
  struct __fbsocket_frame_header hdr;
  uint64_t max_payload;

  pthread_mutex_lock (&st->lock);

  if (!(st->hello_sent && st->hello_received))
    {
      pthread_mutex_unlock (&st->lock);
      __set_errno (EPERM);
      return -1;
    }

  max_payload = st->effective_max_payload;
  pthread_mutex_unlock (&st->lock);

  if (len > max_payload
      || len > __FBSOCKET_PROTO_MAX_MESSAGE_SIZE
      || fd_count > FBSOCKET_MAX_FD_COUNT)
    {
      __set_errno (EMSGSIZE);
      return -1;
    }

  memset (&hdr, 0, sizeof (hdr));
  hdr.magic = __FBSOCKET_PROTO_MAGIC;
  hdr.version = __FBSOCKET_PROTO_VERSION;
  hdr.type = __FBSOCKET_FRAME_DATA;
  hdr.payload_len = len;
  hdr.checksum = __fbsocket_checksum64 (buf, len);
  hdr.fd_count = (uint16_t) fd_count;

  return __fbsocket_send_frame (st->fd, &hdr, buf, len, fds, fd_count);
}