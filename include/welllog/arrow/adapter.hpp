#pragma once

// Optional Arrow / mmap adapter (ADR 0027, #163).
//
// Converts Arrow C Data Interface arrays and dense mmap columns into Core
// BufferView / NullBitmapView without exposing Arrow types through welllog-core.
// Compatible fixed-width primitive arrays are Zero Copy; incompatible types
// require an explicit allow_converted_copy policy and report Converted Copy.

#include <cstdint>
#include <filesystem>
#include <string>

#include <welllog/arrow/c_abi.hpp>
#include <welllog/arrow/export.hpp>
#include <welllog/core/document.hpp>
#include <welllog/core/result.hpp>

namespace welllog {

// Policy for types that cannot be viewed as a native ScalarType without a copy
// (e.g. half-float → float64). Default is refuse: callers must opt in.
struct ArrowImportOptions {
  bool allow_converted_copy{false};
};

// Result of one array import. `values` / `nulls` share a SharedOwner that
// keeps the Arrow release callback (or mmap mapping) alive for the full
// engine read cycle (prepare, LOD, table, export).
struct ArrowArrayImport {
  BufferView values;
  NullBitmapView nulls;
  // Redundant with values.access_mode() / derived nulls mode — kept for host
  // diagnostics without re-probing the views.
  BufferAccessMode values_access{BufferAccessMode::zero_copy};
  BufferAccessMode nulls_access{BufferAccessMode::zero_copy};
  ScalarType scalar_type{ScalarType::float64};
  std::uint64_t length{};
};

// Imports a primitive (non-nested) Arrow C Data array.
//
// `WellLogArrowSchema` / `WellLogArrowArray` are layout-compatible with
// Apache Arrow's ArrowSchema / ArrowArray (same C Data ABI).
//
// On success, ownership of `array` is transferred into the returned views'
// SharedOwner (array.release is cleared on the caller's struct). On failure,
// `array` is left unchanged for the caller to release.
//
// `schema` is borrowed: format is read; the caller retains schema ownership.
//
// Handles length, offset, type, dense stride, and validity bitmap. Arrow
// validity bits (1 = valid) are converted to Core null bits (1 = null).
[[nodiscard]] WELLLOG_ARROW_API Result<ArrowArrayImport>
import_arrow_array(const WellLogArrowSchema &schema, WellLogArrowArray &array,
                   ArrowImportOptions options = {},
                   BufferSourceReference source = {}) noexcept;

// Maps a dense, homogeneous scalar column file into a zero-copy BufferView.
// The mapping covers the full file; `length * scalar_size` must not exceed
// the mapped size. SharedOwner unmaps when the last BufferView dies.
[[nodiscard]] WELLLOG_ARROW_API Result<BufferView>
import_mmap_scalar_column(const std::filesystem::path &path, ScalarType type,
                          std::uint64_t length,
                          BufferSourceReference source = {}) noexcept;

// True when this build linked Apache Arrow C++ and IPC helpers are available.
[[nodiscard]] WELLLOG_ARROW_API bool arrow_ipc_available() noexcept;

// Reads one column of an Arrow IPC file (File or Stream) as a BufferView.
// Requires WELLLOG_ARROW_HAS_IPC. Column must be a fixed-width primitive
// (same rules as import_arrow_array). Zero-copy when the column is compatible.
[[nodiscard]] WELLLOG_ARROW_API Result<ArrowArrayImport>
import_arrow_ipc_file_column(const std::filesystem::path &path,
                             int column_index,
                             ArrowImportOptions options = {},
                             BufferSourceReference source = {}) noexcept;

// Human-readable access mode for diagnostics / host UI.
[[nodiscard]] WELLLOG_ARROW_API std::string_view
buffer_access_mode_name(BufferAccessMode mode) noexcept;

} // namespace welllog
