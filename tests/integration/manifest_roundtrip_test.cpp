#include <welllog/io/manifest.hpp>
#include <welllog/session/session.hpp>

#include <cstdlib>
#include <iostream>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace {

using namespace welllog;

[[noreturn]] void fail(std::string_view message) {
  std::cerr << "FAIL: " << message << '\n';
  // _Exit, not std::exit: avoid CRT/DLL teardown while LOD/frame worker
  // jthreads are still mid-flight (Windows loader-lock deadlock, #241).
  std::_Exit(EXIT_FAILURE);
}

void require(bool condition, std::string_view message) {
  if (!condition) {
    fail(message);
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "test UUID must be valid");
  return *parsed;
}

void manifest_round_trip_rebinds_external_buffers() {
  const auto document_id = id("01234567-89ab-4cde-8fab-0123456789ab");
  const auto axis_id = id("12345678-9abc-4def-8abc-123456789abc");
  const auto curve_id = id("23456789-abcd-4efa-8bcd-23456789abcd");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{900.25, 900.5, 900.75});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{12.5F, 25.0F, 37.5F});
  auto nulls = std::make_shared<const std::vector<std::uint8_t>>(
      std::initializer_list<std::uint8_t>{0b00000010});

  const auto depth_view =
      BufferView::from_vector(depths, BufferSourceReference{
                                          .uri = "mmap://well-a.bin#depth",
                                          .checksum = "sha256:depth",
                                          .byte_offset = 64,
                                      });
  const auto value_view =
      BufferView::from_vector(values, BufferSourceReference{
                                          .uri = "mmap://well-a.bin#gr",
                                          .checksum = "sha256:gr",
                                          .byte_offset = 4096,
                                      });
  const auto null_view = NullBitmapView::from_raw(
      nulls->data(), 3, nulls->size(), SharedOwner{nulls},
      BufferSourceReference{
          .uri = "mmap://well-a.bin#gr-null",
          .checksum = "sha256:gr-null",
          .byte_offset = 8192,
      });

  WellLogDocumentBuilder builder(document_id, DocumentRevision{42});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = depth_view,
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "伽马",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = value_view,
      .nulls = null_view,
  });

  const auto encoded = ManifestCodec::write(builder.build());
  require(encoded.has_value(), "valid document manifest must serialize");
  require(encoded.value().text().find("\"schemaVersion\":2") !=
              std::string::npos,
          "manifest must carry its schema version");
  require(encoded.value().text().find(
              "\"requiredSdkVersion\":\">=0.1.0 <1.0.0\"") != std::string::npos,
          "manifest must carry its required SDK version");
  require(encoded.value().text().find("mmap://well-a.bin#gr") !=
              std::string::npos,
          "manifest must retain external data references");
  require(encoded.value().text().find("900.25") == std::string::npos,
          "manifest must not inline depth samples");
  require(encoded.value().text().find("37.5") == std::string::npos,
          "manifest must not inline curve samples");

  ManifestResolvers resolvers{
      .buffer = [&](const BufferDescriptor &descriptor) -> Result<BufferView> {
        if (descriptor.source.uri == depth_view.source().uri) {
          return depth_view;
        }
        if (descriptor.source.uri == value_view.source().uri) {
          return value_view;
        }
        return Error{
            .code = ErrorCode::unresolved_buffer,
            .entity_id = std::nullopt,
            .message = MessageKey::external_buffer_unresolved,
            .arguments = {},
        };
      },
      .null_bitmap = [&](const NullBitmapDescriptor &descriptor)
          -> Result<NullBitmapView> {
        if (descriptor.source.uri == null_view.source().uri) {
          return null_view;
        }
        return Error{
            .code = ErrorCode::unresolved_buffer,
            .entity_id = std::nullopt,
            .message = MessageKey::external_buffer_unresolved,
            .arguments = {},
        };
      },
      .image_tile = {},
  };
  const auto restored = ManifestCodec::read(encoded.value().text(), resolvers);
  require(restored.has_value(), "manifest must restore through host resolvers");

  WellLogSession session;
  const auto receipt = session.execute(SetDocumentCommand{restored.value()});
  require(receipt.has_value(),
          "restored document must pass session validation");
  require(receipt.value().document_id == document_id,
          "manifest must restore document identity");
  require(receipt.value().document_revision == DocumentRevision{42},
          "manifest must restore document revision");
  require(session.diagnostics().size() == 1,
          "restored null bitmap must retain missing-data semantics");

  // The current schema version is 2 (bumped in #183 for imageSources/customSources).
  // A reader at a DIFFERENT version rejects; patch up to :3 to assert that.
  auto unsupported_version = std::string{encoded.value().text()};
  unsupported_version.replace(unsupported_version.find("\"schemaVersion\":2"),
                              std::string_view{"\"schemaVersion\":2"}.size(),
                              "\"schemaVersion\":3");
  const auto unsupported = ManifestCodec::read(unsupported_version, resolvers);
  require(!unsupported.has_value(),
          "unsupported manifest schema version must be rejected");
  require(unsupported.error().message ==
              MessageKey::manifest_schema_unsupported,
          "schema rejection must expose a stable localizable message key");

  auto extra_field = std::string{encoded.value().text()};
  extra_field.insert(1, "\"unexpected\":true,");
  const auto schema_mismatch = ManifestCodec::read(extra_field, resolvers);
  require(!schema_mismatch.has_value(),
          "manifest fields outside the published schema must be rejected");
  require(schema_mismatch.error().message == MessageKey::manifest_invalid,
          "schema mismatch must expose the stable manifest error key");

  auto empty_source = std::string{encoded.value().text()};
  empty_source.replace(
      empty_source.find("\"uri\":\"mmap://well-a.bin#depth\""),
      std::string_view{"\"uri\":\"mmap://well-a.bin#depth\""}.size(),
      "\"uri\":\"\"");
  const auto invalid_source = ManifestCodec::read(empty_source, resolvers);
  require(!invalid_source.has_value(),
          "manifest buffer source URI must satisfy the published schema");

  auto zero_length = std::string{encoded.value().text()};
  zero_length.replace(zero_length.find("\"length\":3"),
                      std::string_view{"\"length\":3"}.size(), "\"length\":0");
  const auto invalid_length = ManifestCodec::read(zero_length, resolvers);
  require(!invalid_length.has_value(),
          "manifest buffer dimensions must satisfy the published schema");
}

void manifest_writer_rejects_documents_outside_schema() {
  const auto document_id = id("3456789a-bcde-4fab-8cde-3456789abcde");
  WellLogDocumentBuilder empty_builder(document_id, DocumentRevision{1});
  const auto empty_result = ManifestCodec::write(empty_builder.build());
  require(!empty_result.has_value(),
          "manifest writer must reject documents without axes and curves");

  const auto axis_id = id("456789ab-cdef-4abc-8def-456789abcdef");
  const auto curve_id = id("56789abc-defa-4bcd-8efa-56789abcdefa");
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{100.0});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{1.0F});
  WellLogDocumentBuilder missing_source_builder(document_id,
                                                DocumentRevision{1});
  missing_source_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(depths),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  missing_source_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  const auto missing_source_result =
      ManifestCodec::write(missing_source_builder.build());
  require(!missing_source_result.has_value(),
          "manifest writer must reject buffers without external references");
}

} // namespace

namespace {

// A document carrying one ImageSource + one CustomLayerSource (all 4 primitive
// kinds + a clip path). Built with minimal external-reference buffers so it
// passes can_write_manifest_document; the image/custom sources are pure
// metadata + source identity (no buffers to resolve).
WellLogDocument make_image_custom_document(EntityId document_id,
                                           EntityId axis_id,
                                           EntityId curve_id,
                                           EntityId image_id,
                                           EntityId custom_id) {
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0, 1001.0, 1002.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0, 20.0, 30.0});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{9});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates =
          BufferView::from_vector(depths, BufferSourceReference{
                                              .uri = "mmap://img-doc#depth",
                                              .checksum = "depth-sha",
                                              .byte_offset = 0,
                                          }),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "Gamma ray",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values =
          BufferView::from_vector(values, BufferSourceReference{
                                              .uri = "mmap://img-doc#gr",
                                              .checksum = "gr-sha",
                                              .byte_offset = 0,
                                          }),
      .nulls = {},
  });
  builder.add_image_source(ImageSource{
      .id = image_id,
      .width_px = 2048,
      .height_px = 1024,
      .pixel_format = PixelFormat::rgb8,
      .reference_depth_top = 1000.0,
      .reference_depth_bottom = 1100.0,
      .dpi = 300,
      .source = BufferSourceReference{
          .uri = "mmap://img-doc#photo",
          .checksum = "photo-sha",
          .byte_offset = 4096,
      },
  });
  builder.add_custom_source(CustomLayerSource{
      .id = custom_id,
      .content_revision = DocumentRevision{3},
      .primitives =
          {
              CustomPolyline{
                  .points = {PhysicalPoint{Millimetres{1.0}, Millimetres{2.0}},
                             PhysicalPoint{Millimetres{3.0}, Millimetres{4.0}},
                             PhysicalPoint{Millimetres{5.0}, Millimetres{6.0}}},
                  .closed = true,
                  .color = RgbaColor{0x11, 0x22, 0x33, 0x44},
                  .width = Millimetres{0.5},
              },
              CustomTriangle{
                  .a = PhysicalPoint{Millimetres{1.0}, Millimetres{1.0}},
                  .b = PhysicalPoint{Millimetres{2.0}, Millimetres{1.0}},
                  .c = PhysicalPoint{Millimetres{1.5}, Millimetres{2.0}},
                  .fill_color = RgbaColor{0xFF, 0x00, 0x00, 0xFF},
              },
              CustomQuad{
                  .rect = PhysicalRect{Millimetres{0.0}, Millimetres{0.0},
                                       Millimetres{10.0}, Millimetres{5.0}},
                  .fill_color = RgbaColor{0x00, 0xFF, 0x00, 0xFF},
              },
              CustomSymbolOccurrence{
                  .center = PhysicalPoint{Millimetres{4.0}, Millimetres{4.0}},
                  .kind = SymbolKind::diamond,
                  .color = RgbaColor{0x00, 0x00, 0xFF, 0xFF},
                  .size = Millimetres{2.5},
              },
          },
      .clip = CustomClipPath{
          .points = {PhysicalPoint{Millimetres{0.0}, Millimetres{0.0}},
                     PhysicalPoint{Millimetres{10.0}, Millimetres{0.0}},
                     PhysicalPoint{Millimetres{10.0}, Millimetres{10.0}}},
      },
  });
  return builder.build();
}

// Minimal resolvers: bind by URI (the image/custom sources need no resolver —
// they carry only source identity, not buffers).
ManifestResolvers make_uri_resolvers() {
  return ManifestResolvers{
      .buffer = [](const BufferDescriptor &desc)
          -> Result<BufferView> {
        if (desc.source.uri == "mmap://img-doc#depth") {
          auto v = std::make_shared<const std::vector<double>>(
              std::initializer_list<double>{1000.0, 1001.0, 1002.0});
          return BufferView::from_vector(v, desc.source);
        }
        if (desc.source.uri == "mmap://img-doc#gr") {
          auto v = std::make_shared<const std::vector<double>>(
              std::initializer_list<double>{10.0, 20.0, 30.0});
          return BufferView::from_vector(v, desc.source);
        }
        return Error{
            .code = ErrorCode::unresolved_buffer,
            .severity = Severity::error,
            .entity_id = std::nullopt,
            .message = MessageKey::external_buffer_unresolved,
            .arguments = {},
        };
      },
      .null_bitmap = {},
      .image_tile = {},
  };
}

void image_and_custom_sources_round_trip() {
  const auto document_id = id("11111111-0000-4000-8000-000000000001");
  const auto axis_id = id("11111111-0000-4000-8000-000000000002");
  const auto curve_id = id("11111111-0000-4000-8000-000000000003");
  const auto image_id = id("11111111-0000-4000-8000-000000000004");
  const auto custom_id = id("11111111-0000-4000-8000-000000000005");
  const auto original = make_image_custom_document(
      document_id, axis_id, curve_id, image_id, custom_id);

  const auto encoded = ManifestCodec::write(original);
  require(encoded.has_value(), "image+custom document must encode");
  require(std::string{encoded.value().text()}.find("\"imageSources\"") !=
              std::string::npos,
          "encoded manifest must carry the imageSources key");
  require(std::string{encoded.value().text()}.find("\"customSources\"") !=
              std::string::npos,
          "encoded manifest must carry the customSources key");

  const auto resolvers = make_uri_resolvers();
  const auto restored = ManifestCodec::read(encoded.value().text(), resolvers);
  require(restored.has_value(), "image+custom document must round-trip");

  // ImageSource field-by-field.
  const auto &images = restored.value().image_sources();
  require(images.size() == 1, "exactly one image source must round-trip");
  require(images[0].id == image_id, "image id must round-trip");
  require(images[0].width_px == 2048 && images[0].height_px == 1024,
          "image dimensions must round-trip");
  require(images[0].pixel_format == PixelFormat::rgb8,
          "image pixel format must round-trip");
  require(images[0].reference_depth_top == 1000.0 &&
              images[0].reference_depth_bottom == 1100.0,
          "image depth range must round-trip");
  require(images[0].dpi == 300, "image dpi must round-trip");
  require(images[0].source.uri == "mmap://img-doc#photo" &&
              images[0].source.checksum == "photo-sha" &&
              images[0].source.byte_offset == 4096,
          "image source identity must round-trip");

  // CustomLayerSource field-by-field.
  const auto &customs = restored.value().custom_sources();
  require(customs.size() == 1, "exactly one custom source must round-trip");
  require(customs[0].id == custom_id, "custom id must round-trip");
  require(customs[0].content_revision == DocumentRevision{3},
          "content revision must round-trip");
  require(customs[0].primitives.size() == 4,
          "all 4 primitive kinds must round-trip");
  // Polyline.
  const auto *poly = std::get_if<CustomPolyline>(&customs[0].primitives[0]);
  require(poly != nullptr, "first primitive must be a polyline");
  require(poly->points.size() == 3, "polyline points must round-trip");
  require(poly->closed, "polyline closed flag must round-trip");
  require(poly->color == RgbaColor{0x11, 0x22, 0x33, 0x44},
          "polyline color must round-trip");
  require(poly->width == Millimetres{0.5}, "polyline width must round-trip");
  // Triangle.
  const auto *tri = std::get_if<CustomTriangle>(&customs[0].primitives[1]);
  require(tri != nullptr, "second primitive must be a triangle");
  require(tri->fill_color == RgbaColor{0xFF, 0x00, 0x00, 0xFF},
          "triangle fill color must round-trip");
  // All three triangle points, all coords.
  require(tri->a.left.value == 1.0 && tri->a.top.value == 1.0,
          "triangle a must round-trip");
  require(tri->b.left.value == 2.0 && tri->b.top.value == 1.0,
          "triangle b must round-trip");
  require(tri->c.left.value == 1.5 && tri->c.top.value == 2.0,
          "triangle c must round-trip");
  // Quad.
  const auto *quad = std::get_if<CustomQuad>(&customs[0].primitives[2]);
  require(quad != nullptr, "third primitive must be a quad");
  require(quad->rect.width.value == 10.0 && quad->rect.height.value == 5.0,
          "quad rect must round-trip");
  require(quad->rect.left.value == 0.0 && quad->rect.top.value == 0.0,
          "quad rect origin must round-trip");
  require(quad->fill_color == RgbaColor{0x00, 0xFF, 0x00, 0xFF},
          "quad fill color must round-trip");
  // Symbol.
  const auto *sym =
      std::get_if<CustomSymbolOccurrence>(&customs[0].primitives[3]);
  require(sym != nullptr, "fourth primitive must be a symbol");
  require(sym->kind == SymbolKind::diamond, "symbol kind must round-trip");
  require(sym->size == Millimetres{2.5}, "symbol size must round-trip");
  require(sym->center.left.value == 4.0 && sym->center.top.value == 4.0,
          "symbol center must round-trip");
  require(sym->color == RgbaColor{0x00, 0x00, 0xFF, 0xFF},
          "symbol color must round-trip");
  // Clip path.
  require(customs[0].clip.has_value() && customs[0].clip->points.size() == 3,
          "clip path must round-trip");
  require(customs[0].clip->points[2].top.value == 10.0,
          "clip path points must round-trip");

  // A document with NO image/custom sources must NOT emit those keys (so the
  // common case is unchanged), and still round-trips.
  const auto no_image_doc = [] {
    auto depths = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{1000.0});
    auto values = std::make_shared<const std::vector<double>>(
        std::initializer_list<double>{10.0});
    WellLogDocumentBuilder b(
        id("22222222-0000-4000-8000-000000000001"), DocumentRevision{1});
    b.add_sampling_axis(SamplingAxis{
        .id = id("22222222-0000-4000-8000-000000000002"),
        .coordinates = BufferView::from_vector(
            depths, BufferSourceReference{.uri = "mmap://no-img#depth",
                                          .checksum = {}, .byte_offset = 0}),
        .domain = DepthDomain::measured_depth,
        .unit = "m",
        .direction = AxisDirection::increasing,
    });
    b.add_curve(Curve{
        .id = id("22222222-0000-4000-8000-000000000003"),
        .mnemonic = "GR",
        .display_name = "GR",
        .unit = "API",
        .sampling_axis_id = id("22222222-0000-4000-8000-000000000002"),
        .values = BufferView::from_vector(
            values, BufferSourceReference{.uri = "mmap://no-img#gr",
                                          .checksum = {}, .byte_offset = 0}),
        .nulls = {},
    });
    return b.build();
  }();
  const auto no_image_encoded = ManifestCodec::write(no_image_doc);
  require(no_image_encoded.has_value(), "no-image doc must encode");
  require(std::string{no_image_encoded.value().text()}.find("imageSources") ==
              std::string::npos,
          "a document with no images must not emit imageSources");
}

// ADR 0042: over-limit image (dimension) is rejected; an empty custom source
// (zero primitives) is rejected. Both at the manifest read layer, with the
// existing invalid_image / invalid_custom_source codes.
void over_limit_image_and_empty_custom_sources_rejected() {
  const auto document_id = id("33333333-0000-4000-8000-000000000001");
  const auto axis_id = id("33333333-0000-4000-8000-000000000002");
  const auto curve_id = id("33333333-0000-4000-8000-000000000003");
  const auto image_id = id("33333333-0000-4000-8000-000000000004");
  const auto custom_id = id("33333333-0000-4000-8000-000000000005");
  const auto doc = make_image_custom_document(document_id, axis_id, curve_id,
                                              image_id, custom_id);
  const auto encoded = ManifestCodec::write(doc);
  require(encoded.has_value(), "fixture must encode");
  const auto resolvers = make_uri_resolvers();

  // Image: patch widthPx to exceed the 65536 dimension limit.
  auto oversized_image = std::string{encoded.value().text()};
  const auto wpos = oversized_image.find("\"widthPx\":2048");
  require(wpos != std::string::npos, "fixture must have widthPx");
  oversized_image.replace(wpos, std::string_view{"\"widthPx\":2048"}.size(),
                          "\"widthPx\":70000");
  const auto oversize_result =
      ManifestCodec::read(oversized_image, resolvers);
  require(!oversize_result.has_value(),
          "an over-dimension image must be rejected");
  require(oversize_result.error().code == ErrorCode::invalid_image,
          "over-dimension image must return invalid_image");

  // Image: patch both dims large but each under 65536, product over the pixel
  // limit (e.g. 50000 × 50000 = 2.5e9 > 512·1024·1024 ≈ 5.4e8).
  auto over_pixels = std::string{encoded.value().text()};
  over_pixels.replace(over_pixels.find("\"widthPx\":2048"),
                      std::string_view{"\"widthPx\":2048"}.size(),
                      "\"widthPx\":50000");
  over_pixels.replace(over_pixels.find("\"heightPx\":1024"),
                      std::string_view{"\"heightPx\":1024"}.size(),
                      "\"heightPx\":50000");
  const auto pixels_result = ManifestCodec::read(over_pixels, resolvers);
  require(!pixels_result.has_value(),
          "an over-pixel-count image must be rejected");
  require(pixels_result.error().code == ErrorCode::invalid_image,
          "over-pixel image must return invalid_image");

  // Image: dpi below the minimum (0).
  auto low_dpi = std::string{encoded.value().text()};
  low_dpi.replace(low_dpi.find("\"dpi\":300"),
                  std::string_view{"\"dpi\":300"}.size(), "\"dpi\":0");
  const auto dpi_result = ManifestCodec::read(low_dpi, resolvers);
  require(!dpi_result.has_value(),
          "a zero-dpi image must be rejected");
  require(dpi_result.error().code == ErrorCode::invalid_image,
          "invalid-dpi image must return invalid_image");

  // Empty custom source: a document with a CustomLayerSource carrying an empty
  // primitives vector serializes, but the reader rejects it (ADR 0042 non-empty
  // rule, custom_source_empty).
  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{1000.0});
  auto values = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{10.0});
  WellLogDocumentBuilder empty_builder(document_id, DocumentRevision{1});
  empty_builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(
          depths, BufferSourceReference{.uri = "mmap://empty#depth",
                                        .checksum = {}, .byte_offset = 0}),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  empty_builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(
          values, BufferSourceReference{.uri = "mmap://empty#gr",
                                        .checksum = {}, .byte_offset = 0}),
      .nulls = {},
  });
  empty_builder.add_custom_source(CustomLayerSource{
      .id = custom_id,
      .content_revision = DocumentRevision{1},
      .primitives = {}, // empty → must be rejected on read
      .clip = std::nullopt,
  });
  const auto empty_encoded = ManifestCodec::write(empty_builder.build());
  require(empty_encoded.has_value(), "empty-custom doc must encode");
  // Reuse resolvers but they look up the empty-doc URIs.
  ManifestResolvers empty_resolvers = make_uri_resolvers();
  empty_resolvers.buffer = [](const BufferDescriptor &desc)
      -> Result<BufferView> {
    if (desc.source.uri == "mmap://empty#depth") {
      auto v = std::make_shared<const std::vector<double>>(
          std::initializer_list<double>{1000.0});
      return BufferView::from_vector(v, desc.source);
    }
    if (desc.source.uri == "mmap://empty#gr") {
      auto v = std::make_shared<const std::vector<double>>(
          std::initializer_list<double>{10.0});
      return BufferView::from_vector(v, desc.source);
    }
    return Error{
        .code = ErrorCode::unresolved_buffer,
        .severity = Severity::error,
        .entity_id = std::nullopt,
        .message = MessageKey::external_buffer_unresolved,
        .arguments = {},
    };
  };
  const auto empty_result =
      ManifestCodec::read(empty_encoded.value().text(), empty_resolvers);
  require(!empty_result.has_value(),
          "an empty custom source must be rejected");
  require(empty_result.error().code == ErrorCode::invalid_custom_source,
          "empty custom source must return invalid_custom_source");
}

// The v2 reader accepts BOTH v1 and v2 manifests (forward-compat: v2 only
// added two optional keys). Forcing a v2 manifest's version DOWN to 1 must
// still round-trip (the imageSources/customSources are simply re-read). A
// version the reader does not know (:3) is rejected with
// manifest_schema_unsupported (the "old reader rejects new shape" intent).
void version_gate_accepts_v1_and_rejects_unknown() {
  const auto document_id = id("44444444-0000-4000-8000-000000000001");
  const auto axis_id = id("44444444-0000-4000-8000-000000000002");
  const auto curve_id = id("44444444-0000-4000-8000-000000000003");
  const auto image_id = id("44444444-0000-4000-8000-000000000004");
  const auto custom_id = id("44444444-0000-4000-8000-000000000005");
  const auto doc = make_image_custom_document(document_id, axis_id, curve_id,
                                              image_id, custom_id);
  const auto encoded = ManifestCodec::write(doc);
  require(encoded.has_value(), "fixture must encode");
  const auto resolvers = make_uri_resolvers();

  // v2 manifest (current) round-trips.
  const auto v2_result = ManifestCodec::read(encoded.value().text(), resolvers);
  require(v2_result.has_value(), "the current schema version must round-trip");

  // Force version DOWN to 1: a v2 reader still reads it (the imageSources/
  // customSources round-trip because the keys are present in the JSON body
  // regardless of the version number; the v1 label is tolerated).
  auto as_v1 = std::string{encoded.value().text()};
  as_v1.replace(as_v1.find("\"schemaVersion\":2"),
                std::string_view{"\"schemaVersion\":2"}.size(),
                "\"schemaVersion\":1");
  const auto v1_result = ManifestCodec::read(as_v1, resolvers);
  require(v1_result.has_value(),
          "a v2 reader must accept a manifest labelled v1 (forward-compat)");
  require(v1_result.value().image_sources().size() == 1 &&
              v1_result.value().custom_sources().size() == 1,
          "the v1-labelled manifest's image/custom sources must still round-trip");

  // An unknown future version (:3) is rejected with manifest_schema_unsupported.
  auto as_v3 = std::string{encoded.value().text()};
  as_v3.replace(as_v3.find("\"schemaVersion\":2"),
                std::string_view{"\"schemaVersion\":2"}.size(),
                "\"schemaVersion\":3");
  const auto v3_result = ManifestCodec::read(as_v3, resolvers);
  require(!v3_result.has_value(),
          "an unknown schema version must be rejected");
  require(v3_result.error().message == MessageKey::manifest_schema_unsupported,
          "unknown version must surface manifest_schema_unsupported");
}

void escaped_surrogate_pair_decodes_to_astral_character() {
  // #751: RFC 8259 surrogate pairs (e.g. "\ud83d\ude00" = U+1F600) are
  // valid JSON; lone surrogates stay rejected.
  const auto document_id = id("01234567-89ab-4cde-8fab-0123456789ab");
  const auto axis_id = id("12345678-9abc-4def-8abc-123456789abc");
  const auto curve_id = id("23456789-abcd-4efa-8bcd-23456789abcd");

  auto depths = std::make_shared<const std::vector<double>>(
      std::initializer_list<double>{900.25, 900.5, 900.75});
  auto values = std::make_shared<const std::vector<float>>(
      std::initializer_list<float>{12.5F, 25.0F, 37.5F});
  WellLogDocumentBuilder builder(document_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = BufferView::from_vector(
          depths, BufferSourceReference{.uri = "mmap://well-a.bin#depth",
                                        .checksum = "sha256:depth",
                                        .byte_offset = 64}),
      .domain = DepthDomain::measured_depth,
      .unit = "m",
      .direction = AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = "GR",
      .display_name = "GR",
      .unit = "API",
      .sampling_axis_id = axis_id,
      .values = BufferView::from_vector(
          values, BufferSourceReference{.uri = "mmap://well-a.bin#gr",
                                        .checksum = "sha256:gr",
                                        .byte_offset = 4096}),
      .nulls = {},
  });
  const auto encoded = ManifestCodec::write(builder.build());
  require(encoded.has_value(), "fixture manifest must serialize");

  ManifestResolvers resolvers{
      .buffer = [&](const BufferDescriptor &descriptor) -> Result<BufferView> {
        if (descriptor.source.uri == "mmap://well-a.bin#depth") {
          return BufferView::from_vector(
              depths, BufferSourceReference{.uri = "mmap://well-a.bin#depth",
                                            .checksum = "sha256:depth",
                                            .byte_offset = 64});
        }
        if (descriptor.source.uri == "mmap://well-a.bin#gr") {
          return BufferView::from_vector(
              values, BufferSourceReference{.uri = "mmap://well-a.bin#gr",
                                            .checksum = "sha256:gr",
                                            .byte_offset = 4096});
        }
        return Error{
            .code = ErrorCode::unresolved_buffer,
            .entity_id = std::nullopt,
            .message = MessageKey::external_buffer_unresolved,
            .arguments = {},
        };
      },
      .null_bitmap = {},
      .image_tile = {},
  };

  auto with_pair = std::string{encoded.value().text()};
  const auto name_key = std::string{"\"displayName\":\"GR\""};
  require(with_pair.find(name_key) != std::string::npos,
          "fixture carries displayName");
  with_pair.replace(with_pair.find(name_key), name_key.size(),
                    "\"displayName\":\"\\ud83d\\ude00\"");
  const auto grin = ManifestCodec::read(with_pair, resolvers);
  require(grin.has_value(),
          "escaped surrogate pair must parse as valid JSON");
  require(grin.value().curves().front().display_name == "\xF0\x9F\x98\x80",
          "\\ud83d\\ude00 must decode to U+1F600");

  auto lone_high = std::string{encoded.value().text()};
  lone_high.replace(lone_high.find(name_key), name_key.size(),
                    "\"displayName\":\"\\ud83d\"");
  const auto lone = ManifestCodec::read(lone_high, resolvers);
  require(!lone.has_value(), "lone high surrogate must stay rejected");

  auto lone_low = std::string{encoded.value().text()};
  lone_low.replace(lone_low.find(name_key), name_key.size(),
                   "\"displayName\":\"\\ude00\"");
  const auto low = ManifestCodec::read(lone_low, resolvers);
  require(!low.has_value(), "lone low surrogate must stay rejected");
}

} // namespace

int main() {
  manifest_round_trip_rebinds_external_buffers();
  manifest_writer_rejects_documents_outside_schema();
  image_and_custom_sources_round_trip();
  over_limit_image_and_empty_custom_sources_rejected();
  version_gate_accepts_v1_and_rejects_unknown();
  escaped_surrogate_pair_decodes_to_astral_character();
  std::cout << "PASS: manifest round trip\n";
  return EXIT_SUCCESS;
}
