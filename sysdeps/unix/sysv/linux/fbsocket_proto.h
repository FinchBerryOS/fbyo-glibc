#ifndef _FBOS_FBSOCKET_PROTO_H
#define _FBOS_FBSOCKET_PROTO_H 1

#include <stdint.h>
#include <stddef.h>

#define __FBSOCKET_PROTO_MAGIC   0x4642534fU
#define __FBSOCKET_PROTO_VERSION 1

enum __fbsocket_frame_type
{
  __FBSOCKET_FRAME_DATA = 1,
  __FBSOCKET_FRAME_ACK = 2
};

struct __attribute__ ((__packed__)) __fbsocket_frame_header
{
  uint32_t magic;
  uint16_t version;
  uint16_t type;
  uint64_t payload_len;
  uint64_t checksum;
  uint16_t fd_count;
  uint16_t reserved;
};

uint64_t __fbsocket_checksum64 (const void *buf, size_t len);

#endif