// See LICENSE for license details.

#ifndef _LIBSPIKE_H
#define _LIBSPIKE_H

#include <stdint.h>

#define LIBSPIKE_BASE_ADDR 0x200000000000ll
#define LIBSPIKE_FN_OFFSET 0xe00
#define LIBSPIKE_TAG_OFFSET 0xf00

#define ADD_FN(fn_name) inline void fn_name() {*((uint8_t*) (LIBSPIKE_BASE_ADDR + LIBSPIKE_FN_OFFSET + 8*(__LINE__ - base))) = 1;}

// To add new functions, first create function in mmu.h and add it to init_libspike() in mmu.cc
// Then add the function here, making sure that the order is the same
// Don't add extra newlines!
static int base = __LINE__ + 1;
ADD_FN(libspike_reset)
ADD_FN(libspike_cachestats)
ADD_FN(libspike_monitor)
ADD_FN(libspike_track)
ADD_FN(libspike_exit_with_retcode)

typedef struct {
  char name[32];
  uint64_t read_accesses;
  uint64_t read_misses;
  uint64_t bytes_read;
  uint64_t write_accesses;
  uint64_t write_misses;
  uint64_t bytes_written;
  uint64_t writebacks;
} cache_stats_t;

typedef struct {
  cache_stats_t stats[16];
  uint32_t n_caches;
} cache_info_t;

typedef struct {
  uint64_t arg0;
  uint64_t arg1;
  uint64_t arg2;
  uint64_t arg3;
  uint64_t arg4;
} libspike_args_t;

typedef union {
  cache_info_t cache_info;
  libspike_args_t args;
  uint8_t buf[4096];
} libspike_page_u;

inline cache_info_t* libspike_get_cache_info() {
  return &(((libspike_page_u*) (LIBSPIKE_BASE_ADDR))->cache_info);
}

inline libspike_args_t* libspike_get_args() {
  return &(((libspike_page_u*) (LIBSPIKE_BASE_ADDR))->args);
}

#undef ADD_FN
#endif
