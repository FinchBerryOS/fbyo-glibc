#include "fbsocket_proto.h"

uint64_t
__fbsocket_checksum64 (const void *buf, size_t len)
{
  const unsigned char *p = (const unsigned char *) buf;
  uint64_t h = 1469598103934665603ULL;
  size_t i;

  for (i = 0; i < len; ++i)
    {
      h ^= (uint64_t) p[i];
      h *= 1099511628211ULL;
    }

  return h;
}