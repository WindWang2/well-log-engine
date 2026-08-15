// Minimal ZIP writer implementation (see zip_writer.hpp). Produces a valid
// archive for OOXML (.xlsx). Uses zlib deflate + crc32. The format written is
// the classic "local file header + data + central directory + EOCD" structure,
// version-made 20 (2.0), no Zip64 (entries are well under the 4GiB limit).

#include "zip_writer.hpp"

#include <cctype>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace welllog {
namespace export_table {

namespace {

// Local path policy (mirrors welllog::is_safe_archive_entry_name) so ExportTable
// does not depend on WellLog::IO.
[[nodiscard]] bool safe_entry_name(std::string_view name) noexcept {
  if (name.empty() || name.size() > 512) {
    return false;
  }
  if (name.front() == '/' || name.front() == '\\') {
    return false;
  }
  if (name.size() >= 2 && std::isalpha(static_cast<unsigned char>(name[0])) &&
      name[1] == ':') {
    return false;
  }
  for (const auto c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\') {
      return false;
    }
  }
  std::size_t pos = 0;
  while ((pos = name.find("..", pos)) != std::string_view::npos) {
    const bool left_ok = pos == 0 || name[pos - 1] == '/';
    const bool right_ok = pos + 2 >= name.size() || name[pos + 2] == '/';
    if (left_ok && right_ok) {
      return false;
    }
    pos += 2;
  }
  return true;
}

// Little-endian byte writers.
void put_u16(std::string &out, std::uint16_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
}
void put_u32(std::string &out, std::uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFF));
  out.push_back(static_cast<char>((v >> 8) & 0xFF));
  out.push_back(static_cast<char>((v >> 16) & 0xFF));
  out.push_back(static_cast<char>((v >> 24) & 0xFF));
}

// Deflates `input`; returns the compressed bytes. Falls back to stored (method
// 0) on zlib error or if compression doesn't help.
[[nodiscard]] std::pair<std::vector<unsigned char>, std::uint16_t>
deflate_or_store(const std::string &input) {
  z_stream stream{};
  // windowBits = -15 → raw deflate (no zlib header), as ZIP expects.
  if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8,
                   Z_DEFAULT_STRATEGY) != Z_OK) {
    return {{input.begin(), input.end()}, 0};
  }
  std::vector<unsigned char> out;
  out.resize(input.size() + 64);
  stream.next_in =
      reinterpret_cast<Bytef *>(const_cast<char *>(input.data()));
  stream.avail_in = static_cast<uInt>(input.size());
  stream.next_out = out.data();
  stream.avail_out = static_cast<uInt>(out.size());
  if (deflate(&stream, Z_FINISH) != Z_STREAM_END) {
    deflateEnd(&stream);
    return {{input.begin(), input.end()}, 0};
  }
  out.resize(stream.total_out);
  deflateEnd(&stream);
  return {std::move(out), 8};
}

[[nodiscard]] std::uint32_t compute_crc32(const std::string &input) {
  return static_cast<std::uint32_t>(::crc32(
      0L, reinterpret_cast<const Bytef *>(input.data()),
      static_cast<uInt>(input.size())));
}

} // namespace

bool ZipWriter::add_entry(const std::string &name, const std::string &content,
                          bool store) {
  // Reject path traversal / absolute names so even host-built packages stay
  // within the untrusted-container policy (#171).
  if (!safe_entry_name(name)) {
    return false;
  }
  constexpr std::size_t max_entries = 4096;
  constexpr std::size_t max_entry_bytes = 256ULL * 1024ULL * 1024ULL;
  if (entries_.size() >= max_entries || content.size() > max_entry_bytes) {
    return false;
  }
  ZipEntry e;
  e.name = name;
  e.crc32 = compute_crc32(content);
  e.uncompressed_size = content.size();
  if (store) {
    e.data.assign(content.begin(), content.end());
    e.method = 0;
  } else {
    auto [deflated, method] = deflate_or_store(content);
    e.data = std::move(deflated);
    e.method = method;
  }
  entries_.push_back(std::move(e));
  return true;
}

bool ZipWriter::add_entry_streamed(
    const std::string &name, const std::function<bool(std::string &)> &producer) {
  std::string body;
  body.reserve(1 << 16);
  if (!producer(body)) {
    return false;
  }
  return add_entry(name, body, false);
}

void ZipWriter::serialize(std::string &out) const {
  out.clear();
  out.reserve(1 << 16);
  // Track local-header offsets for the central directory.
  struct DirEntry {
    std::string name;
    std::uint32_t crc32;
    std::uint64_t uncompressed_size;
    std::uint64_t compressed_size;
    std::uint16_t method;
    std::uint64_t local_header_offset;
  };
  std::vector<DirEntry> central;
  central.reserve(entries_.size());
  for (const auto &e : entries_) {
    // Value-initialize so the struct's padding bytes are zeroed; GCC's
    // maybe-uninitialized analysis otherwise flags the whole-object copy in
    // central.push_back(d) (uninitialized padding, benign but real).
    DirEntry d{};
    d.name = e.name;
    d.crc32 = e.crc32;
    d.uncompressed_size = e.uncompressed_size;
    d.compressed_size = e.data.size();
    d.method = e.method;
    d.local_header_offset = out.size();
    central.push_back(d);
    // Local file header (signature 0x04034b50).
    put_u32(out, 0x04034b50);
    put_u16(out, 20);            // version needed to extract (2.0)
    put_u16(out, 0);             // flags
    put_u16(out, e.method);      // compression method
    put_u16(out, 0);             // mod time
    put_u16(out, 0);             // mod date
    put_u32(out, e.crc32);
    put_u32(out, static_cast<std::uint32_t>(e.data.size()));   // compressed
    put_u32(out, static_cast<std::uint32_t>(e.uncompressed_size)); // uncompressed
    put_u16(out, static_cast<std::uint16_t>(e.name.size()));
    put_u16(out, 0);             // extra field length
    out.append(e.name);
    out.append(reinterpret_cast<const char *>(e.data.data()), e.data.size());
  }
  const auto central_start = out.size();
  for (const auto &d : central) {
    // Central directory header (signature 0x02014b50).
    put_u32(out, 0x02014b50);
    put_u16(out, 20);            // version made by
    put_u16(out, 20);            // version needed
    put_u16(out, 0);             // flags
    put_u16(out, d.method);
    put_u16(out, 0);             // mod time
    put_u16(out, 0);             // mod date
    put_u32(out, d.crc32);
    put_u32(out, static_cast<std::uint32_t>(d.compressed_size));
    put_u32(out, static_cast<std::uint32_t>(d.uncompressed_size));
    put_u16(out, static_cast<std::uint16_t>(d.name.size()));
    put_u16(out, 0);             // extra
    put_u16(out, 0);             // comment
    put_u16(out, 0);             // disk number start
    put_u16(out, 0);             // internal attrs
    put_u32(out, 0);             // external attrs
    put_u32(out, static_cast<std::uint32_t>(d.local_header_offset));
    out.append(d.name);
  }
  const auto central_size = out.size() - central_start;
  // End of central directory (signature 0x06054b50).
  put_u32(out, 0x06054b50);
  put_u16(out, 0);             // disk number
  put_u16(out, 0);             // disk with central dir
  put_u16(out, static_cast<std::uint16_t>(central.size()));
  put_u16(out, static_cast<std::uint16_t>(central.size()));
  put_u32(out, static_cast<std::uint32_t>(central_size));
  put_u32(out, static_cast<std::uint32_t>(central_start));
  put_u16(out, 0);             // comment length
}

bool StreamingZipSink::add_entry(const std::string &name,
                                const std::string &content, bool store) {
  if (failed_ || !safe_entry_name(name)) {
    failed_ = true;
    return false;
  }
  constexpr std::size_t max_entries = 4096;
  constexpr std::size_t max_entry_bytes = 256ULL * 1024ULL * 1024ULL;
  if (central_.size() >= max_entries || content.size() > max_entry_bytes) {
    failed_ = true;
    return false;
  }
  const auto crc = compute_crc32(content);
  std::vector<unsigned char> payload;
  std::uint16_t method = 0;
  if (store) {
    payload.assign(content.begin(), content.end());
  } else {
    auto [deflated, used_method] = deflate_or_store(content);
    payload = std::move(deflated);
    method = used_method;
  }
  DirEntry entry{};
  entry.name = name;
  entry.crc32 = crc;
  entry.uncompressed_size = content.size();
  entry.compressed_size = payload.size();
  entry.method = method;
  entry.local_header_offset = archive_offset_;
  std::string header;
  header.reserve(30 + name.size());
  put_u32(header, 0x04034b50);
  put_u16(header, 20);          // version needed to extract (2.0)
  put_u16(header, 0);           // flags
  put_u16(header, entry.method);
  put_u16(header, 0);           // mod time (deterministic)
  put_u16(header, 0);           // mod date (deterministic)
  put_u32(header, entry.crc32);
  put_u32(header, static_cast<std::uint32_t>(entry.compressed_size));
  put_u32(header, static_cast<std::uint32_t>(entry.uncompressed_size));
  put_u16(header, static_cast<std::uint16_t>(entry.name.size()));
  put_u16(header, 0);           // extra field length
  header.append(entry.name);
  out_.write(header.data(), static_cast<std::streamsize>(header.size()));
  out_.write(reinterpret_cast<const char *>(payload.data()),
             static_cast<std::streamsize>(payload.size()));
  if (!out_) {
    failed_ = true;
    return false;
  }
  archive_offset_ += header.size() + payload.size();
  central_.push_back(std::move(entry));
  return true;
}

bool StreamingZipSink::finalize() {
  if (failed_) {
    return false;
  }
  std::string directory;
  directory.reserve(64 * central_.size());
  const auto central_start = archive_offset_;
  for (const auto &d : central_) {
    put_u32(directory, 0x02014b50);
    put_u16(directory, 20);      // version made by
    put_u16(directory, 20);      // version needed to extract
    put_u16(directory, 0);       // flags
    put_u16(directory, d.method);
    put_u16(directory, 0);       // mod time (deterministic)
    put_u16(directory, 0);       // mod date (deterministic)
    put_u32(directory, d.crc32);
    put_u32(directory, static_cast<std::uint32_t>(d.compressed_size));
    put_u32(directory, static_cast<std::uint32_t>(d.uncompressed_size));
    put_u16(directory, static_cast<std::uint16_t>(d.name.size()));
    put_u16(directory, 0);       // extra length
    put_u16(directory, 0);       // comment length
    put_u16(directory, 0);       // disk number start
    put_u16(directory, 0);       // internal attributes
    put_u32(directory, 0);       // external attributes
    put_u32(directory, static_cast<std::uint32_t>(d.local_header_offset));
    directory.append(d.name);
  }
  // EOCD.
  put_u32(directory, 0x06054b50);
  put_u16(directory, 0);         // disk number
  put_u16(directory, 0);         // disk with central dir
  put_u16(directory, static_cast<std::uint16_t>(central_.size()));
  put_u16(directory, static_cast<std::uint16_t>(central_.size()));
  put_u32(directory, static_cast<std::uint32_t>(directory.size() - 22));
  put_u32(directory, static_cast<std::uint32_t>(central_start));
  put_u16(directory, 0);         // comment length
  out_.write(directory.data(), static_cast<std::streamsize>(directory.size()));
  if (!out_) {
    failed_ = true;
    return false;
  }
  archive_offset_ += directory.size();
  return true;
}

} // namespace export_table
} // namespace welllog
