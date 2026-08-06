#pragma once

// Minimal in-memory ZIP writer for self-contained OOXML (.xlsx) output (#155
// XLSX backend). Produces a valid ZIP archive: per-entry local file header +
// deflated (or stored) data, then a central directory and end-of-central-
// directory record. Uses ZLIB for deflate compression and CRC-32. Only the
// subset OOXML readers (Excel/LibreOffice/the test's inflate readback) need.
//
// KNOWN LIMIT (table-and-export.md §5.2/§10 "流式/constant-memory"): the whole
// archive is materialized in memory before the atomic write. The XLSX path
// (xlsx.cpp) builds every worksheet body into memory, then serializes the full
// zip into one std::string. This is O(total workbook size) in RAM — acceptable
// for Phase-A table sizes, but NOT constant-memory for very large workbooks.
// A streaming zip-to-file (deflate each worksheet incrementally, write local
// headers + central dir as it goes) is the tracked follow-up. The
// add_entry_streamed hook is retained for that future path but the current XLSX
// writer does not use it.

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <zlib.h>

namespace welllog {
namespace export_table {

struct ZipEntry {
  std::string name;          // e.g. "xl/worksheets/sheet1.xml"
  std::vector<unsigned char> data; // compressed (deflated) bytes
  std::uint32_t crc32{0};
  std::uint64_t uncompressed_size{0};
  std::uint16_t method{8}; // 8 = deflate, 0 = store
};

class ZipWriter {
public:
  ZipWriter() = default;

  // Adds an entry whose content is the raw bytes; compresses with deflate
  // (method 8) unless `store` is true (method 0). Returns false on a zlib
  // error.
  bool add_entry(const std::string &name, const std::string &content,
                 bool store = false);

  // Adds an entry from a producer that streams into an accumulating string
  // (used so a large worksheet can be built incrementally without holding the
  // whole workbook in one buffer before compression).
  bool add_entry_streamed(const std::string &name,
                          const std::function<bool(std::string &)> &producer);

  // Serializes the archive (local headers + data + central directory + EOCD)
  // into `out`.
  void serialize(std::string &out) const;

  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] const std::vector<ZipEntry> &entries() const noexcept {
    return entries_;
  }

private:
  std::vector<ZipEntry> entries_;
};

} // namespace export_table
} // namespace welllog
