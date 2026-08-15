#include <welllog/core/document.hpp>

#include <algorithm>
#include <cstring>

namespace welllog {
namespace {

template <typename T>
[[nodiscard]] double load_as_double(const std::byte *data) noexcept {
  T value{};
  std::memcpy(&value, data, sizeof(T));
  return static_cast<double>(value);
}

} // namespace

std::uint64_t scalar_size_bytes(ScalarType type) noexcept {
  switch (type) {
  case ScalarType::float32:
  case ScalarType::int32:
  case ScalarType::uint32:
    return 4;
  case ScalarType::float64:
  case ScalarType::int64:
  case ScalarType::uint64:
    return 8;
  case ScalarType::int16:
  case ScalarType::uint16:
    return 2;
  case ScalarType::uint8:
    return 1;
  }
  return 0;
}

std::string_view scalar_type_name(ScalarType type) noexcept {
  switch (type) {
  case ScalarType::float32:
    return "float32";
  case ScalarType::float64:
    return "float64";
  case ScalarType::int16:
    return "int16";
  case ScalarType::int32:
    return "int32";
  case ScalarType::int64:
    return "int64";
  case ScalarType::uint8:
    return "uint8";
  case ScalarType::uint16:
    return "uint16";
  case ScalarType::uint32:
    return "uint32";
  case ScalarType::uint64:
    return "uint64";
  }
  return {};
}

std::optional<ScalarType> parse_scalar_type(std::string_view name) noexcept {
  constexpr ScalarType types[] = {
      ScalarType::float32, ScalarType::float64, ScalarType::int16,
      ScalarType::int32,   ScalarType::int64,   ScalarType::uint8,
      ScalarType::uint16,  ScalarType::uint32,  ScalarType::uint64,
  };
  for (const auto type : types) {
    if (scalar_type_name(type) == name) {
      return type;
    }
  }
  return std::nullopt;
}

std::string_view depth_domain_name(DepthDomain domain) noexcept {
  switch (domain) {
  case DepthDomain::measured_depth:
    return "md";
  case DepthDomain::true_vertical_depth:
    return "tvd";
  case DepthDomain::true_vertical_depth_subsea:
    return "tvdss";
  case DepthDomain::source_index:
    return "sourceIndex";
  }
  return "";
}

std::optional<DepthDomain>
parse_depth_domain(std::string_view name) noexcept {
  if (name == "md") {
    return DepthDomain::measured_depth;
  }
  if (name == "tvd") {
    return DepthDomain::true_vertical_depth;
  }
  if (name == "tvdss") {
    return DepthDomain::true_vertical_depth_subsea;
  }
  if (name == "sourceIndex") {
    return DepthDomain::source_index;
  }
  return std::nullopt;
}

struct SharedOwner::Impl {
  std::shared_ptr<const void> owner;
};

SharedOwner::SharedOwner() = default;
SharedOwner::~SharedOwner() = default;
SharedOwner::SharedOwner(const SharedOwner &) = default;
SharedOwner &SharedOwner::operator=(const SharedOwner &) = default;
SharedOwner::SharedOwner(SharedOwner &&) noexcept = default;
SharedOwner &SharedOwner::operator=(SharedOwner &&) noexcept = default;

SharedOwner::SharedOwner(std::shared_ptr<const void> owner) noexcept {
  try {
    impl_ = std::make_shared<Impl>(Impl{.owner = std::move(owner)});
  } catch (...) {
    impl_.reset();
  }
}

bool SharedOwner::has_value() const noexcept {
  return impl_ != nullptr && static_cast<bool>(impl_->owner);
}

struct BufferView::Impl {
  const std::byte *data{};
  std::uint64_t length{};
  std::uint64_t stride_bytes{};
  ScalarType scalar_type{ScalarType::float64};
  std::uint64_t byte_capacity{};
  SharedOwner owner;
  BufferSourceReference source;
  BufferAccessMode access_mode{BufferAccessMode::zero_copy};
};

BufferView::BufferView() = default;
BufferView::~BufferView() = default;
BufferView::BufferView(const BufferView &) = default;
BufferView &BufferView::operator=(const BufferView &) = default;
BufferView::BufferView(BufferView &&) noexcept = default;
BufferView &BufferView::operator=(BufferView &&) noexcept = default;

BufferView::BufferView(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

BufferView BufferView::from_raw(const void *data, std::uint64_t length,
                                std::uint64_t stride_bytes,
                                ScalarType scalar_type,
                                std::uint64_t byte_capacity, SharedOwner owner,
                                BufferSourceReference source,
                                BufferAccessMode access_mode) noexcept {
  try {
    return BufferView{std::make_shared<Impl>(Impl{
        .data = static_cast<const std::byte *>(data),
        .length = length,
        .stride_bytes = stride_bytes,
        .scalar_type = scalar_type,
        .byte_capacity = byte_capacity,
        .owner = std::move(owner),
        .source = std::move(source),
        .access_mode = access_mode,
    })};
  } catch (...) {
    return {};
  }
}

const std::byte *BufferView::data() const noexcept {
  return impl_ == nullptr ? nullptr : impl_->data;
}

std::uint64_t BufferView::length() const noexcept {
  return impl_ == nullptr ? 0 : impl_->length;
}

std::uint64_t BufferView::stride_bytes() const noexcept {
  return impl_ == nullptr ? 0 : impl_->stride_bytes;
}

ScalarType BufferView::scalar_type() const noexcept {
  return impl_ == nullptr ? ScalarType::float64 : impl_->scalar_type;
}

std::uint64_t BufferView::byte_capacity() const noexcept {
  return impl_ == nullptr ? 0 : impl_->byte_capacity;
}

bool BufferView::has_owner() const noexcept {
  return impl_ != nullptr && impl_->owner.has_value();
}

const BufferSourceReference &BufferView::source() const noexcept {
  static const BufferSourceReference empty;
  return impl_ == nullptr ? empty : impl_->source;
}

BufferAccessMode BufferView::access_mode() const noexcept {
  return impl_ == nullptr ? BufferAccessMode::zero_copy : impl_->access_mode;
}

std::optional<double>
BufferView::value_as_double(std::uint64_t index) const noexcept {
  if (impl_ == nullptr || impl_->data == nullptr || index >= impl_->length) {
    return std::nullopt;
  }
  const auto element_size = scalar_size_bytes(impl_->scalar_type);
  if (impl_->stride_bytes < element_size ||
      impl_->byte_capacity < element_size ||
      index > (impl_->byte_capacity - element_size) / impl_->stride_bytes) {
    return std::nullopt;
  }
  const auto *value = impl_->data + index * impl_->stride_bytes;
  switch (impl_->scalar_type) {
  case ScalarType::float32:
    return load_as_double<float>(value);
  case ScalarType::float64:
    return load_as_double<double>(value);
  case ScalarType::int16:
    return load_as_double<std::int16_t>(value);
  case ScalarType::int32:
    return load_as_double<std::int32_t>(value);
  case ScalarType::int64:
    return load_as_double<std::int64_t>(value);
  case ScalarType::uint8:
    return load_as_double<std::uint8_t>(value);
  case ScalarType::uint16:
    return load_as_double<std::uint16_t>(value);
  case ScalarType::uint32:
    return load_as_double<std::uint32_t>(value);
  case ScalarType::uint64:
    return load_as_double<std::uint64_t>(value);
  }
  return std::nullopt;
}

// --- CompositeBufferView (#196) --------------------------------------------
// A logical buffer spanning N immutable BufferView segments. Random access
// walks the segments to locate the one holding element `index`, then delegates
// to that segment's value_as_double (reusing its proven bounds/capacity
// checks). No contiguous copy is ever made; each segment's SharedOwner keeps
// its physical block alive independently.
struct CompositeBufferView::Impl {
  std::vector<BufferView> segments;
  std::uint64_t total_length{};
  ScalarType scalar_type{ScalarType::float64};
};

CompositeBufferView::CompositeBufferView() = default;
CompositeBufferView::~CompositeBufferView() = default;
CompositeBufferView::CompositeBufferView(const CompositeBufferView &) = default;
CompositeBufferView &CompositeBufferView::
operator=(const CompositeBufferView &) = default;
CompositeBufferView::CompositeBufferView(CompositeBufferView &&) noexcept =
    default;
CompositeBufferView &CompositeBufferView::
operator=(CompositeBufferView &&) noexcept = default;

CompositeBufferView::CompositeBufferView(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

CompositeBufferView CompositeBufferView::from_segments(
    std::vector<BufferView> segments) noexcept {
  try {
    if (segments.empty()) {
      return {};
    }
    // All segments must share a scalar_type and be non-empty with data.
    const auto type = segments.front().scalar_type();
    std::uint64_t total = 0;
    for (const auto &s : segments) {
      if (s.data() == nullptr || s.length() == 0 ||
          s.scalar_type() != type) {
        return {};
      }
      total += s.length();
    }
    auto impl = std::make_shared<Impl>();
    impl->segments = std::move(segments);
    impl->total_length = total;
    impl->scalar_type = type;
    return CompositeBufferView{std::move(impl)};
  } catch (...) {
    return {};
  }
}

bool CompositeBufferView::empty() const noexcept {
  return impl_ == nullptr || impl_->segments.empty();
}

ScalarType CompositeBufferView::scalar_type() const noexcept {
  return impl_ == nullptr ? ScalarType::float64 : impl_->scalar_type;
}

std::uint64_t CompositeBufferView::length() const noexcept {
  return impl_ == nullptr ? 0 : impl_->total_length;
}

std::optional<double>
CompositeBufferView::value_as_double(std::uint64_t index) const noexcept {
  if (impl_ == nullptr || index >= impl_->total_length) {
    return std::nullopt;
  }
  // Walk segments to find the one holding this element, decrementing the
  // index by each skipped segment's length.
  std::uint64_t remaining = index;
  for (const auto &segment : impl_->segments) {
    const auto seg_len = segment.length();
    if (remaining < seg_len) {
      return segment.value_as_double(remaining);
    }
    remaining -= seg_len;
  }
  return std::nullopt;
}

std::span<const BufferView> CompositeBufferView::segments() const noexcept {
  if (impl_ == nullptr) {
    return {};
  }
  return std::span<const BufferView>{impl_->segments};
}

// --- CurveBuffer (#197) -----------------------------------------------------
// Forwards the three index-based accessors to whichever underlying view the
// curve carries. The single-block path is the common case; the composite path
// is the append case. No consumer branches on the variant — they call these.
CurveBuffer::CurveBuffer(BufferView view) noexcept
    : single_(std::move(view)), is_composite_(false) {}

CurveBuffer::CurveBuffer(CompositeBufferView composite) noexcept
    : composite_(std::move(composite)), is_composite_(true) {}

bool CurveBuffer::empty() const noexcept {
  return is_composite_ ? composite_.empty() : single_.length() == 0;
}

std::uint64_t CurveBuffer::length() const noexcept {
  return is_composite_ ? composite_.length() : single_.length();
}

ScalarType CurveBuffer::scalar_type() const noexcept {
  return is_composite_ ? composite_.scalar_type() : single_.scalar_type();
}

std::optional<double>
CurveBuffer::value_as_double(std::uint64_t index) const noexcept {
  return is_composite_ ? composite_.value_as_double(index)
                       : single_.value_as_double(index);
}

bool CurveBuffer::is_composite() const noexcept { return is_composite_; }

const BufferView &CurveBuffer::as_single() const noexcept {
  // Returns the single-block view (default-constructed/empty when this curve
  // carries a composite). Callers must check is_composite() first.
  return single_;
}

std::span<const BufferView> CurveBuffer::segments() const noexcept {
  return is_composite_ ? composite_.segments() : std::span<const BufferView>{};
}

struct NullBitmapView::Impl {
  const std::uint8_t *data{};
  std::uint64_t bit_length{};
  std::uint64_t byte_capacity{};
  SharedOwner owner;
  BufferSourceReference source;
};

NullBitmapView::NullBitmapView() = default;
NullBitmapView::~NullBitmapView() = default;
NullBitmapView::NullBitmapView(const NullBitmapView &) = default;
NullBitmapView &NullBitmapView::operator=(const NullBitmapView &) = default;
NullBitmapView::NullBitmapView(NullBitmapView &&) noexcept = default;
NullBitmapView &NullBitmapView::operator=(NullBitmapView &&) noexcept = default;

NullBitmapView::NullBitmapView(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

NullBitmapView NullBitmapView::from_raw(const std::uint8_t *data,
                                        std::uint64_t bit_length,
                                        std::uint64_t byte_capacity,
                                        SharedOwner owner,
                                        BufferSourceReference source) noexcept {
  try {
    return NullBitmapView{std::make_shared<Impl>(Impl{
        .data = data,
        .bit_length = bit_length,
        .byte_capacity = byte_capacity,
        .owner = std::move(owner),
        .source = std::move(source),
    })};
  } catch (...) {
    return {};
  }
}

bool NullBitmapView::empty() const noexcept {
  return impl_ == nullptr || impl_->bit_length == 0;
}

bool NullBitmapView::is_null(std::uint64_t index) const noexcept {
  // data may legally be null when bit_length is 0, but from_raw (public API)
  // does not reject bit_length > 0 with a null data pointer — guard so the
  // accessor returns false instead of dereferencing null (issue #478).
  return impl_ != nullptr && impl_->data != nullptr &&
         index < impl_->bit_length &&
         (impl_->data[index / 8] & (std::uint8_t{1} << (index % 8))) != 0;
}

const std::uint8_t *NullBitmapView::data() const noexcept {
  return impl_ == nullptr ? nullptr : impl_->data;
}

std::uint64_t NullBitmapView::bit_length() const noexcept {
  return impl_ == nullptr ? 0 : impl_->bit_length;
}

std::uint64_t NullBitmapView::byte_capacity() const noexcept {
  return impl_ == nullptr ? 0 : impl_->byte_capacity;
}

bool NullBitmapView::has_owner() const noexcept {
  return impl_ != nullptr && impl_->owner.has_value();
}

const BufferSourceReference &NullBitmapView::source() const noexcept {
  static const BufferSourceReference empty;
  return impl_ == nullptr ? empty : impl_->source;
}

struct WellLogDocument::Impl {
  EntityId id;
  DocumentRevision revision;
  std::vector<SamplingAxis> axes;
  std::vector<Curve> curves;
  std::vector<QcMask> qc_masks;
  std::vector<Interval> intervals;
  std::vector<Marker> markers;
  std::vector<SymbolOccurrence> symbols;
  std::vector<ImageSource> image_sources;
  std::vector<TextAnnotation> annotations;
  std::vector<CustomLayerSource> custom_sources;
};

WellLogDocument::WellLogDocument() = default;

WellLogDocument::WellLogDocument(std::shared_ptr<const Impl> impl)
    : impl_(std::move(impl)) {}

EntityId WellLogDocument::id() const noexcept {
  return impl_ == nullptr ? EntityId{} : impl_->id;
}

DocumentRevision WellLogDocument::revision() const noexcept {
  return impl_ == nullptr ? DocumentRevision{} : impl_->revision;
}

std::span<const SamplingAxis> WellLogDocument::sampling_axes() const noexcept {
  return impl_ == nullptr ? std::span<const SamplingAxis>{}
                          : std::span<const SamplingAxis>{impl_->axes};
}

std::span<const Curve> WellLogDocument::curves() const noexcept {
  return impl_ == nullptr ? std::span<const Curve>{}
                          : std::span<const Curve>{impl_->curves};
}

std::span<const QcMask> WellLogDocument::qc_masks() const noexcept {
  return impl_ == nullptr ? std::span<const QcMask>{}
                          : std::span<const QcMask>{impl_->qc_masks};
}

std::span<const Interval> WellLogDocument::intervals() const noexcept {
  return impl_ == nullptr ? std::span<const Interval>{}
                          : std::span<const Interval>{impl_->intervals};
}

std::span<const Marker> WellLogDocument::markers() const noexcept {
  return impl_ == nullptr ? std::span<const Marker>{}
                          : std::span<const Marker>{impl_->markers};
}

std::span<const SymbolOccurrence> WellLogDocument::symbols() const noexcept {
  return impl_ == nullptr
             ? std::span<const SymbolOccurrence>{}
             : std::span<const SymbolOccurrence>{impl_->symbols};
}

std::span<const ImageSource>
WellLogDocument::image_sources() const noexcept {
  return impl_ == nullptr
             ? std::span<const ImageSource>{}
             : std::span<const ImageSource>{impl_->image_sources};
}

std::span<const TextAnnotation> WellLogDocument::annotations() const noexcept {
  return impl_ == nullptr ? std::span<const TextAnnotation>{}
                          : std::span<const TextAnnotation>{impl_->annotations};
}

std::span<const CustomLayerSource>
WellLogDocument::custom_sources() const noexcept {
  return impl_ == nullptr
             ? std::span<const CustomLayerSource>{}
             : std::span<const CustomLayerSource>{impl_->custom_sources};
}

struct WellLogDocumentBuilder::Impl {
  EntityId id;
  DocumentRevision revision;
  std::vector<SamplingAxis> axes;
  std::vector<Curve> curves;
  std::vector<QcMask> qc_masks;
  std::vector<Interval> intervals;
  std::vector<Marker> markers;
  std::vector<SymbolOccurrence> symbols;
  std::vector<ImageSource> image_sources;
  std::vector<TextAnnotation> annotations;
  std::vector<CustomLayerSource> custom_sources;
  bool allocation_failed{};
};

WellLogDocumentBuilder::WellLogDocumentBuilder(
    EntityId id, DocumentRevision revision) noexcept {
  try {
    impl_ = std::make_unique<Impl>(Impl{.id = id,
                                        .revision = revision,
                                        .axes = {},
                                        .curves = {},
                                        .qc_masks = {},
                                        .intervals = {},
                                        .markers = {},
                                        .symbols = {},
                                        .image_sources = {},
                                        .annotations = {},
                                        .custom_sources = {},
                                        .allocation_failed = false});
  } catch (...) {
    impl_.reset();
  }
}

WellLogDocumentBuilder::~WellLogDocumentBuilder() = default;
WellLogDocumentBuilder::WellLogDocumentBuilder(
    WellLogDocumentBuilder &&) noexcept = default;
WellLogDocumentBuilder &
WellLogDocumentBuilder::operator=(WellLogDocumentBuilder &&) noexcept = default;

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_sampling_axis(const SamplingAxis &axis) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->axes.push_back(axis);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_curve(const Curve &curve) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  try {
    impl_->curves.push_back(curve);
  } catch (...) {
    impl_->allocation_failed = true;
  }
  return *this;
}

namespace {

template <typename Collection>
void append_entity(Collection &collection, const typename Collection::value_type &entity,
                   bool &allocation_failed) {
  try {
    collection.push_back(entity);
  } catch (...) {
    allocation_failed = true;
  }
}

} // namespace

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_qc_mask(const QcMask &mask) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->qc_masks, mask, impl_->allocation_failed);
  return *this;
}

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_interval(const Interval &interval) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->intervals, interval, impl_->allocation_failed);
  return *this;
}

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_marker(const Marker &marker) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->markers, marker, impl_->allocation_failed);
  return *this;
}

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_symbol(const SymbolOccurrence &symbol) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->symbols, symbol, impl_->allocation_failed);
  return *this;
}

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_image_source(const ImageSource &source) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->image_sources, source, impl_->allocation_failed);
  return *this;
}

WellLogDocumentBuilder &WellLogDocumentBuilder::add_annotation(
    const TextAnnotation &annotation) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->annotations, annotation, impl_->allocation_failed);
  return *this;
}

WellLogDocumentBuilder &
WellLogDocumentBuilder::add_custom_source(
    const CustomLayerSource &source) noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return *this;
  }
  append_entity(impl_->custom_sources, source, impl_->allocation_failed);
  return *this;
}

WellLogDocument WellLogDocumentBuilder::build() const noexcept {
  if (impl_ == nullptr || impl_->allocation_failed) {
    return {};
  }
  try {
    auto document = std::make_shared<WellLogDocument::Impl>();
    document->id = impl_->id;
    document->revision = impl_->revision;
    document->axes = impl_->axes;
    document->curves = impl_->curves;
    document->qc_masks = impl_->qc_masks;
    document->intervals = impl_->intervals;
    document->markers = impl_->markers;
    document->symbols = impl_->symbols;
    document->image_sources = impl_->image_sources;
    document->annotations = impl_->annotations;
    document->custom_sources = impl_->custom_sources;
    // Stamp derived freshness against the just-built curve set (#159).
    for (auto &curve : document->curves) {
      if (!curve.derived.has_value()) {
        continue;
      }
      const auto &prov = *curve.derived;
      const auto input = std::find_if(
          document->curves.begin(), document->curves.end(),
          [&](const Curve &candidate) {
            return candidate.id == prov.input_curve_id;
          });
      if (input == document->curves.end()) {
        curve.derived->freshness = DerivedFreshness::stale;
        continue;
      }
      const auto length = input->values.length();
      const void *data = nullptr;
      if (input->values.is_composite()) {
        const auto segs = input->values.segments();
        if (!segs.empty()) {
          data = segs.front().data();
        }
      } else {
        data = input->values.as_single().data();
      }
      curve.derived->freshness =
          (data == prov.input_values_data &&
           length == prov.input_values_length)
              ? DerivedFreshness::current
              : DerivedFreshness::stale;
    }
    return WellLogDocument{std::move(document)};
  } catch (...) {
    return {};
  }
}

QcState qc_state_at(const WellLogDocument &document, const Curve &curve,
                    std::uint64_t sample_index) noexcept {
  const auto masks = document.qc_masks();
  const auto found = std::find_if(
      masks.begin(), masks.end(), [&](const QcMask &mask) {
        // Prefer explicit curve.qc_mask_id; otherwise bind by mask.curve_id so
        // a patch can attach a mask without rewriting the raw Curve (#159).
        if (!curve.qc_mask_id.is_nil()) {
          return mask.id == curve.qc_mask_id;
        }
        return mask.curve_id == curve.id;
      });
  if (found == masks.end()) {
    return QcState::valid;
  }
  if (found->states.scalar_type() != ScalarType::uint8 ||
      sample_index >= found->states.length()) {
    return QcState::valid;
  }
  const auto value = found->states.value_as_double(sample_index);
  if (!value.has_value()) {
    return QcState::valid;
  }
  const auto code = static_cast<std::uint8_t>(*value);
  if (code > static_cast<std::uint8_t>(QcState::user_excluded)) {
    return QcState::valid;
  }
  return static_cast<QcState>(code);
}

DerivedFreshness compute_derived_freshness(const WellLogDocument &document,
                                           const Curve &curve) noexcept {
  if (!curve.derived.has_value()) {
    return DerivedFreshness::current;
  }
  const auto &prov = *curve.derived;
  const auto curves = document.curves();
  const auto input =
      std::find_if(curves.begin(), curves.end(), [&](const Curve &candidate) {
        return candidate.id == prov.input_curve_id;
      });
  if (input == curves.end()) {
    return DerivedFreshness::stale;
  }
  // Input buffer identity: length + data pointer of the logical buffer.
  const auto length = input->values.length();
  const void *data = nullptr;
  if (input->values.is_composite()) {
    const auto segs = input->values.segments();
    if (!segs.empty()) {
      data = segs.front().data();
    }
  } else {
    data = input->values.as_single().data();
  }
  if (data != prov.input_values_data || length != prov.input_values_length) {
    return DerivedFreshness::stale;
  }
  return DerivedFreshness::current;
}

} // namespace welllog
