#include "fbsocket_table.h"
#include "fbsocket_state.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static struct __fbsocket_state **__fbsocket_fd_table;
static size_t __fbsocket_fd_table_cap;
static pthread_mutex_t __fbsocket_fd_table_lock = PTHREAD_MUTEX_INITIALIZER;

static int
__fbsocket_table_ensure_capacity (int fd)
{
  struct __fbsocket_state **newtab;
  size_t oldcap;
  size_t newcap;

  if (fd < 0)
    {
      __set_errno (EBADF);
      return -1;
    }

  if ((size_t) fd < __fbsocket_fd_table_cap)
    return 0;

  oldcap = __fbsocket_fd_table_cap;
  newcap = oldcap ? oldcap : 64;

  while ((size_t) fd >= newcap)
    newcap *= 2;

  newtab = realloc (__fbsocket_fd_table, newcap * sizeof (*newtab));
  if (newtab == NULL)
    {
      __set_errno (ENOMEM);
      return -1;
    }

  memset (newtab + oldcap, 0, (newcap - oldcap) * sizeof (*newtab));
  __fbsocket_fd_table = newtab;
  __fbsocket_fd_table_cap = newcap;
  return 0;
}

struct __fbsocket_state *
__fbsocket_get_state (int fd)
{
  struct __fbsocket_state *st = NULL;

  if (fd < 0)
    return NULL;

  pthread_mutex_lock (&__fbsocket_fd_table_lock);
  if ((size_t) fd < __fbsocket_fd_table_cap)
    st = __fbsocket_fd_table[fd];
  pthread_mutex_unlock (&__fbsocket_fd_table_lock);

  return st;
}

int
__fbsocket_set_state (int fd, struct __fbsocket_state *st)
{
  if (fd < 0)
    {
      __set_errno (EBADF);
      return -1;
    }

  pthread_mutex_lock (&__fbsocket_fd_table_lock);

  if (__fbsocket_table_ensure_capacity (fd) < 0)
    {
      pthread_mutex_unlock (&__fbsocket_fd_table_lock);
      return -1;
    }

  __fbsocket_fd_table[fd] = st;
  pthread_mutex_unlock (&__fbsocket_fd_table_lock);
  return 0;
}

struct __fbsocket_state *
__fbsocket_take_state (int fd)
{
  struct __fbsocket_state *st = NULL;

  if (fd < 0)
    return NULL;

  pthread_mutex_lock (&__fbsocket_fd_table_lock);
  if ((size_t) fd < __fbsocket_fd_table_cap)
    {
      st = __fbsocket_fd_table[fd];
      __fbsocket_fd_table[fd] = NULL;
    }
  pthread_mutex_unlock (&__fbsocket_fd_table_lock);

  return st;
}

int
__fbsocket_register_fd (int fd)
{
  struct __fbsocket_state *st;

  st = __fbsocket_state_create (fd);
  if (st == NULL)
    {
      __set_errno (ENOMEM);
      return -1;
    }

  if (__fbsocket_set_state (fd, st) < 0)
    {
      __fbsocket_state_unref (st);
      return -1;
    }

  return 0;
}

int
__fbsocket_is_managed (int fd)
{
  return __fbsocket_get_state (fd) != NULL;
}

void
__fbsocket_forkproc (void)
{
  size_t i;
  struct __fbsocket_state *st;

  pthread_mutex_lock (&__fbsocket_fd_table_lock);

  for (i = 0; i < __fbsocket_fd_table_cap; ++i)
    {
      st = __fbsocket_fd_table[i];
      if (st != NULL)
        {
          __fbsocket_fd_table[i] = NULL;
          close ((int) i);
          __fbsocket_state_unref (st);
        }
    }

  pthread_mutex_unlock (&__fbsocket_fd_table_lock);
}