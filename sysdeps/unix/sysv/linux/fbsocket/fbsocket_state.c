#include "fbsocket_state.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

__thread struct __fbsocket_callback_ctx *__fbsocket_current_cb;

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

static void
__fbsocket_slot_reset_owned (struct __fbsocket_message_slot *slot)
{
  if (slot == NULL)
    return;

  __fbsocket_close_fd_array (slot->fds, slot->fd_count);
  free (slot->data);
  free (slot->fds);

  slot->data = NULL;
  slot->len = 0;
  slot->fds = NULL;
  slot->fd_count = 0;
  slot->occupied = 0;
}

static void
__fbsocket_state_destroy (struct __fbsocket_state *st)
{
  if (st == NULL)
    return;

  __fbsocket_slot_reset_owned (&st->rx_slot);

  pthread_mutex_destroy (&st->lock);
  pthread_cond_destroy (&st->ack_cv);
  pthread_cond_destroy (&st->slot_cv);
  pthread_cond_destroy (&st->hello_cv);

  free (st);
}

struct __fbsocket_state *
__fbsocket_state_create (int fd)
{
  struct __fbsocket_state *st;

  st = calloc (1, sizeof (*st));
  if (st == NULL)
    return NULL;

  st->fd = fd;
  st->owner_pid = getpid ();
  st->refcnt = 1;
  st->ack_status = FBSOCKET_ACK_OK;
  st->reader_kind = __FBSOCKET_READER_NONE;
  st->local_max_payload = __FBSOCKET_PROTO_MAX_MESSAGE_SIZE;
  st->peer_max_payload = 0;
  st->effective_max_payload = __FBSOCKET_PROTO_MAX_MESSAGE_SIZE;

  if (pthread_mutex_init (&st->lock, NULL) != 0)
    {
      free (st);
      return NULL;
    }

  if (pthread_cond_init (&st->ack_cv, NULL) != 0)
    {
      pthread_mutex_destroy (&st->lock);
      free (st);
      return NULL;
    }

  if (pthread_cond_init (&st->slot_cv, NULL) != 0)
    {
      pthread_cond_destroy (&st->ack_cv);
      pthread_mutex_destroy (&st->lock);
      free (st);
      return NULL;
    }

  if (pthread_cond_init (&st->hello_cv, NULL) != 0)
    {
      pthread_cond_destroy (&st->slot_cv);
      pthread_cond_destroy (&st->ack_cv);
      pthread_mutex_destroy (&st->lock);
      free (st);
      return NULL;
    }

  return st;
}

void
__fbsocket_state_ref (struct __fbsocket_state *st)
{
  if (st == NULL)
    return;

  pthread_mutex_lock (&st->lock);
  st->refcnt++;
  pthread_mutex_unlock (&st->lock);
}

void
__fbsocket_state_unref (struct __fbsocket_state *st)
{
  int destroy = 0;

  if (st == NULL)
    return;

  pthread_mutex_lock (&st->lock);
  st->refcnt--;
  if (st->refcnt <= 0)
    destroy = 1;
  pthread_mutex_unlock (&st->lock);

  if (destroy)
    __fbsocket_state_destroy (st);
}

int
__fbsocket_state_check_pid (struct __fbsocket_state *st)
{
  if (st == NULL)
    {
      __set_errno (EBADF);
      return -1;
    }

  if (st->owner_pid != getpid ())
    {
      __set_errno (EBADF);
      return -1;
    }

  return 0;
}