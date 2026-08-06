#pragma once

// Vendored Arrow C Data Interface (format/CDataInterface.html). Kept local so
// WellLog::Arrow can adapt arrays without putting Apache Arrow C++ types or
// headers into welllog-core, and without requiring libarrow for the C-ABI path
// (ADR 0027). Layout matches the stable ABI; producers may be Arrow C++,
// PyArrow, or any other C Data exporter.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef WELLLOG_ARROW_C_DATA_INTERFACE
#define WELLLOG_ARROW_C_DATA_INTERFACE

#define WELLLOG_ARROW_FLAG_DICTIONARY_ORDERED 1
#define WELLLOG_ARROW_FLAG_NULLABLE 2
#define WELLLOG_ARROW_FLAG_MAP_KEYS_SORTED 4

struct WellLogArrowSchema {
  const char *format;
  const char *name;
  const char *metadata;
  int64_t flags;
  int64_t n_children;
  struct WellLogArrowSchema **children;
  struct WellLogArrowSchema *dictionary;
  void (*release)(struct WellLogArrowSchema *);
  void *private_data;
};

struct WellLogArrowArray {
  int64_t length;
  int64_t null_count;
  int64_t offset;
  int64_t n_buffers;
  int64_t n_children;
  const void **buffers;
  struct WellLogArrowArray **children;
  struct WellLogArrowArray *dictionary;
  void (*release)(struct WellLogArrowArray *);
  void *private_data;
};

#endif // WELLLOG_ARROW_C_DATA_INTERFACE

#ifdef __cplusplus
} // extern "C"
#endif
