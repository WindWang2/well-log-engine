#pragma once

// Atomic file-write helper (table-and-export.md §10). Writes producer output to
// a temp file beside the target, then std::filesystem::rename onto the target
// (atomic on POSIX). On any failure the temp file is removed. The producer
// streams into an ostream so a million-row table never builds one giant string
// in memory (constant-memory path, §5.2 / §10). Returns the path written or an
// Error (architecture.md §2 / quality-security-performance.md §7: I/O failures
// are internal_error; resource_exhausted is reserved for bad_alloc only —
// matching the svg/pdf export siblings, NOT the manifest error code).

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

#include <welllog/core/result.hpp>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace welllog {
namespace export_table {

// A producer streams the file body into `out`. Returns true on success, false
// to abort (the helper then removes the temp file and returns an error).
using StreamProducer = std::function<bool(std::ostream &out)>;

// Writes `producer`'s output to `target` atomically. I/O failures (open,
// flush, rename, mkdir) map to internal_error; bad_alloc thrown by the producer
// maps to resource_exhausted. The temp file is cleaned on any failure.
[[nodiscard]] inline Result<std::filesystem::path>
write_file_atomic(const std::filesystem::path &target,
                  const StreamProducer &producer) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const auto parent = target.parent_path();
  const auto dir = parent.empty() ? fs::current_path(ec) : parent;
  const auto io_error = Error{
      .code = ErrorCode::internal_error,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = MessageKey::internal_error,
      .arguments = {},
  };
  if (ec) {
    return io_error;
  }
  // Temp file: target + ".<pid>.tmp" in the same directory (same filesystem →
  // rename is atomic).
  auto temp = target;
  temp += ".";
  temp += std::to_string(static_cast<std::uint64_t>(
#if defined(_WIN32)
      ::_getpid()
#else
      ::getpid()
#endif
      ));
  temp += ".tmp";
  {
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    if (!out) {
      fs::remove(temp, ec);
      return io_error;
    }
    try {
      if (!producer(out)) {
        out.close();
        fs::remove(temp, ec);
        return io_error;
      }
    } catch (const std::bad_alloc &) {
      out.close();
      fs::remove(temp, ec);
      return Error{
          .code = ErrorCode::resource_exhausted,
          .severity = Severity::error,
          .entity_id = std::nullopt,
          .message = MessageKey::resource_exhausted,
          .arguments = {},
      };
    } catch (...) {
      out.close();
      fs::remove(temp, ec);
      return io_error;
    }
    out.flush();
    if (!out) {
      fs::remove(temp, ec);
      return io_error;
    }
  }
  fs::rename(temp, target, ec);
  if (ec) {
    fs::remove(temp, ec);
    return io_error;
  }
  return target;
}

} // namespace export_table
} // namespace welllog
