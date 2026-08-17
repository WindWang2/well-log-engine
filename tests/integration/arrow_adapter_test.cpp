#include <welllog/arrow/adapter.hpp>
#include <welllog/core/document.hpp>
#include <welllog/scene/curve_lod.hpp>
#include <welllog/scene/scene.hpp>
#include <welllog/session/session.hpp>

#if defined(WELLLOG_ARROW_HAS_IPC)
#include <arrow/api.h>
#include <arrow/c/bridge.h>
#include <arrow/io/file.h>
#include <arrow/ipc/api.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
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

void require_near(double actual, double expected, std::string_view message) {
  if (!(std::isfinite(actual) && std::isfinite(expected)) ||
      std::abs(actual - expected) > 1.0e-9) {
    std::cerr << "FAIL: " << message << " actual=" << actual
              << " expected=" << expected << '\n';
    std::_Exit(EXIT_FAILURE); // #241: see fail() — no CRT/DLL teardown
  }
}

EntityId id(std::string_view text) {
  const auto parsed = EntityId::parse(text);
  require(parsed.has_value(), "UUID");
  return *parsed;
}

// Heap storage freed by the C Data release callback.
struct SyntheticStorage {
  std::vector<double> values;
  std::vector<std::uint8_t> validity; // Arrow polarity: 1 = valid
  std::vector<const void *> buffer_ptrs;
};

struct SyntheticArray {
  WellLogArrowSchema schema{};
  WellLogArrowArray array{};
  SyntheticStorage *storage{nullptr};

  static void release_schema(WellLogArrowSchema *schema) {
    if (schema == nullptr || schema->release == nullptr) {
      return;
    }
    schema->release = nullptr;
    schema->private_data = nullptr;
  }

  static void release_array(WellLogArrowArray *array) {
    if (array == nullptr || array->release == nullptr) {
      return;
    }
    delete static_cast<SyntheticStorage *>(array->private_data);
    array->private_data = nullptr;
    array->buffers = nullptr;
    array->release = nullptr;
  }

  void init(std::vector<double> data, std::vector<bool> is_valid,
            std::int64_t offset = 0) {
    auto *heap = new SyntheticStorage{};
    heap->values = std::move(data);
    const auto logical =
        static_cast<std::int64_t>(heap->values.size()) - offset;
    require(logical >= 0, "offset within values");
    heap->validity.assign((heap->values.size() + 7) / 8, 0xff);
    std::int64_t null_count = 0;
    for (std::size_t i = 0; i < is_valid.size() && i < heap->values.size();
         ++i) {
      if (!is_valid[i]) {
        heap->validity[i / 8] = static_cast<std::uint8_t>(
            heap->validity[i / 8] &
            static_cast<std::uint8_t>(~(std::uint8_t{1} << (i % 8))));
        ++null_count;
      }
    }
    heap->buffer_ptrs = {heap->validity.data(), heap->values.data()};
    storage = heap;
    schema = WellLogArrowSchema{
        .format = "g",
        .name = "values",
        .metadata = nullptr,
        .flags = WELLLOG_ARROW_FLAG_NULLABLE,
        .n_children = 0,
        .children = nullptr,
        .dictionary = nullptr,
        .release = &SyntheticArray::release_schema,
        .private_data = nullptr,
    };
    array = WellLogArrowArray{
        .length = logical,
        .null_count = null_count,
        .offset = offset,
        .n_buffers = 2,
        .n_children = 0,
        .buffers = heap->buffer_ptrs.data(),
        .children = nullptr,
        .dictionary = nullptr,
        .release = &SyntheticArray::release_array,
        .private_data = heap,
    };
  }
};

void zero_copy_float64_with_nulls() {
  SyntheticArray synth;
  synth.init({10.0, 20.0, 30.0, 40.0}, {true, false, true, true});
  auto imported = import_arrow_array(synth.schema, synth.array);
  require(imported.has_value(), "import float64");
  const auto &imp = imported.value();
  require(imp.values_access == BufferAccessMode::zero_copy, "values zero copy");
  require(std::string_view{buffer_access_mode_name(imp.values_access)} ==
              "zero_copy",
          "name zero_copy");
  require(imp.nulls_access == BufferAccessMode::converted_copy,
          "nulls inverted copy");
  require(imp.length == 4, "length 4");
  require(imp.values.scalar_type() == ScalarType::float64, "type f64");
  require(imp.values.has_owner(), "owner present");
  require(imp.values.data() == reinterpret_cast<const std::byte *>(
                                    synth.storage->values.data()),
          "pointer is original storage");
  require_near(*imp.values.value_as_double(0), 10.0, "v0");
  require(imp.nulls.is_null(1), "arrow null → core null");
  require(!imp.nulls.is_null(0), "valid sample");
  require(!imp.nulls.is_null(2), "valid sample 2");
  require(synth.array.release == nullptr, "release stolen");
}

void offset_slice_is_zero_copy() {
  SyntheticArray synth;
  synth.init({0.0, 1.0, 2.0, 3.0, 4.0}, {true, true, true, true, true},
             /*offset=*/2);
  auto imported = import_arrow_array(synth.schema, synth.array);
  require(imported.has_value(), "import offset slice");
  const auto &imp = imported.value();
  require(imp.length == 3, "logical length");
  require(imp.values_access == BufferAccessMode::zero_copy,
          "offset still zero copy");
  require_near(*imp.values.value_as_double(0), 2.0, "first logical");
  require_near(*imp.values.value_as_double(2), 4.0, "last logical");
  require(imp.values.data() ==
              reinterpret_cast<const std::byte *>(synth.storage->values.data() +
                                                  2),
          "data at offset");
}

void half_float_requires_explicit_convert() {
  std::vector<std::uint16_t> halves = {
      0x3c00, // 1.0
      0x4000, // 2.0
      0x4200, // 3.0
  };
  std::vector<const void *> bufs{nullptr, halves.data()};
  WellLogArrowSchema schema{.format = "e",
                            .name = "h",
                            .metadata = nullptr,
                            .flags = 0,
                            .n_children = 0,
                            .children = nullptr,
                            .dictionary = nullptr,
                            .release = nullptr,
                            .private_data = nullptr};
  WellLogArrowArray array{.length = 3,
                          .null_count = 0,
                          .offset = 0,
                          .n_buffers = 2,
                          .n_children = 0,
                          .buffers = bufs.data(),
                          .children = nullptr,
                          .dictionary = nullptr,
                          .release = nullptr,
                          .private_data = nullptr};
  require(!import_arrow_array(schema, array).has_value(),
          "half refused without policy");
  auto converted = import_arrow_array(
      schema, array, ArrowImportOptions{.allow_converted_copy = true});
  require(converted.has_value(), "half with convert");
  const auto &imp = converted.value();
  require(imp.values_access == BufferAccessMode::converted_copy,
          "reported converted");
  require(imp.scalar_type == ScalarType::float64, "as float64");
  require_near(*imp.values.value_as_double(0), 1.0, "half 1");
  require_near(*imp.values.value_as_double(1), 2.0, "half 2");
  require_near(*imp.values.value_as_double(2), 3.0, "half 3");
}

void mmap_column_zero_copy_and_lod() {
  const auto dir =
      std::filesystem::temp_directory_path() / "welllog_arrow_mmap_test";
  std::filesystem::create_directories(dir);
  const auto path = dir / "depths.f64";
  std::vector<double> depths(10'000);
  for (std::size_t i = 0; i < depths.size(); ++i) {
    depths[i] = 1000.0 + static_cast<double>(i) * 0.1;
  }
  {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(depths.data()),
              static_cast<std::streamsize>(depths.size() * sizeof(double)));
    require(static_cast<bool>(out), "write mmap file");
  }
  auto view = import_mmap_scalar_column(
      path, ScalarType::float64, depths.size(),
      BufferSourceReference{.uri = path.string(), .checksum = {}, .byte_offset = 0});
  require(view.has_value(), "mmap import");
  const auto &buf = view.value();
  require(buf.access_mode() == BufferAccessMode::zero_copy, "mmap zero copy");
  require(buf.has_owner(), "mmap owner");
  require(buf.length() == depths.size(), "mmap length");
  require_near(*buf.value_as_double(0), 1000.0, "mmap first");
  require_near(*buf.value_as_double(depths.size() - 1),
               1000.0 + 0.1 * static_cast<double>(depths.size() - 1),
               "mmap last");

  auto depth_buf = std::move(view.value());
  auto values =
      std::make_shared<const std::vector<double>>(depths.size(), 50.0);
  const auto doc_id = id("16300000-0000-4000-8000-000000000001");
  const auto axis_id = id("16300000-0000-4000-8000-000000000002");
  const auto curve_id = id("16300000-0000-4000-8000-000000000003");
  WellLogDocumentBuilder builder(doc_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = depth_buf,
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
      .values = BufferView::from_vector(values),
      .nulls = {},
  });
  auto document = builder.build();
  require(!document.id().is_nil(), "document builds with mmap axis");

  const auto pyramid = CurveLodPyramid::build(
      document.sampling_axes().front(), document.curves().front(),
      CurveLodBuildOptions{
          .algorithm = CurveLodAlgorithm::hierarchical,
          .base_bucket_samples = 64,
          .maximum_derived_bytes = 1 << 20,
      });
  require(pyramid.has_value(), "LOD builds on mmap axis");
  const auto selection = pyramid.value().query(CurveLodQuery{
      .viewport_top = 1000.0,
      .viewport_bottom = 1100.0,
      .pixel_height = 200,
      .prefetch_viewports = 0.0,
  });
  require(selection.has_value(), "LOD query");
  require(!selection.value().points().empty(), "LOD points");

  document = WellLogDocument{};
  depth_buf = BufferView{};
  std::filesystem::remove_all(dir);
}

void arrow_array_into_session_and_scene() {
  SyntheticArray synth;
  synth.init({1000.0, 1001.0, 1002.0}, {true, true, true});
  auto depth_imp = import_arrow_array(synth.schema, synth.array);
  require(depth_imp.has_value(), "depth import");

  SyntheticArray values_synth;
  values_synth.init({10.0, std::numeric_limits<double>::quiet_NaN(), 30.0},
                    {true, true, true});
  auto value_imp =
      import_arrow_array(values_synth.schema, values_synth.array);
  require(value_imp.has_value(), "values import");
  require(value_imp.value().values_access == BufferAccessMode::zero_copy,
          "curve zero copy");

  const auto doc_id = id("16300000-0000-4000-8000-000000000011");
  const auto axis_id = id("16300000-0000-4000-8000-000000000012");
  const auto curve_id = id("16300000-0000-4000-8000-000000000013");
  const auto track_id = id("16300000-0000-4000-8000-000000000014");
  const auto scale_id = id("16300000-0000-4000-8000-000000000015");
  const auto layer_id = id("16300000-0000-4000-8000-000000000016");

  WellLogDocumentBuilder builder(doc_id, DocumentRevision{1});
  builder.add_sampling_axis(SamplingAxis{
      .id = axis_id,
      .coordinates = depth_imp.value().values,
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
      .values = value_imp.value().values,
      .nulls = value_imp.value().nulls,
  });
  auto document = builder.build();
  require(!document.id().is_nil(), "doc");

  WellLogSession session;
  require(session.execute(SetDocumentCommand{document}).has_value(), "set doc");
  ScenePresentationBuilder pres(
      doc_id,
      ReferenceDepthRange{.domain = DepthDomain::measured_depth,
                          .unit = "m",
                          .top = 1000.0,
                          .bottom = 1002.0},
      Millimetres{80.0}, "font-fixture");
  pres.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{25.0}, .z_order = 1});
  pres.add_scale(TrackScaleSpec{.id = scale_id,
                                .track_id = track_id,
                                .mode = ScaleMode::linear,
                                .minimum = 0.0,
                                .maximum = 100.0,
                                .unit = "API"});
  pres.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color = RgbaColor{1, 2, 3, 255},
      .line_width = Millimetres{0.25},
      .z_order = 1,
      .visible = true,
  });
  require(session.execute(SetPresentationCommand{pres.build()}).has_value(),
          "set presentation");
  require(session
              .execute(SetViewportMetricsCommand{
                  .document_id = doc_id,
                  .viewport = DepthViewport{.top = 1000.0, .bottom = 1002.0},
                  .pixel_height = 100,
              })
              .has_value(),
          "viewport");
  auto scene = session.prepared_scene(doc_id);
  require(scene != nullptr, "prepared scene from arrow buffers");
  require(!scene->curve_points().empty(), "geometry produced");
  require(session.execute(SetDocumentCommand{document}).has_value(),
          "replace document");
}

void ipc_availability_matches_build() {
#if defined(WELLLOG_ARROW_HAS_IPC)
  require(arrow_ipc_available(), "IPC advertised when Arrow C++ is linked");
#else
  require(!arrow_ipc_available(),
          "IPC must not be advertised without Arrow C++");
  const auto missing = import_arrow_ipc_file_column(
      "/nonexistent-welllog-arrow-ipc.arrow", 0);
  require(!missing.has_value(), "disabled IPC import must fail closed");
  require(missing.error().code == ErrorCode::unresolved_buffer,
          "disabled IPC import must use unresolved_buffer");
  std::cerr << "SKIP: Arrow IPC not built (C Data + mmap still tested)\n";
#endif
}

#if defined(WELLLOG_ARROW_HAS_IPC)
void ipc_file_column_zero_copy() {
  require(arrow_ipc_available(), "IPC built");
  const auto dir =
      std::filesystem::temp_directory_path() / "welllog_arrow_ipc_test";
  std::filesystem::create_directories(dir);
  const auto path = dir / "column.arrow";

  {
    arrow::DoubleBuilder depths;
    arrow::FloatBuilder values;
    require(depths.AppendValues({1000.0, 1001.0, 1002.0}).ok(), "depths");
    require(values.Append(1.5f).ok() && values.AppendNull().ok() &&
                values.Append(3.5f).ok(),
            "values");
    std::shared_ptr<arrow::Array> d_arr;
    std::shared_ptr<arrow::Array> v_arr;
    require(depths.Finish(&d_arr).ok() && values.Finish(&v_arr).ok(), "finish");
    auto schema = arrow::schema(
        {arrow::field("DEPT", arrow::float64()),
         arrow::field("GR", arrow::float32(), /*nullable=*/true)});
    auto batch = arrow::RecordBatch::Make(schema, 3, {d_arr, v_arr});
    auto outfile = arrow::io::FileOutputStream::Open(path.string());
    require(outfile.ok(), "open out");
    auto writer = arrow::ipc::MakeFileWriter(*outfile, schema);
    require(writer.ok(), "writer");
    require((*writer)->WriteRecordBatch(*batch).ok(), "write batch");
    require((*writer)->Close().ok(), "close writer");
    require((*outfile)->Close().ok(), "close file");
  }

  auto dept = import_arrow_ipc_file_column(path, 0);
  require(dept.has_value(), "ipc depth column");
  const auto &d = dept.value();
  require(d.values_access == BufferAccessMode::zero_copy,
          "ipc depth zero copy");
  require(d.length == 3, "ipc length");
  require_near(*d.values.value_as_double(0), 1000.0, "ipc d0");

  auto gr = import_arrow_ipc_file_column(path, 1);
  require(gr.has_value(), "ipc gr column");
  const auto &g = gr.value();
  require(g.values_access == BufferAccessMode::zero_copy, "ipc gr zero copy");
  require(g.scalar_type == ScalarType::float32, "float32");
  require(g.nulls.is_null(1), "ipc null preserved");
  require_near(*g.values.value_as_double(0), 1.5, "gr0");
  require_near(*g.values.value_as_double(2), 3.5, "gr2");

  std::filesystem::remove_all(dir);
}
#endif

void large_mmap_budget_smoke() {
  constexpr std::uint64_t n = 1'000'000;
  const auto dir =
      std::filesystem::temp_directory_path() / "welllog_arrow_budget";
  std::filesystem::create_directories(dir);
  const auto path = dir / "big.f64";
  {
    std::ofstream out(path, std::ios::binary);
    double v = 0.0;
    for (std::uint64_t i = 0; i < n; ++i) {
      v = static_cast<double>(i);
      out.write(reinterpret_cast<const char *>(&v), sizeof(v));
    }
    require(static_cast<bool>(out), "write big");
  }
  const auto t0 = std::chrono::steady_clock::now();
  auto view = import_mmap_scalar_column(path, ScalarType::float64, n);
  const auto t1 = std::chrono::steady_clock::now();
  require(view.has_value(), "big mmap");
  const auto &buf = view.value();
  require(buf.access_mode() == BufferAccessMode::zero_copy, "big zero copy");
  require_near(*buf.value_as_double(n - 1), static_cast<double>(n - 1),
               "last sample");
  const auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  require(ms < 2000.0, "mmap first-touch import within budget");
  std::cerr << "INFO: mmap 1e6 float64 import " << ms << " ms\n";
  view = BufferView{};
  std::filesystem::remove_all(dir);
}

void nested_types_rejected() {
  WellLogArrowSchema child{.format = "g",
                           .name = "x",
                           .metadata = nullptr,
                           .flags = 0,
                           .n_children = 0,
                           .children = nullptr,
                           .dictionary = nullptr,
                           .release = nullptr,
                           .private_data = nullptr};
  WellLogArrowSchema *children[] = {&child};
  WellLogArrowSchema schema{.format = "+s",
                            .name = "struct",
                            .metadata = nullptr,
                            .flags = 0,
                            .n_children = 1,
                            .children = children,
                            .dictionary = nullptr,
                            .release = nullptr,
                            .private_data = nullptr};
  WellLogArrowArray array{};
  array.length = 0;
  array.n_children = 1;
  require(!import_arrow_array(schema, array).has_value(), "nested rejected");
}

} // namespace

int main() {
  zero_copy_float64_with_nulls();
  offset_slice_is_zero_copy();
  half_float_requires_explicit_convert();
  mmap_column_zero_copy_and_lod();
  arrow_array_into_session_and_scene();
  ipc_availability_matches_build();
#if defined(WELLLOG_ARROW_HAS_IPC)
  ipc_file_column_zero_copy();
#endif
  large_mmap_budget_smoke();
  nested_types_rejected();
  return EXIT_SUCCESS;
}
