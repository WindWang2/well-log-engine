#include <welllog/arrow/adapter.hpp>

#include <welllog/core/checked_math.hpp>
#include <welllog/io/container_security.hpp>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(WELLLOG_ARROW_HAS_IPC)
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/io/file.h>
#include <arrow/ipc/api.h>
#endif

namespace welllog {
namespace {

[[nodiscard]] Error buffer_error(ErrorCode code = ErrorCode::invalid_buffer) {
  return Error{
      .code = code,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = code == ErrorCode::resource_exhausted
                     ? MessageKey::resource_exhausted
                     : MessageKey::buffer_data_required,
      .arguments = {},
  };
}

[[nodiscard]] Error missing_owner_error() {
  return Error{
      .code = ErrorCode::missing_owner,
      .severity = Severity::error,
      .entity_id = std::nullopt,
      .message = MessageKey::buffer_owner_required,
      .arguments = {},
  };
}

// Holds a C Data array and invokes its release callback exactly once.
struct ArrowArrayOwner {
  WellLogArrowArray array{};

  ArrowArrayOwner() = default;
  ArrowArrayOwner(const ArrowArrayOwner &) = delete;
  ArrowArrayOwner &operator=(const ArrowArrayOwner &) = delete;

  ~ArrowArrayOwner() {
    if (array.release != nullptr) {
      array.release(&array);
      array.release = nullptr;
    }
  }
};

struct MmapOwner {
  void *addr{MAP_FAILED};
  std::size_t size{};

  MmapOwner() = default;
  MmapOwner(const MmapOwner &) = delete;
  MmapOwner &operator=(const MmapOwner &) = delete;

  ~MmapOwner() {
    if (addr != MAP_FAILED && addr != nullptr && size > 0) {
      ::munmap(addr, size);
      addr = MAP_FAILED;
    }
  }
};

// Native format → ScalarType mapping (Arrow format strings for primitives).
struct NativeType {
  ScalarType type{};
  std::uint64_t width{};
};

[[nodiscard]] std::optional<NativeType>
native_type_for_format(std::string_view format) noexcept {
  if (format == "f") {
    return NativeType{ScalarType::float32, 4};
  }
  if (format == "g") {
    return NativeType{ScalarType::float64, 8};
  }
  if (format == "s") {
    return NativeType{ScalarType::int16, 2};
  }
  if (format == "i") {
    return NativeType{ScalarType::int32, 4};
  }
  if (format == "l") {
    return NativeType{ScalarType::int64, 8};
  }
  if (format == "C") {
    return NativeType{ScalarType::uint8, 1};
  }
  if (format == "S") {
    return NativeType{ScalarType::uint16, 2};
  }
  if (format == "I") {
    return NativeType{ScalarType::uint32, 4};
  }
  if (format == "L") {
    return NativeType{ScalarType::uint64, 8};
  }
  return std::nullopt;
}

// Half-float (IEEE binary16) → float64 conversion for allow_converted_copy.
[[nodiscard]] double half_to_double(std::uint16_t h) noexcept {
  const std::uint16_t sign = (h >> 15) & 1;
  const std::uint16_t exp = (h >> 10) & 0x1f;
  const std::uint16_t frac = h & 0x3ff;
  if (exp == 0) {
    if (frac == 0) {
      return sign ? -0.0 : 0.0;
    }
    // Subnormal.
    double m = frac / 1024.0;
    double val = std::ldexp(m, -14);
    return sign ? -val : val;
  }
  if (exp == 31) {
    if (frac == 0) {
      return sign ? -std::numeric_limits<double>::infinity()
                  : std::numeric_limits<double>::infinity();
    }
    return std::numeric_limits<double>::quiet_NaN();
  }
  double val = std::ldexp(1.0 + frac / 1024.0, static_cast<int>(exp) - 15);
  return sign ? -val : val;
}

// Builds a Core null bitmap (bit=1 means null) from an Arrow validity bitmap
// (bit=1 means valid), applying the array logical offset.
[[nodiscard]] Result<std::pair<NullBitmapView, BufferAccessMode>>
import_null_bitmap(const WellLogArrowArray &array, std::uint64_t length,
                   const SharedOwner &owner,
                   BufferSourceReference source) noexcept {
  try {
    // null_count == 0 ⇒ no nulls. null_count == -1 ⇒ unknown (still read
    // validity if present). null_count > 0 with null validity buffer ⇒ all
    // null (Arrow allows a null validity pointer when every value is null).
    if (length == 0 || array.null_count == 0) {
      return std::pair{NullBitmapView{}, BufferAccessMode::zero_copy};
    }
    const auto bit_offset = static_cast<std::uint64_t>(
        array.offset < 0 ? 0 : array.offset);
    const auto byte_len = (length + 7) / 8;
    auto bytes = std::make_shared<std::vector<std::uint8_t>>(byte_len, 0);
    const auto *validity =
        (array.buffers != nullptr && array.n_buffers >= 1)
            ? static_cast<const std::uint8_t *>(array.buffers[0])
            : nullptr;
    if (validity == nullptr) {
      // Treat as all-null (every Core null bit set).
      std::fill(bytes->begin(), bytes->end(),
                static_cast<std::uint8_t>(0xff));
      if (const auto rem = length % 8; rem != 0) {
        (*bytes)[byte_len - 1] = static_cast<std::uint8_t>((1u << rem) - 1u);
      }
    } else {
      for (std::uint64_t i = 0; i < length; ++i) {
        const auto arrow_bit = bit_offset + i;
        const auto valid =
            (validity[arrow_bit / 8] &
             (std::uint8_t{1} << (arrow_bit % 8))) != 0;
        if (!valid) {
          (*bytes)[i / 8] =
              static_cast<std::uint8_t>((*bytes)[i / 8] |
                                        (std::uint8_t{1} << (i % 8)));
        }
      }
    }
    // Chain Arrow owner + inverted bitmap so both live for the engine cycle.
    struct Holder {
      SharedOwner arrow;
      std::shared_ptr<const std::vector<std::uint8_t>> bits;
    };
    auto holder = std::make_shared<Holder>(
        Holder{.arrow = owner, .bits = bytes});
    auto view = NullBitmapView::from_raw(
        holder->bits->data(), length, byte_len, SharedOwner{holder},
        std::move(source));
    if (!view.has_owner()) {
      return buffer_error(ErrorCode::resource_exhausted);
    }
    return std::pair{std::move(view), BufferAccessMode::converted_copy};
  } catch (const std::bad_alloc &) {
    return buffer_error(ErrorCode::resource_exhausted);
  } catch (...) {
    return buffer_error(ErrorCode::internal_error);
  }
}

[[nodiscard]] Result<ArrowArrayImport>
import_converted_half(const WellLogArrowArray &array, std::uint64_t length,
                      std::uint64_t bit_offset, SharedOwner owner,
                      BufferSourceReference source) noexcept {
  try {
    if (array.buffers == nullptr || array.n_buffers < 2 ||
        array.buffers[1] == nullptr) {
      return buffer_error();
    }
    const auto *data = static_cast<const std::uint16_t *>(array.buffers[1]);
    auto values = std::make_shared<std::vector<double>>(length);
    for (std::uint64_t i = 0; i < length; ++i) {
      (*values)[static_cast<std::size_t>(i)] =
          half_to_double(data[bit_offset + i]);
    }
    struct Holder {
      SharedOwner arrow;
      std::shared_ptr<const std::vector<double>> values;
    };
    auto holder = std::make_shared<Holder>(
        Holder{.arrow = std::move(owner), .values = values});
    auto buffer = BufferView::from_raw(
        holder->values->data(), length, sizeof(double), ScalarType::float64,
        length * sizeof(double), SharedOwner{holder}, source,
        BufferAccessMode::converted_copy);
    if (!buffer.has_owner()) {
      return buffer_error(ErrorCode::resource_exhausted);
    }
    auto nulls = import_null_bitmap(
        array, length, SharedOwner{holder}, source);
    if (!nulls.has_value()) {
      return nulls.error();
    }
    return ArrowArrayImport{
        .values = std::move(buffer),
        .nulls = std::move(nulls.value().first),
        .values_access = BufferAccessMode::converted_copy,
        .nulls_access = nulls.value().second,
        .scalar_type = ScalarType::float64,
        .length = length,
    };
  } catch (const std::bad_alloc &) {
    return buffer_error(ErrorCode::resource_exhausted);
  } catch (...) {
    return buffer_error(ErrorCode::internal_error);
  }
}

} // namespace

std::string_view buffer_access_mode_name(BufferAccessMode mode) noexcept {
  switch (mode) {
  case BufferAccessMode::zero_copy:
    return "zero_copy";
  case BufferAccessMode::shared_copy:
    return "shared_copy";
  case BufferAccessMode::converted_copy:
    return "converted_copy";
  }
  return "unknown";
}

Result<ArrowArrayImport>
import_arrow_array(const WellLogArrowSchema &schema, WellLogArrowArray &array,
                   ArrowImportOptions options,
                   BufferSourceReference source) noexcept {
  try {
    if (schema.format == nullptr) {
      return buffer_error();
    }
    if (array.length < 0 || array.offset < 0) {
      return buffer_error();
    }
    // Nested / dictionary types are out of scope for curve samples.
    if (array.n_children != 0 || array.children != nullptr ||
        array.dictionary != nullptr || schema.n_children != 0 ||
        schema.dictionary != nullptr) {
      return buffer_error();
    }
    const auto length = static_cast<std::uint64_t>(array.length);
    const auto offset = static_cast<std::uint64_t>(array.offset);
    const auto format = std::string_view{schema.format};

    auto owner_box = std::make_shared<ArrowArrayOwner>();
    // Transfer ownership only after validation of length bounds against
    // buffers — but we need the buffers alive. Transfer now; on failure
    // the owner still releases correctly so the caller must not also
    // release. Document: on any return after transfer, caller must not
    // call release. We transfer at the end of successful validation of
    // struct fields that don't need buffers... Actually if we fail after
    // transfer, caller loses ability to release. Spec says on failure
    // leave unchanged. So validate first, then transfer.

    const auto native = native_type_for_format(format);
    const bool is_half = (format == "e");

    if (!native.has_value() && !(is_half && options.allow_converted_copy)) {
      // Unsupported without conversion policy.
      return buffer_error();
    }

    if (length > 0) {
      if (array.buffers == nullptr || array.n_buffers < 2) {
        return buffer_error();
      }
      if (array.buffers[1] == nullptr) {
        return buffer_error();
      }
    }

    // Transfer ownership into SharedOwner.
    owner_box->array = array;
    array.release = nullptr;
    array.private_data = nullptr;
    // Clear caller's view of buffers so a double-release is harder to
    // trigger accidentally (owner still has the real buffers).
    array.buffers = nullptr;
    array.length = 0;
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 0;
    array.n_children = 0;
    array.children = nullptr;
    array.dictionary = nullptr;

    SharedOwner owner{owner_box};
    if (!owner.has_value()) {
      return missing_owner_error();
    }

    const WellLogArrowArray &owned = owner_box->array;

    if (is_half) {
      return import_converted_half(owned, length, offset, std::move(owner),
                                   std::move(source));
    }

    const auto width = native->width;
    // Overflow-safe end index: offset + length.
    if (length > 0 &&
        offset > (std::numeric_limits<std::uint64_t>::max() - length)) {
      return buffer_error(ErrorCode::arithmetic_overflow);
    }
    const auto end_index = offset + length;
    if (end_index > 0 &&
        end_index > (std::numeric_limits<std::uint64_t>::max() / width)) {
      return buffer_error(ErrorCode::arithmetic_overflow);
    }
    const auto byte_capacity = end_index * width;
    const auto *base =
        length == 0 ? static_cast<const std::byte *>(nullptr)
                    : static_cast<const std::byte *>(owned.buffers[1]);
    const auto *data =
        base == nullptr ? nullptr : base + static_cast<std::ptrdiff_t>(
                                               offset * width);

    auto values = BufferView::from_raw(
        data, length, width, native->type, byte_capacity - offset * width,
        owner, source, BufferAccessMode::zero_copy);
    // byte_capacity for the view is the remaining bytes from data pointer;
    // from_raw stores it as capacity of the view region.
    if (length > 0 && !values.has_owner()) {
      return buffer_error(ErrorCode::resource_exhausted);
    }
    // Empty arrays still need an owner token for document contract.
    if (length == 0) {
      values = BufferView::from_raw(nullptr, 0, width, native->type, 0, owner,
                                    source, BufferAccessMode::zero_copy);
      if (!values.has_owner()) {
        return buffer_error(ErrorCode::resource_exhausted);
      }
    }

    auto nulls =
        import_null_bitmap(owned, length, owner, source);
    if (!nulls.has_value()) {
      return nulls.error();
    }

    return ArrowArrayImport{
        .values = std::move(values),
        .nulls = std::move(nulls.value().first),
        .values_access = BufferAccessMode::zero_copy,
        .nulls_access = nulls.value().second,
        .scalar_type = native->type,
        .length = length,
    };
  } catch (const std::bad_alloc &) {
    return buffer_error(ErrorCode::resource_exhausted);
  } catch (...) {
    return buffer_error(ErrorCode::internal_error);
  }
}

Result<BufferView>
import_mmap_scalar_column(const std::filesystem::path &path, ScalarType type,
                          std::uint64_t length,
                          BufferSourceReference source) noexcept {
  try {
    const auto width = scalar_size_bytes(type);
    if (width == 0) {
      return buffer_error();
    }
    const auto need_bytes = checked_mul_u64(length, width);
    if (!need_bytes.has_value()) {
      return buffer_error(ErrorCode::arithmetic_overflow);
    }
    if (const auto err = validate_buffer_extent(length, width, width, *need_bytes);
        err.has_value()) {
      return *err;
    }

    const auto path_str = path.string();
    // Reject empty / clearly non-file path strings that would confuse open.
    if (path_str.empty() || path_str.find('\0') != std::string::npos) {
      return buffer_error(ErrorCode::unresolved_buffer);
    }
    const int fd = ::open(path_str.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
      return buffer_error(ErrorCode::unresolved_buffer);
    }
    struct stat st {};
    if (::fstat(fd, &st) != 0) {
      ::close(fd);
      return buffer_error(ErrorCode::unresolved_buffer);
    }
    if (st.st_size < 0 ||
        static_cast<std::uint64_t>(st.st_size) < *need_bytes) {
      ::close(fd);
      return buffer_error();
    }
    const auto map_size = static_cast<std::size_t>(st.st_size);
    if (static_cast<std::uint64_t>(map_size) >
        default_container_security_limits().max_mmap_file_bytes) {
      ::close(fd);
      return buffer_error(ErrorCode::resource_exhausted);
    }
    if (map_size == 0) {
      ::close(fd);
      // Empty file / empty column — still require an owner token.
      auto empty = std::make_shared<std::vector<std::byte>>();
      return BufferView::from_raw(nullptr, 0, width, type, 0,
                                  SharedOwner{empty}, std::move(source),
                                  BufferAccessMode::zero_copy);
    }
    void *addr =
        ::mmap(nullptr, map_size, PROT_READ, MAP_PRIVATE, fd, 0);
    ::close(fd);
    if (addr == MAP_FAILED) {
      return buffer_error(ErrorCode::resource_exhausted);
    }
    auto owner = std::make_shared<MmapOwner>();
    owner->addr = addr;
    owner->size = map_size;
    if (source.uri.empty()) {
      source.uri = path_str;
    }
    auto view = BufferView::from_raw(addr, length, width, type, *need_bytes,
                                     SharedOwner{owner}, std::move(source),
                                     BufferAccessMode::zero_copy);
    if (!view.has_owner()) {
      return buffer_error(ErrorCode::resource_exhausted);
    }
    return view;
  } catch (const std::bad_alloc &) {
    return buffer_error(ErrorCode::resource_exhausted);
  } catch (...) {
    return buffer_error(ErrorCode::internal_error);
  }
}

bool arrow_ipc_available() noexcept {
#if defined(WELLLOG_ARROW_HAS_IPC)
  return true;
#else
  return false;
#endif
}

Result<ArrowArrayImport>
import_arrow_ipc_file_column(const std::filesystem::path &path,
                             int column_index, ArrowImportOptions options,
                             BufferSourceReference source) noexcept {
#if !defined(WELLLOG_ARROW_HAS_IPC)
  (void)path;
  (void)column_index;
  (void)options;
  (void)source;
  return buffer_error(ErrorCode::unresolved_buffer);
#else
  try {
    if (column_index < 0) {
      return buffer_error();
    }
    auto file_result =
        arrow::io::ReadableFile::Open(path.string());
    if (!file_result.ok()) {
      return buffer_error(ErrorCode::unresolved_buffer);
    }
    auto file = *std::move(file_result);
    // Prefer File format; fall back to stream.
    std::shared_ptr<arrow::RecordBatch> batch;
    {
      auto file_reader_result =
          arrow::ipc::RecordBatchFileReader::Open(file);
      if (file_reader_result.ok()) {
        auto reader = *std::move(file_reader_result);
        if (reader->num_record_batches() < 1) {
          return buffer_error();
        }
        auto batch_result = reader->ReadRecordBatch(0);
        if (!batch_result.ok()) {
          return buffer_error();
        }
        batch = *std::move(batch_result);
      } else {
        auto stream_result =
            arrow::ipc::RecordBatchStreamReader::Open(file);
        if (!stream_result.ok()) {
          return buffer_error(ErrorCode::unresolved_buffer);
        }
        auto reader = *std::move(stream_result);
        auto batch_result = reader->Next();
        if (!batch_result.ok() || !*batch_result) {
          return buffer_error();
        }
        batch = *std::move(batch_result);
      }
    }
    if (batch == nullptr || column_index >= batch->num_columns()) {
      return buffer_error();
    }
    auto column = batch->column(column_index);
    if (column == nullptr) {
      return buffer_error();
    }
    // Export to C Data Interface, then reuse the zero-copy import path.
    // ExportArray's release keeps the Arrow Array refcount alive.
    // Layout-compatible with Apache Arrow's ArrowSchema / ArrowArray.
    WellLogArrowSchema schema{};
    WellLogArrowArray c_array{};
    auto *as_schema = reinterpret_cast<::ArrowSchema *>(&schema);
    auto *as_array = reinterpret_cast<::ArrowArray *>(&c_array);
    auto export_status = arrow::ExportArray(*column, as_array, as_schema);
    if (!export_status.ok()) {
      return buffer_error();
    }
    if (source.uri.empty()) {
      source.uri = path.string();
    }
    auto imported =
        import_arrow_array(schema, c_array, options, std::move(source));
    // import_arrow_array takes array ownership; schema still needs release.
    if (schema.release != nullptr) {
      schema.release(&schema);
    }
    if (!imported.has_value() && c_array.release != nullptr) {
      // import failed before transfer — release ourselves.
      c_array.release(&c_array);
    }
    return imported;
  } catch (const std::bad_alloc &) {
    return buffer_error(ErrorCode::resource_exhausted);
  } catch (...) {
    return buffer_error(ErrorCode::internal_error);
  }
#endif
}

} // namespace welllog
