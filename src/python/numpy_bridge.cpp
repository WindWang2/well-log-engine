#ifdef Py_LIMITED_API
#undef Py_LIMITED_API
#endif
#define Py_LIMITED_API 0x030b0000
#include <Python.h>

#include "numpy_bridge.hpp"

#include <welllog/core/document.hpp>
#include <welllog/export/cgm.hpp>
#include <welllog/export/pdf_scene.hpp>
#include <welllog/export/svg.hpp>
#include <welllog/qtwidgets/well_log_view.hpp>
#include <welllog/scene/axis_ticks.hpp>
#include <welllog/scene/inspect.hpp>
#include <welllog/scene/presentation_index.hpp>
#include <welllog/session/track_commands.hpp>

#include <QByteArray>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace welllog::python {
namespace {

class PythonBufferOwner final {
public:
  PythonBufferOwner() = default;
  PythonBufferOwner(const PythonBufferOwner &) = delete;
  PythonBufferOwner &operator=(const PythonBufferOwner &) = delete;

  ~PythonBufferOwner() {
    if (view_.obj == nullptr || !Py_IsInitialized()) {
      return;
    }
    const auto state = PyGILState_Ensure();
    PyBuffer_Release(&view_);
    PyGILState_Release(state);
  }

  [[nodiscard]] bool acquire(PyObject *object) noexcept {
    return PyObject_GetBuffer(object, &view_, PyBUF_FORMAT | PyBUF_STRIDES) ==
           0;
  }

  [[nodiscard]] const Py_buffer &view() const noexcept { return view_; }

private:
  Py_buffer view_{};
};

struct AdaptedBuffer {
  BufferView buffer;
  std::string dtype;
  std::uint64_t address{};
};

void set_welllog_error(const char *type_name, const char *code,
                       const char *message) {
  auto *module = PyImport_ImportModule("welllog.errors");
  if (module == nullptr) {
    PyErr_Clear();
    PyErr_Format(PyExc_RuntimeError, "%s [%s]: %s", type_name, code, message);
    return;
  }
  auto *type = PyObject_GetAttrString(module, type_name);
  Py_DECREF(module);
  if (type == nullptr) {
    PyErr_Clear();
    PyErr_Format(PyExc_RuntimeError, "%s [%s]: %s", type_name, code, message);
    return;
  }
  auto *instance = PyObject_CallFunction(type, "ss", message, code);
  if (instance != nullptr) {
    PyErr_SetObject(type, instance);
    Py_DECREF(instance);
  } else {
    PyErr_Clear();
    PyErr_Format(PyExc_RuntimeError, "%s [%s]: %s", type_name, code, message);
  }
  Py_DECREF(type);
}

[[nodiscard]] const char *error_code_name(ErrorCode code) noexcept {
  switch (code) {
  case ErrorCode::missing_owner:
    return "missing_owner";
  case ErrorCode::invalid_buffer:
    return "invalid_buffer";
  case ErrorCode::arithmetic_overflow:
    return "arithmetic_overflow";
  case ErrorCode::invalid_sampling_axis:
    return "invalid_sampling_axis";
  case ErrorCode::length_mismatch:
    return "length_mismatch";
  case ErrorCode::duplicate_entity_id:
    return "duplicate_entity_id";
  case ErrorCode::missing_sampling_axis:
    return "missing_sampling_axis";
  case ErrorCode::invalid_document:
    return "invalid_document";
  case ErrorCode::invalid_presentation:
    return "invalid_presentation";
  case ErrorCode::invalid_viewport:
    return "invalid_viewport";
  case ErrorCode::document_not_found:
    return "document_not_found";
  case ErrorCode::invalid_manifest:
    return "invalid_manifest";
  case ErrorCode::unresolved_buffer:
    return "unresolved_buffer";
  case ErrorCode::operation_cancelled:
    return "operation_cancelled";
  case ErrorCode::resource_exhausted:
    return "resource_exhausted";
  case ErrorCode::internal_error:
    return "internal_error";
  case ErrorCode::patch_conflict:
    return "patch_conflict";
  case ErrorCode::history_empty:
    return "history_empty";
  }
  return "internal_error";
}

void set_result_error(const Error &error, const char *operation) {
  const auto *type_name = error.code == ErrorCode::resource_exhausted ||
                                  error.code == ErrorCode::internal_error
                              ? "WellLogError"
                              : "WellLogValidationError";
  const auto *code = error_code_name(error.code);
  const auto message = std::string{operation} + " failed with code " + code;
  set_welllog_error(type_name, code, message.c_str());
}

[[nodiscard]] std::optional<ScalarType>
scalar_type_for_buffer(const Py_buffer &view) noexcept {
  if (view.format == nullptr) {
    return std::nullopt;
  }
  auto format = std::string_view{view.format};
  if (format.size() == 2 && (format.front() == '@' || format.front() == '=' ||
                             format.front() == '<' || format.front() == '>')) {
    if ((format.front() == '<' || format.front() == '>') &&
        (format.front() == '<') !=
#if Q_BYTE_ORDER == Q_LITTLE_ENDIAN
            true
#else
            false
#endif
    ) {
      return std::nullopt;
    }
    format.remove_prefix(1);
  }
  if (format == "f" && view.itemsize == 4) {
    return ScalarType::float32;
  }
  if (format == "d" && view.itemsize == 8) {
    return ScalarType::float64;
  }
  if (format == "h" && view.itemsize == 2) {
    return ScalarType::int16;
  }
  if (format == "i" && view.itemsize == 4) {
    return ScalarType::int32;
  }
  if (format == "q" && view.itemsize == 8) {
    return ScalarType::int64;
  }
  // LP64 platforms (Linux/macOS) report C long as 'l'/'L': numpy's int64/
  // uint64 expose buffer format "l"/"L" with itemsize 8 there, so accept them
  // alongside the explicit 'q'/'Q'. The itemsize check keeps 4-byte longs
  // (LLP64 Windows) rejected (#36).
  if (format == "l" && view.itemsize == 8) {
    return ScalarType::int64;
  }
  if (format == "L" && view.itemsize == 8) {
    return ScalarType::uint64;
  }
  if (format == "B" && view.itemsize == 1) {
    return ScalarType::uint8;
  }
  if (format == "H" && view.itemsize == 2) {
    return ScalarType::uint16;
  }
  if (format == "I" && view.itemsize == 4) {
    return ScalarType::uint32;
  }
  if (format == "Q" && view.itemsize == 8) {
    return ScalarType::uint64;
  }
  return std::nullopt;
}

[[nodiscard]] std::optional<AdaptedBuffer> adapt_buffer(PyObject *object,
                                                        const char *role) {
  auto owner = std::make_shared<PythonBufferOwner>();
  if (!owner->acquire(object)) {
    PyErr_Clear();
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      "object does not expose a compatible buffer");
    return std::nullopt;
  }
  const auto &view = owner->view();
  if (view.ndim != 1 || view.shape == nullptr || view.shape[0] <= 0 ||
      view.buf == nullptr || view.itemsize <= 0) {
    const auto message =
        std::string{role} + " must be a non-empty one-dimensional buffer";
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      message.c_str());
    return std::nullopt;
  }
  if (view.readonly == 0) {
    const auto message =
        std::string{role} + " must be marked read-only for zero-copy access";
    set_welllog_error("WellLogValidationError", "writable_buffer",
                      message.c_str());
    return std::nullopt;
  }
  const auto scalar_type = scalar_type_for_buffer(view);
  if (!scalar_type.has_value()) {
    const auto message =
        std::string{role} + " uses an unsupported or non-native scalar dtype";
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      message.c_str());
    return std::nullopt;
  }
  const auto stride = view.strides == nullptr ? view.itemsize : view.strides[0];
  if (stride < view.itemsize || stride <= 0) {
    const auto message =
        std::string{role} + " must use a positive stride of at least one item";
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      message.c_str());
    return std::nullopt;
  }
  const auto length = static_cast<std::uint64_t>(view.shape[0]);
  const auto stride_bytes = static_cast<std::uint64_t>(stride);
  const auto item_size = static_cast<std::uint64_t>(view.itemsize);
  if (length - 1 >
      (std::numeric_limits<std::uint64_t>::max() - item_size) / stride_bytes) {
    const auto message = std::string{role} + " buffer extent is too large";
    set_welllog_error("WellLogValidationError", "arithmetic_overflow",
                      message.c_str());
    return std::nullopt;
  }
  const auto capacity = (length - 1) * stride_bytes + item_size;
  const auto address =
      static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(view.buf));
  const auto dtype = std::string{scalar_type_name(*scalar_type)};
  return AdaptedBuffer{
      .buffer = BufferView::from_raw(view.buf, length, stride_bytes,
                                     *scalar_type, capacity, SharedOwner{owner},
                                     {}, BufferAccessMode::zero_copy),
      .dtype = dtype,
      .address = address,
  };
}

[[nodiscard]] std::optional<EntityId> parse_id(const QString &text,
                                               const char *role) {
  const auto encoded = text.toUtf8();
  const auto result = EntityId::parse(std::string_view{
      encoded.constData(), static_cast<std::size_t>(encoded.size())});
  if (!result.has_value() || result->is_nil()) {
    const auto message = std::string{role} + " must be a non-nil UUID";
    set_welllog_error("WellLogValidationError", "invalid_document",
                      message.c_str());
    return std::nullopt;
  }
  return result;
}

[[nodiscard]] EntityId
derive_presentation_id(EntityId document_id, std::string_view role,
                       std::initializer_list<EntityId> forbidden) {
  const auto namespace_uuid =
      QUuid{QString::fromStdString(document_id.to_string())};
  const auto base_name =
      QByteArray{role.data(), static_cast<qsizetype>(role.size())};
  for (auto suffix = 0; suffix <= static_cast<int>(forbidden.size());
       ++suffix) {
    auto name = base_name;
    if (suffix != 0) {
      name.append('/');
      name.append(QByteArray::number(suffix));
    }
    const auto derived = QUuid::createUuidV5(namespace_uuid, name);
    const auto encoded = derived.toString(QUuid::WithoutBraces).toStdString();
    const auto candidate = EntityId::parse(encoded).value();
    if (std::find(forbidden.begin(), forbidden.end(), candidate) ==
        forbidden.end()) {
      return candidate;
    }
  }
  throw std::runtime_error{"could not derive a unique presentation entity ID"};
}

[[nodiscard]] Result<CommandReceipt> prepare_default_curve_scene(
    WellLogView &view, EntityId document_id, EntityId axis_id,
    EntityId curve_id, const BufferView &depth, const BufferView &values,
    const std::string &depth_unit, const std::string &value_unit) {
  auto top = depth.value_as_double(0).value();
  auto bottom = depth.value_as_double(depth.length() - 1).value();
  if (top > bottom) {
    std::swap(top, bottom);
  }
  if (top == bottom) {
    bottom = top + 1.0;
  }

  auto minimum = std::numeric_limits<double>::infinity();
  auto maximum = -std::numeric_limits<double>::infinity();
  for (std::uint64_t index = 0; index < values.length(); ++index) {
    const auto value = values.value_as_double(index);
    if (value.has_value() && std::isfinite(*value)) {
      minimum = std::min(minimum, *value);
      maximum = std::max(maximum, *value);
    }
  }
  if (!std::isfinite(minimum) || !std::isfinite(maximum)) {
    minimum = 0.0;
    maximum = 1.0;
  } else if (minimum == maximum) {
    maximum = minimum + 1.0;
  }

  const auto track_id =
      derive_presentation_id(document_id, "welllog-python/default-track",
                             {document_id, axis_id, curve_id});
  const auto scale_id =
      derive_presentation_id(document_id, "welllog-python/default-scale",
                             {document_id, axis_id, curve_id, track_id});
  const auto layer_role =
      std::string{"welllog-python/default-layer/"} + curve_id.to_string();
  const auto layer_id = derive_presentation_id(
      document_id, layer_role,
      {document_id, axis_id, curve_id, track_id, scale_id});
  ScenePresentationBuilder presentation_builder(
      document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = depth_unit,
          .top = top,
          .bottom = bottom,
      },
      Millimetres{100.0}, "welllog-python-default");
  presentation_builder.add_track(
      TrackSpec{.id = track_id, .width = Millimetres{40.0}, .z_order = 0});
  presentation_builder.add_scale(TrackScaleSpec{
      .id = scale_id,
      .track_id = track_id,
      .mode = ScaleMode::linear,
      .minimum = minimum,
      .maximum = maximum,
      .direction = ScaleDirection::left_to_right,
      .unit = value_unit,
  });
  presentation_builder.add_curve_layer(CurveLayerSpec{
      .id = layer_id,
      .track_id = track_id,
      .curve_id = curve_id,
      .scale_id = scale_id,
      .color =
          RgbaColor{.red = 0x19, .green = 0x72, .blue = 0xb8, .alpha = 0xff},
      .line_width = Millimetres{0.35},
      .z_order = 0,
      .visible = true,
  });
  return view.session().execute(
      SetPresentationCommand{presentation_builder.build()});
}

[[nodiscard]] PyObject *buffer_report(const AdaptedBuffer &buffer) {
  auto *report = PyDict_New();
  if (report == nullptr) {
    return nullptr;
  }
  auto put = [report](const char *key, PyObject *value) {
    if (value == nullptr || PyDict_SetItemString(report, key, value) != 0) {
      Py_XDECREF(value);
      return false;
    }
    Py_DECREF(value);
    return true;
  };
  if (!put("access_mode", PyUnicode_FromString("zero_copy")) ||
      !put("dtype", PyUnicode_FromString(buffer.dtype.c_str())) ||
      !put("length", PyLong_FromUnsignedLongLong(buffer.buffer.length())) ||
      !put("stride_bytes",
           PyLong_FromUnsignedLongLong(buffer.buffer.stride_bytes())) ||
      !put("address", PyLong_FromUnsignedLongLong(buffer.address))) {
    Py_DECREF(report);
    return nullptr;
  }
  return report;
}

} // namespace

namespace {

void dict_get_string_optional(PyObject *dict, const char *key, QString *out);

PyObject *submit_curve_impl(WellLogView *view, PyObject *depth,
                            PyObject *values, const QString &document_id_text,
                            const QString &axis_id_text,
                            const QString &curve_id_text,
                            const QString &mnemonic, const QString &depth_unit,
                            const QString &value_unit) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "curve submission must run on the Qt GUI thread");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  const auto axis_id = parse_id(axis_id_text, "axis_id");
  const auto curve_id = parse_id(curve_id_text, "curve_id");
  if (!document_id || !axis_id || !curve_id) {
    return nullptr;
  }
  if (depth_unit.isEmpty() || value_unit.isEmpty()) {
    set_welllog_error("WellLogValidationError", "invalid_presentation",
                      "depth_unit and value_unit must be non-empty");
    return nullptr;
  }
  auto depth_buffer = adapt_buffer(depth, "depth");
  if (!depth_buffer) {
    return nullptr;
  }
  auto value_buffer = adapt_buffer(values, "values");
  if (!value_buffer) {
    return nullptr;
  }
  if (depth_buffer->buffer.length() != value_buffer->buffer.length()) {
    set_welllog_error("WellLogValidationError", "length_mismatch",
                      "depth and values must have the same length");
    return nullptr;
  }

  WellLogDocumentBuilder builder(*document_id, DocumentRevision{1});
  const auto depth_unit_utf8 = depth_unit.toUtf8();
  const auto value_unit_utf8 = value_unit.toUtf8();
  const auto mnemonic_utf8 = mnemonic.toUtf8();
  const auto first_depth = depth_buffer->buffer.value_as_double(0).value();
  const auto last_depth =
      depth_buffer->buffer.value_as_double(depth_buffer->buffer.length() - 1)
          .value();
  builder.add_sampling_axis(SamplingAxis{
      .id = *axis_id,
      .coordinates = depth_buffer->buffer,
      .domain = DepthDomain::measured_depth,
      .unit = depth_unit_utf8.constData(),
      .direction = last_depth < first_depth ? AxisDirection::decreasing
                                            : AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = *curve_id,
      .mnemonic = mnemonic_utf8.constData(),
      .display_name = mnemonic_utf8.constData(),
      .unit = value_unit_utf8.constData(),
      .sampling_axis_id = *axis_id,
      .values = value_buffer->buffer,
      .nulls = {},
  });
  const auto result =
      view->session().execute(SetDocumentCommand{builder.build()});
  if (!result.has_value()) {
    set_result_error(result.error(), "document submission");
    return nullptr;
  }
  const auto presentation = prepare_default_curve_scene(
      *view, *document_id, *axis_id, *curve_id, depth_buffer->buffer,
      value_buffer->buffer, depth_unit_utf8.constData(),
      value_unit_utf8.constData());
  if (!presentation.has_value()) {
    set_result_error(presentation.error(), "presentation preparation");
    return nullptr;
  }
  view->set_document_id(*document_id);

  auto *report = PyDict_New();
  auto *depth_report = buffer_report(*depth_buffer);
  auto *curve_report = buffer_report(*value_buffer);
  if (report == nullptr || depth_report == nullptr || curve_report == nullptr ||
      PyDict_SetItemString(report, "depth", depth_report) != 0 ||
      PyDict_SetItemString(report, "curve", curve_report) != 0 ||
      PyDict_SetItemString(report, "render_prepared", Py_True) != 0) {
    Py_XDECREF(report);
    Py_XDECREF(depth_report);
    Py_XDECREF(curve_report);
    return nullptr;
  }
  Py_DECREF(depth_report);
  Py_DECREF(curve_report);
  return report;
}

PyObject *sample_value_impl(WellLogView *view, const QString &curve_id_text,
                            unsigned long long sample_index) {
  if (view != nullptr && QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "sample_value must run on the Qt GUI thread");
    return nullptr;
  }
  if (view == nullptr || !view->document_id().has_value()) {
    Py_RETURN_NONE;
  }
  const auto curve_id = parse_id(curve_id_text, "curve_id");
  if (!curve_id) {
    return nullptr;
  }
  const auto document = view->session().document(*view->document_id());
  if (document == nullptr) {
    Py_RETURN_NONE;
  }
  for (const auto &curve : document->curves()) {
    if (curve.id == *curve_id) {
      const auto value = curve.values.value_as_double(
          static_cast<std::uint64_t>(sample_index));
      return value.has_value() ? PyFloat_FromDouble(*value)
                               : Py_NewRef(Py_None);
    }
  }
  Py_RETURN_NONE;
}

// Multi-rate support (Epic A): a curve dict may carry an optional "depth"
// array. When present the curve gets its own SamplingAxis and its values
// must match THAT axis length; otherwise it shares the document-level axis.
// The per-curve invariant (values == own axis) is preserved either way.
// Adds the axis (when present) + curve to the builder and returns the
// adapted values buffer so the caller can keep it alive in its bookkeeping.
[[nodiscard]] std::optional<AdaptedBuffer>
add_curve_with_optional_axis(WellLogDocumentBuilder &builder,
                             PyObject *curve_dict, PyObject *values_obj,
                             const EntityId &curve_id, const char *mnemonic,
                             const char *value_unit,
                             const BufferView &shared_depth,
                             const EntityId &shared_axis_id,
                             const char *depth_unit) {
  auto value_buffer = adapt_buffer(values_obj, "values");
  if (!value_buffer) {
    return std::nullopt;
  }
  auto *curve_depth_obj = PyDict_GetItemString(curve_dict, "depth");
  if (curve_depth_obj == nullptr) {
    if (value_buffer->buffer.length() != shared_depth.length()) {
      set_welllog_error("WellLogValidationError", "length_mismatch",
                        "curve values length must match depth");
      return std::nullopt;
    }
    builder.add_curve(Curve{
        .id = curve_id,
        .mnemonic = mnemonic,
        .display_name = mnemonic,
        .unit = value_unit,
        .sampling_axis_id = shared_axis_id,
        .values = value_buffer->buffer,
        .nulls = {},
    });
    return value_buffer;
  }
  auto curve_depth = adapt_buffer(curve_depth_obj, "curve.depth");
  if (!curve_depth) {
    return std::nullopt;
  }
  if (curve_depth->buffer.length() < 2) {
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      "curve.depth must have at least 2 samples");
    return std::nullopt;
  }
  if (value_buffer->buffer.length() != curve_depth->buffer.length()) {
    set_welllog_error("WellLogValidationError", "length_mismatch",
                      "curve values length must match its depth");
    return std::nullopt;
  }
  QString curve_axis_id_text;
  dict_get_string_optional(curve_dict, "axis_id", &curve_axis_id_text);
  if (curve_axis_id_text.isEmpty()) {
    curve_axis_id_text = QUuid::createUuid().toString(QUuid::WithoutBraces);
  }
  const auto curve_axis_id = parse_id(curve_axis_id_text, "curve axis_id");
  if (!curve_axis_id) {
    return std::nullopt;
  }
  const auto curve_first = curve_depth->buffer.value_as_double(0).value();
  const auto curve_last = curve_depth->buffer
                              .value_as_double(
                                  curve_depth->buffer.length() - 1)
                              .value();
  builder.add_sampling_axis(SamplingAxis{
      .id = *curve_axis_id,
      .coordinates = curve_depth->buffer,
      .domain = DepthDomain::measured_depth,
      .unit = depth_unit,
      .direction = curve_last < curve_first ? AxisDirection::decreasing
                                            : AxisDirection::increasing,
  });
  builder.add_curve(Curve{
      .id = curve_id,
      .mnemonic = mnemonic,
      .display_name = mnemonic,
      .unit = value_unit,
      .sampling_axis_id = *curve_axis_id,
      .values = value_buffer->buffer,
      .nulls = {},
  });
  return value_buffer;
}

} // namespace

PyObject *nice_axis_ticks(double d0, double d1,
                            unsigned long max_ticks) noexcept {
  try {
    const auto ticks = welllog::nice_axis_ticks(d0, d1, max_ticks);
    PyObject *list = PyList_New(static_cast<Py_ssize_t>(ticks.values.size()));
    if (list == nullptr) {
      return nullptr;
    }
    for (std::size_t i = 0; i < ticks.values.size(); ++i) {
      PyObject *item = PyFloat_FromDouble(ticks.values[i]);
      if (item == nullptr) {
        Py_DECREF(list);
        return nullptr;
      }
      PyList_SetItem(list, static_cast<Py_ssize_t>(i), item);
    }
    PyObject *tuple = PyTuple_New(2);
    if (tuple == nullptr) {
      Py_DECREF(list);
      return nullptr;
    }
    // PyTuple_SetItem(NULL) would build a tuple with a null slot that later
    // crashes the interpreter instead of raising (issue #482).
    PyObject *step_value = PyFloat_FromDouble(ticks.step);
    if (step_value == nullptr) {
      Py_DECREF(tuple);
      Py_DECREF(list);
      return nullptr;
    }
    PyTuple_SetItem(tuple, 0, step_value);
    PyTuple_SetItem(tuple, 1, list);
    return tuple;
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    return PyErr_SetString(PyExc_RuntimeError, "axis ticks failed"), nullptr;
  }
}

PyObject *format_axis_tick_label(double value, double step) noexcept {
  try {
    const auto label = welllog::format_axis_tick_label(value, step);
    return PyUnicode_FromStringAndSize(label.data(),
                                       static_cast<Py_ssize_t>(label.size()));
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    return PyErr_SetString(PyExc_RuntimeError, "tick label failed"), nullptr;
  }
}

PyObject *ticks_for_secondary_axis(PyObject *points, double display_top,
                                    double display_bottom,
                                    unsigned long max_ticks) noexcept {
  try {
    if (points == nullptr || !PyList_Check(points)) {
      return PyErr_SetString(PyExc_TypeError,
                             "points must be a list of [reference, display]"),
             nullptr;
    }
    std::vector<std::pair<double, double>> pairs;
    pairs.reserve(static_cast<std::size_t>(PyList_Size(points)));
    for (Py_ssize_t i = 0; i < PyList_Size(points); ++i) {
      PyObject *item = PyList_GetItem(points, i);
      if (item == nullptr || !(PyList_Check(item) || PyTuple_Check(item)) ||
          PySequence_Size(item) != 2) {
        return PyErr_SetString(PyExc_TypeError,
                               "each point must be a [reference, display] pair"),
               nullptr;
      }
      PyObject *ref_obj = PySequence_GetItem(item, 0);
      PyObject *disp_obj = PySequence_GetItem(item, 1);
      if (ref_obj == nullptr || disp_obj == nullptr) {
        Py_XDECREF(ref_obj);
        Py_XDECREF(disp_obj);
        return nullptr;
      }
      const double ref = PyFloat_AsDouble(ref_obj);
      const double disp = PyFloat_AsDouble(disp_obj);
      Py_DECREF(ref_obj);
      Py_DECREF(disp_obj);
      if (PyErr_Occurred()) {
        return nullptr;
      }
      pairs.emplace_back(ref, disp);
    }
    const auto ticks = welllog::ticks_for_secondary_window(
        pairs, display_top, display_bottom, max_ticks);
    PyObject *list = PyList_New(static_cast<Py_ssize_t>(ticks.values.size()));
    if (list == nullptr) {
      return nullptr;
    }
    for (std::size_t i = 0; i < ticks.values.size(); ++i) {
      PyObject *item = PyFloat_FromDouble(ticks.values[i]);
      if (item == nullptr) {
        Py_DECREF(list);
        return nullptr;
      }
      PyList_SetItem(list, static_cast<Py_ssize_t>(i), item);
    }
    PyObject *tuple = PyTuple_New(2);
    if (tuple == nullptr) {
      Py_DECREF(list);
      return nullptr;
    }
    // PyTuple_SetItem(NULL) would build a tuple with a null slot that later
    // crashes the interpreter instead of raising (issue #482).
    PyObject *step_value = PyFloat_FromDouble(ticks.step);
    if (step_value == nullptr) {
      Py_DECREF(tuple);
      Py_DECREF(list);
      return nullptr;
    }
    PyTuple_SetItem(tuple, 0, step_value);
    PyTuple_SetItem(tuple, 1, list);
    return tuple;
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    return PyErr_SetString(PyExc_RuntimeError, "secondary axis ticks failed"),
           nullptr;
  }
}

PyObject *submit_curve(WellLogView *view, PyObject *depth, PyObject *values,
                       const QString &document_id_text,
                       const QString &axis_id_text,
                       const QString &curve_id_text, const QString &mnemonic,
                       const QString &depth_unit,
                       const QString &value_unit) noexcept {
  try {
    return submit_curve_impl(view, depth, values, document_id_text,
                             axis_id_text, curve_id_text, mnemonic, depth_unit,
                             value_unit);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during curve submission");
    return nullptr;
  }
}

PyObject *sample_value(WellLogView *view, const QString &curve_id_text,
                       unsigned long long sample_index) noexcept {
  try {
    return sample_value_impl(view, curve_id_text, sample_index);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure while reading a curve sample");
    return nullptr;
  }
}

namespace {

[[nodiscard]] bool dict_get_string(PyObject *dict, const char *key,
                                   QString *out) {
  auto *item = PyDict_GetItemString(dict, key);
  if (item == nullptr || !PyUnicode_Check(item)) {
    return false;
  }
  Py_ssize_t size = 0;
  // PyUnicode_AsUTF8AndSize is available under limited API 3.10+.
  const char *utf8 = PyUnicode_AsUTF8AndSize(item, &size);
  if (utf8 == nullptr) {
    // Clear the conversion error so a failed read never leaves a stale
    // error indicator that poisons later binding calls (review D-002).
    PyErr_Clear();
    return false;
  }
  *out = QString::fromUtf8(utf8, static_cast<qsizetype>(size));
  return true;
}

[[nodiscard]] bool dict_get_float(PyObject *dict, const char *key, double *out) {
  auto *item = PyDict_GetItemString(dict, key);
  if (item == nullptr) {
    return false;
  }
  const auto value = PyFloat_AsDouble(item);
  if (PyErr_Occurred()) {
    // Non-numeric value: clear the TypeError so callers that skip the item
    // don't carry a stale error into later successful work (review D-002).
    PyErr_Clear();
    return false;
  }
  *out = value;
  return true;
}

// Optional helpers — return false without treating as hard error when missing.
void dict_get_float_optional(PyObject *dict, const char *key, double *out) {
  if (!dict_get_float(dict, key, out)) {
    PyErr_Clear();
  }
}

void dict_get_string_optional(PyObject *dict, const char *key, QString *out) {
  if (!dict_get_string(dict, key, out)) {
    PyErr_Clear();
  }
}

// Tolerant MarkerSemantic parsing for the markers payload list. An absent
// key keeps `fallback` (historically formation_top); an unknown token maps to
// custom rather than pretending to be a known semantic.
[[nodiscard]] MarkerSemantic
marker_semantic_from_dict(PyObject *marker, MarkerSemantic fallback) {
  QString text;
  dict_get_string_optional(marker, "semantic", &text);
  if (text.isEmpty()) {
    return fallback;
  }
  const auto s = text.toUtf8();
  if (s == "formation_top") {
    return MarkerSemantic::formation_top;
  }
  if (s == "fault") {
    return MarkerSemantic::fault;
  }
  if (s == "fluid_contact") {
    return MarkerSemantic::fluid_contact;
  }
  if (s == "casing_shoe") {
    return MarkerSemantic::casing_shoe;
  }
  return MarkerSemantic::custom;
}

// Interval semantics belong to the retained document.  An absent or unknown
// payload value remains custom, preserving the historical generic interval
// surface while enabling native lithology/facies tracks.
[[nodiscard]] IntervalSemantic
interval_semantic_from_text(const QString &text,
                            IntervalSemantic fallback =
                                IntervalSemantic::custom) {
  if (text.isEmpty()) {
    return fallback;
  }
  const auto semantic = text.trimmed().toLower().toUtf8();
  if (semantic == "lithology") {
    return IntervalSemantic::lithology;
  }
  if (semantic == "stratigraphy") {
    return IntervalSemantic::stratigraphy;
  }
  if (semantic == "sequence") {
    return IntervalSemantic::sequence;
  }
  if (semantic == "systems_tract") {
    return IntervalSemantic::systems_tract;
  }
  if (semantic == "facies") {
    return IntervalSemantic::facies;
  }
  return IntervalSemantic::custom;
}

[[nodiscard]] IntervalSemantic
interval_semantic_from_dict(PyObject *interval,
                            IntervalSemantic fallback =
                                IntervalSemantic::custom) {
  QString text;
  dict_get_string_optional(interval, "semantic", &text);
  return interval_semantic_from_text(text, fallback);
}

[[nodiscard]] RgbaColor parse_hex_color(const QString &text,
                                        RgbaColor fallback) {
  QString t = text.trimmed();
  if (t.startsWith(QLatin1Char('#'))) {
    t = t.mid(1);
  }
  if (t.size() == 6) {
    bool ok = false;
    const auto value = t.toUInt(&ok, 16);
    if (ok) {
      return RgbaColor{
          .red = static_cast<std::uint8_t>((value >> 16) & 0xff),
          .green = static_cast<std::uint8_t>((value >> 8) & 0xff),
          .blue = static_cast<std::uint8_t>(value & 0xff),
          .alpha = 0xff,
      };
    }
  }
  return fallback;
}

[[nodiscard]] double buffer_min_max(const BufferView &values, double *maximum) {
  auto minimum = std::numeric_limits<double>::infinity();
  auto max_v = -std::numeric_limits<double>::infinity();
  for (std::uint64_t index = 0; index < values.length(); ++index) {
    const auto value = values.value_as_double(index);
    if (value.has_value() && std::isfinite(*value)) {
      minimum = std::min(minimum, *value);
      max_v = std::max(max_v, *value);
    }
  }
  if (!std::isfinite(minimum) || !std::isfinite(max_v)) {
    minimum = 0.0;
    max_v = 1.0;
  } else if (minimum == max_v) {
    max_v = minimum + 1.0;
  }
  *maximum = max_v;
  return minimum;
}

[[nodiscard]] double buffer_min_max(const CurveBuffer &values,
                                    double *maximum) {
  auto minimum = std::numeric_limits<double>::infinity();
  auto max_v = -std::numeric_limits<double>::infinity();
  for (std::uint64_t index = 0; index < values.length(); ++index) {
    const auto value = values.value_as_double(index);
    if (value.has_value() && std::isfinite(*value)) {
      minimum = std::min(minimum, *value);
      max_v = std::max(max_v, *value);
    }
  }
  if (!std::isfinite(minimum) || !std::isfinite(max_v)) {
    minimum = 0.0;
    max_v = 1.0;
  } else if (minimum == max_v) {
    max_v = minimum + 1.0;
  }
  *maximum = max_v;
  return minimum;
}

// Single-well multi-track (#225). Payload:
// {
//   document_id, depth, depth_unit,
//   axis_id? (auto), top?, bottom?,
//   curves: [{curve_id, mnemonic, values, value_unit}],
//   tracks: [{width_mm?, scale_min?, scale_max?, scale_mode?,
//             layers: [{curve_id, color?}]}],
//   markers?: [{id, depth, label?, semantic?}]
//   // semantic ∈ formation_top|fault|fluid_contact|casing_shoe|custom
//   // (absent → formation_top; unknown → custom)
// }
[[nodiscard]] PyObject *
submit_multi_track_impl(WellLogView *view, PyObject *payload) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "multi-track submission must run on the Qt GUI thread");
    return nullptr;
  }
  if (payload == nullptr || !PyDict_Check(payload)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload must be a dict");
    return nullptr;
  }

  QString document_id_text;
  QString depth_unit;
  if (!dict_get_string(payload, "document_id", &document_id_text) ||
      !dict_get_string(payload, "depth_unit", &depth_unit)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "document_id and depth_unit are required");
    return nullptr;
  }
  if (depth_unit.isEmpty()) {
    set_welllog_error("WellLogValidationError", "invalid_presentation",
                      "depth_unit must be non-empty");
    return nullptr;
  }
  auto *depth_obj = PyDict_GetItemString(payload, "depth");
  if (depth_obj == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      "payload.depth is required");
    return nullptr;
  }
  auto *curves_obj = PyDict_GetItemString(payload, "curves");
  auto *tracks_obj = PyDict_GetItemString(payload, "tracks");
  if (curves_obj == nullptr || !PyList_Check(curves_obj) ||
      PyList_Size(curves_obj) <= 0) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload.curves must be a non-empty list");
    return nullptr;
  }
  if (tracks_obj == nullptr || !PyList_Check(tracks_obj) ||
      PyList_Size(tracks_obj) <= 0) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload.tracks must be a non-empty list");
    return nullptr;
  }

  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  QString axis_id_text;
  dict_get_string_optional(payload, "axis_id", &axis_id_text);
  if (axis_id_text.isEmpty()) {
    axis_id_text = QUuid::createUuid().toString(QUuid::WithoutBraces);
  }
  const auto axis_id = parse_id(axis_id_text, "axis_id");
  if (!axis_id) {
    return nullptr;
  }

  auto depth_buffer = adapt_buffer(depth_obj, "depth");
  if (!depth_buffer) {
    return nullptr;
  }
  if (depth_buffer->buffer.length() < 2) {
    set_welllog_error("WellLogValidationError", "invalid_buffer",
                      "depth must have at least 2 samples");
    return nullptr;
  }

  struct CurveEntry {
    EntityId id;
    QString mnemonic;
    QString value_unit;
    AdaptedBuffer values;
  };
  std::vector<CurveEntry> curves;
  curves.reserve(static_cast<std::size_t>(PyList_Size(curves_obj)));
  std::unordered_map<std::string, std::size_t> curve_index_by_id;

  WellLogDocumentBuilder builder(*document_id, DocumentRevision{1});
  const auto depth_unit_utf8 = depth_unit.toUtf8();
  const auto first_depth = depth_buffer->buffer.value_as_double(0).value();
  const auto last_depth =
      depth_buffer->buffer
          .value_as_double(depth_buffer->buffer.length() - 1)
          .value();
  builder.add_sampling_axis(SamplingAxis{
      .id = *axis_id,
      .coordinates = depth_buffer->buffer,
      .domain = DepthDomain::measured_depth,
      .unit = depth_unit_utf8.constData(),
      .direction = last_depth < first_depth ? AxisDirection::decreasing
                                            : AxisDirection::increasing,
  });

  for (Py_ssize_t ci = 0; ci < PyList_Size(curves_obj); ++ci) {
    auto *curve = PyList_GetItem(curves_obj, ci);
    if (curve == nullptr || !PyDict_Check(curve)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "each curve must be a dict");
      return nullptr;
    }
    QString curve_id_text;
    QString mnemonic;
    QString value_unit;
    if (!dict_get_string(curve, "curve_id", &curve_id_text) ||
        !dict_get_string(curve, "mnemonic", &mnemonic) ||
        !dict_get_string(curve, "value_unit", &value_unit)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "curve needs curve_id, mnemonic, value_unit");
      return nullptr;
    }
    if (value_unit.isEmpty()) {
      value_unit = QStringLiteral("unit");
    }
    auto *values_obj = PyDict_GetItemString(curve, "values");
    if (values_obj == nullptr) {
      set_welllog_error("WellLogValidationError", "invalid_buffer",
                        "curve.values is required");
      return nullptr;
    }
    const auto curve_id = parse_id(curve_id_text, "curve_id");
    if (!curve_id) {
      return nullptr;
    }
    const auto mnemonic_utf8 = mnemonic.toUtf8();
    const auto value_unit_utf8 = value_unit.toUtf8();
    auto value_buffer = add_curve_with_optional_axis(
        builder, curve, values_obj, *curve_id, mnemonic_utf8.constData(),
        value_unit_utf8.constData(), depth_buffer->buffer, *axis_id,
        depth_unit_utf8.constData());
    if (!value_buffer) {
      return nullptr;
    }
    curve_index_by_id[curve_id->to_string()] = curves.size();
    curves.push_back(CurveEntry{
        .id = *curve_id,
        .mnemonic = mnemonic,
        .value_unit = value_unit,
        .values = std::move(*value_buffer),
    });
  }

  // Optional markers
  auto *markers_obj = PyDict_GetItemString(payload, "markers");
  if (markers_obj != nullptr && PyList_Check(markers_obj)) {
    const auto marker_count = PyList_Size(markers_obj);
    for (Py_ssize_t mi = 0; mi < marker_count; ++mi) {
      auto *marker = PyList_GetItem(markers_obj, mi);
      if (marker == nullptr || !PyDict_Check(marker)) {
        continue;
      }
      QString marker_id_text;
      double marker_depth = 0.0;
      QString label;
      if (!dict_get_string(marker, "id", &marker_id_text) ||
          !dict_get_float(marker, "depth", &marker_depth)) {
        continue;
      }
      dict_get_string_optional(marker, "label", &label);
      const auto marker_id = parse_id(marker_id_text, "marker_id");
      if (!marker_id) {
        PyErr_Clear();
        continue;
      }
      const auto label_utf8 = label.toUtf8();
      builder.add_marker(Marker{
          .id = *marker_id,
          .reference_depth = marker_depth,
          .semantic =
              marker_semantic_from_dict(marker, MarkerSemantic::formation_top),
          .label = label_utf8.constData(),
      });
    }
  }

  // Intervals (T4 / #276): document-side depth spans with optional pattern
  // fill. Mirrors the markers block above. The presentation-side
  // add_interval_layer (drawn per track below) renders all document
  // intervals in the track.
  auto *intervals_obj = PyDict_GetItemString(payload, "intervals");
  if (intervals_obj != nullptr && PyList_Check(intervals_obj)) {
    const auto interval_count = PyList_Size(intervals_obj);
    for (Py_ssize_t ii = 0; ii < interval_count; ++ii) {
      auto *interval = PyList_GetItem(intervals_obj, ii);
      if (interval == nullptr || !PyDict_Check(interval)) {
        continue;
      }
      QString interval_id_text;
      double top_depth = 0.0;
      double bottom_depth = 0.0;
      if (!dict_get_string(interval, "id", &interval_id_text) ||
          !dict_get_float(interval, "top_depth", &top_depth) ||
          !dict_get_float(interval, "bottom_depth", &bottom_depth)) {
        continue;
      }
      if (!(bottom_depth > top_depth)) {
        continue;  // zero/negative thickness — treat as a marker, not interval
      }
      const auto interval_id = parse_id(interval_id_text, "interval_id");
      if (!interval_id) {
        PyErr_Clear();
        continue;
      }
      QString fill_text;
      dict_get_string_optional(interval, "fill_color", &fill_text);
      const auto fill = parse_hex_color(
          fill_text, RgbaColor{0xcc, 0xcc, 0xcc, 0xff});
      QString label;
      dict_get_string_optional(interval, "label", &label);
      // Optional pattern_id: references a pattern registered on the
      // presentation (parsed below). A nil id = solid fill only.
      EntityId pattern_id{};
      QString pattern_id_text;
      dict_get_string_optional(interval, "pattern_id", &pattern_id_text);
      if (!pattern_id_text.isEmpty()) {
        const auto parsed = parse_id(pattern_id_text, "pattern_id");
        if (parsed) {
          pattern_id = *parsed;
        } else {
          PyErr_Clear();
        }
      }
      const auto label_utf8 = label.toUtf8();
      builder.add_interval(Interval{
          .id = *interval_id,
          .top_reference_depth = top_depth,
          .bottom_reference_depth = bottom_depth,
          .semantic = interval_semantic_from_dict(interval),
          .pattern_id = pattern_id,
          .fill_color = fill,
          .label = label_utf8.constData(),
      });
    }
  }

  auto built = builder.build();
  if (built.id().is_nil()) {
    set_welllog_error("WellLogError", "resource_exhausted",
                      "document build failed");
    return nullptr;
  }
  const auto doc_result =
      view->session().execute(SetDocumentCommand{std::move(built)});
  if (!doc_result.has_value()) {
    set_result_error(doc_result.error(), "document submission");
    return nullptr;
  }

  double top = first_depth;
  double bottom = last_depth;
  if (top > bottom) {
    std::swap(top, bottom);
  }
  if (top == bottom) {
    bottom = top + 1.0;
  }
  dict_get_float_optional(payload, "top", &top);
  dict_get_float_optional(payload, "bottom", &bottom);
  if (!(bottom > top)) {
    set_welllog_error("WellLogValidationError", "invalid_viewport",
                      "top/bottom must form a positive depth span");
    return nullptr;
  }

  ScenePresentationBuilder presentation_builder(
      *document_id,
      ReferenceDepthRange{
          .domain = DepthDomain::measured_depth,
          .unit = depth_unit_utf8.constData(),
          .top = top,
          .bottom = bottom,
      },
      Millimetres{100.0}, "welllog-python-multi-track");

  // Optional "depth_transform": [{reference, display}, ...] control points
  // (TVD/TVDSS display domains). Same key as the multi-well path.
  auto *xform_obj = PyDict_GetItemString(payload, "depth_transform");
  if (xform_obj != nullptr && PyList_Check(xform_obj)) {
    DepthTransform transform{};
    const auto npts = PyList_Size(xform_obj);
    for (Py_ssize_t pi = 0; pi < npts; ++pi) {
      auto *pt = PyList_GetItem(xform_obj, pi);
      if (pt == nullptr || !PyDict_Check(pt)) {
        continue;
      }
      double ref = 0.0;
      double disp = 0.0;
      if (!dict_get_float(pt, "reference", &ref) ||
          !dict_get_float(pt, "display", &disp)) {
        continue;
      }
      transform.control_points.push_back(DepthControlPoint{
          .reference_depth = ref,
          .display_depth = disp,
      });
    }
    if (!transform.control_points.empty()) {
      transform.version = 1;
      presentation_builder.set_depth_transform(transform);
    }
  }

  const auto track_count = PyList_Size(tracks_obj);
  int z_order = 0;
  int curve_track_count = 0;
  int presentation_track_count = 0;
  int interval_track_count = 0;
  for (Py_ssize_t ti = 0; ti < track_count; ++ti) {
    auto *track = PyList_GetItem(tracks_obj, ti);
    if (track == nullptr || !PyDict_Check(track)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "each track must be a dict");
      return nullptr;
    }
    auto *layers_obj = PyDict_GetItemString(track, "layers");
    const bool has_curve_layers =
        layers_obj != nullptr && PyList_Check(layers_obj) &&
        PyList_Size(layers_obj) > 0;
    QString interval_semantic_text;
    dict_get_string_optional(track, "interval_semantic",
                             &interval_semantic_text);
    const bool has_interval_layer = !interval_semantic_text.isEmpty();
    if (!has_curve_layers && !has_interval_layer) {
      // Empty host depth-role tracks are intentionally omitted.
      continue;
    }
    double width_mm = 40.0;
    dict_get_float_optional(track, "width_mm", &width_mm);
    if (width_mm < 5.0) {
      width_mm = 5.0;
    }
    const auto track_role =
        std::string{"welllog-python/mt-track/"} + std::to_string(ti);
    const auto track_id = derive_presentation_id(
        *document_id, track_role, {*document_id, *axis_id});
    presentation_builder.add_track(TrackSpec{
        .id = track_id,
        .width = Millimetres{width_mm},
        .z_order = z_order++,
        .header = TrackHeaderSpec{.height = Millimetres{8.0},
                                  .font_size = Millimetres{2.5}},
    });
    ++presentation_track_count;

    // A retained interval-only track has no numeric scale or curve layer.
    // It is valid when the host requests a semantic interval column.
    if (!has_curve_layers) {
      ++interval_track_count;
      continue;
    }

    // Scale from first layer curve or explicit min/max
    auto *first_layer = PyList_GetItem(layers_obj, 0);
    QString first_curve_id_text;
    if (first_layer != nullptr && PyDict_Check(first_layer)) {
      dict_get_string_optional(first_layer, "curve_id", &first_curve_id_text);
    }
    // Hoist the lookup key once per track (review D-008: avoid repeated
    // toStdString heap allocations in the scale/unit lookups below).
    const auto first_curve_key = first_curve_id_text.toStdString();
    double scale_min = 0.0;
    double scale_max = 100.0;
    bool have_explicit = dict_get_float(track, "scale_min", &scale_min);
    bool have_max = dict_get_float(track, "scale_max", &scale_max);
    if (!have_explicit || !have_max) {
      auto it = curve_index_by_id.find(first_curve_key);
      if (it != curve_index_by_id.end()) {
        double auto_max = 1.0;
        const double auto_min =
            buffer_min_max(curves[it->second].values.buffer, &auto_max);
        if (!have_explicit) {
          scale_min = auto_min;
        }
        if (!have_max) {
          scale_max = auto_max;
        }
      }
    }
    if (!(scale_max > scale_min)) {
      scale_max = scale_min + 1.0;
    }
    QString scale_mode_text;
    dict_get_string_optional(track, "scale_mode", &scale_mode_text);
    const ScaleMode scale_mode =
        scale_mode_text.compare(QStringLiteral("log"), Qt::CaseInsensitive) == 0
            ? ScaleMode::logarithmic
            : ScaleMode::linear;

    QString scale_unit = QStringLiteral("unit");
    auto it_unit = curve_index_by_id.find(first_curve_key);
    if (it_unit != curve_index_by_id.end()) {
      scale_unit = curves[it_unit->second].value_unit;
    }
    const auto scale_role =
        std::string{"welllog-python/mt-scale/"} + std::to_string(ti);
    const auto scale_id = derive_presentation_id(
        *document_id, scale_role, {*document_id, *axis_id, track_id});
    const auto scale_unit_utf8 = scale_unit.toUtf8();
    // FRS §2.x 反向刻度 (density pair track): host-side ScaleSpec.reverse is
    // serialized as ``scale_reverse``. The C++ renderer already implements
    // right_to_left as ``1.0 - normalized_value`` (scene.cpp), so this only
    // needs payload plumbing - no new C++ rendering logic.
    bool scale_reverse = false;
    if (auto *rev_obj = PyDict_GetItemString(track, "scale_reverse")) {
      scale_reverse = PyObject_IsTrue(rev_obj) != 0;
    }
    presentation_builder.add_scale(TrackScaleSpec{
        .id = scale_id,
        .track_id = track_id,
        .mode = scale_mode,
        .minimum = scale_min,
        .maximum = scale_max,
        .direction = scale_reverse ? ScaleDirection::right_to_left
                                   : ScaleDirection::left_to_right,
        .unit = scale_unit_utf8.constData(),
    });

    for (Py_ssize_t li = 0; li < PyList_Size(layers_obj); ++li) {
      auto *layer = PyList_GetItem(layers_obj, li);
      if (layer == nullptr || !PyDict_Check(layer)) {
        continue;
      }
      QString layer_curve_id_text;
      if (!dict_get_string(layer, "curve_id", &layer_curve_id_text)) {
        continue;
      }
      const auto layer_curve_id = parse_id(layer_curve_id_text, "curve_id");
      if (!layer_curve_id) {
        PyErr_Clear();
        continue;
      }
      if (curve_index_by_id.find(layer_curve_id->to_string()) ==
          curve_index_by_id.end()) {
        set_welllog_error("WellLogValidationError", "invalid_document",
                          "track layer curve_id not in curves list");
        return nullptr;
      }
      QString color_text;
      dict_get_string_optional(layer, "color", &color_text);
      const auto color = parse_hex_color(
          color_text,
          RgbaColor{.red = 0x19, .green = 0x72, .blue = 0xb8, .alpha = 0xff});
      const auto layer_role = std::string{"welllog-python/mt-layer/"} +
                              std::to_string(ti) + "/" + std::to_string(li);
      const auto layer_id = derive_presentation_id(
          *document_id, layer_role,
          {*document_id, *axis_id, track_id, scale_id, *layer_curve_id});
      presentation_builder.add_curve_layer(CurveLayerSpec{
          .id = layer_id,
          .track_id = track_id,
          .curve_id = *layer_curve_id,
          .scale_id = scale_id,
          .color = color,
          .line_width = Millimetres{0.35},
          .z_order = static_cast<std::int32_t>(li),
          .visible = true,
      });
    }
    ++curve_track_count;
  }

  if (curve_track_count < 1) {
    set_welllog_error("WellLogValidationError", "invalid_presentation",
                      "no tracks with curve layers were provided");
    return nullptr;
  }

  if (markers_obj != nullptr && PyList_Check(markers_obj) &&
      PyList_Size(markers_obj) > 0) {
    // Optional "marker_symbols": true → the auto marker layer draws each
    // marker's MarkerSemantic symbol glyph (SDK marker symbols).
    bool marker_symbols = false;
    auto *symbols_flag = PyDict_GetItemString(payload, "marker_symbols");
    if (symbols_flag != nullptr && PyObject_IsTrue(symbols_flag) == 1) {
      marker_symbols = true;
    }
    PyErr_Clear();
    // Marker layer on the first track that has curve layers.
    for (Py_ssize_t ti = 0; ti < track_count; ++ti) {
      auto *track = PyList_GetItem(tracks_obj, ti);
      auto *layers_obj =
          track != nullptr ? PyDict_GetItemString(track, "layers") : nullptr;
      if (layers_obj == nullptr || !PyList_Check(layers_obj) ||
          PyList_Size(layers_obj) <= 0) {
        continue;
      }
      const auto track_role =
          std::string{"welllog-python/mt-track/"} + std::to_string(ti);
      const auto track_id = derive_presentation_id(
          *document_id, track_role, {*document_id, *axis_id});
      const auto marker_layer_id = derive_presentation_id(
          *document_id, "welllog-python/mt-marker-layer",
          {*document_id, *axis_id, track_id});
      presentation_builder.add_marker_layer(MarkerLayerSpec{
          .id = marker_layer_id,
          .track_id = track_id,
          .z_order = 50,
          .line_color = RgbaColor{200, 40, 40, 255},
          .line_width = Millimetres{0.4},
          .draw_labels = true,
          .draw_symbols = marker_symbols,
      });
      break;
    }
  }

  // Patterns (T4 / #276): presentation-level vector tile sources
  // referenced by interval fills. Parsed and registered before build so
  // add_interval_layer can emit referencing them.
  auto *patterns_obj = PyDict_GetItemString(payload, "patterns");
  if (patterns_obj != nullptr && PyList_Check(patterns_obj)) {
    const auto pattern_count = PyList_Size(patterns_obj);
    for (Py_ssize_t pi = 0; pi < pattern_count; ++pi) {
      auto *pattern = PyList_GetItem(patterns_obj, pi);
      if (pattern == nullptr || !PyDict_Check(pattern)) {
        continue;
      }
      QString pattern_id_text;
      double tile_w = 5.0;
      double tile_h = 5.0;
      if (!dict_get_string(pattern, "id", &pattern_id_text) ||
          !dict_get_float(pattern, "tile_width_mm", &tile_w) ||
          !dict_get_float(pattern, "tile_height_mm", &tile_h)) {
        continue;
      }
      const auto pattern_id = parse_id(pattern_id_text, "pattern_id");
      if (!pattern_id) {
        PyErr_Clear();
        continue;
      }
      double rotation = 0.0;
      dict_get_float_optional(pattern, "rotation_degrees", &rotation);
      double stroke_w = 0.2;
      dict_get_float_optional(pattern, "stroke_width_mm", &stroke_w);
      QString fg_text;
      QString bg_text;
      dict_get_string_optional(pattern, "foreground", &fg_text);
      dict_get_string_optional(pattern, "background", &bg_text);
      const auto foreground =
          parse_hex_color(fg_text, RgbaColor{0x33, 0x33, 0x33, 0xff});
      const auto background =
          parse_hex_color(bg_text, RgbaColor{0xff, 0xff, 0xff, 0x00});
      // Primitives: a list of {line/polyline/circle: {...}} dicts.
      std::vector<PatternPrimitive> primitives;
      auto *prims_obj = PyDict_GetItemString(pattern, "primitives");
      if (prims_obj != nullptr && PyList_Check(prims_obj)) {
        for (Py_ssize_t pri = 0; pri < PyList_Size(prims_obj); ++pri) {
          auto *prim = PyList_GetItem(prims_obj, pri);
          if (prim == nullptr || !PyDict_Check(prim)) {
            continue;
          }
          if (auto *line_obj = PyDict_GetItemString(prim, "line")) {
            double fx = 0, fy = 0, tx = 0, ty = 0;
            if (PyDict_Check(line_obj) &&
                dict_get_float(line_obj, "from_x", &fx) &&
                dict_get_float(line_obj, "from_y", &fy) &&
                dict_get_float(line_obj, "to_x", &tx) &&
                dict_get_float(line_obj, "to_y", &ty)) {
              primitives.emplace_back(PatternLine{
                  .from = PhysicalPoint{Millimetres{fx}, Millimetres{fy}},
                  .to = PhysicalPoint{Millimetres{tx}, Millimetres{ty}}});
            }
          } else if (auto *poly_obj =
                         PyDict_GetItemString(prim, "polyline")) {
            auto *pts_obj =
                poly_obj != nullptr && PyDict_Check(poly_obj)
                    ? PyDict_GetItemString(poly_obj, "points")
                    : nullptr;
            if (pts_obj != nullptr && PyList_Check(pts_obj)) {
              PatternPolyline poly{};
              for (Py_ssize_t ppi = 0; ppi < PyList_Size(pts_obj); ++ppi) {
                auto *pt = PyList_GetItem(pts_obj, ppi);
                if (pt == nullptr || !PyList_Check(pt) ||
                    PyList_Size(pt) < 2) {
                  continue;
                }
                auto *px = PyList_GetItem(pt, 0);
                auto *py = PyList_GetItem(pt, 1);
                if (px != nullptr && py != nullptr) {
                  poly.points.push_back(PhysicalPoint{
                      Millimetres{PyFloat_AsDouble(px)},
                      Millimetres{PyFloat_AsDouble(py)}});
                }
              }
              PyErr_Clear();
              if (!poly.points.empty()) {
                if (PyDict_Check(poly_obj)) {
                  double closed = 0.0;
                  dict_get_float_optional(poly_obj, "closed", &closed);
                  poly.closed = closed != 0.0;
                }
                primitives.emplace_back(std::move(poly));
              }
            }
          } else if (auto *circ_obj =
                         PyDict_GetItemString(prim, "circle")) {
            double cx = 0, cy = 0, r = 0;
            if (circ_obj != nullptr && PyDict_Check(circ_obj) &&
                dict_get_float(circ_obj, "center_x", &cx) &&
                dict_get_float(circ_obj, "center_y", &cy) &&
                dict_get_float(circ_obj, "radius_mm", &r)) {
              double filled = 0.0;
              dict_get_float_optional(circ_obj, "filled", &filled);
              primitives.emplace_back(PatternCircle{
                  .center = PhysicalPoint{Millimetres{cx}, Millimetres{cy}},
                  .radius = Millimetres{r},
                  .filled = filled != 0.0});
            }
          }
        }
        PyErr_Clear();
      }
      presentation_builder.add_pattern(PatternDefinition{
          .id = *pattern_id,
          .tile_width = Millimetres{tile_w},
          .tile_height = Millimetres{tile_h},
          .rotation_degrees = rotation,
          .foreground = foreground,
          .background = background,
          .stroke_width = Millimetres{stroke_w},
          .primitives = std::move(primitives),
      });
    }
  }

  // Interval layers can target retained interval-only tracks. A semantic
  // filter keeps lithology and facies in separate columns without duplicating
  // the document. Old payloads without a filter retain one all-interval layer
  // on the first curve track.
  if (intervals_obj != nullptr && PyList_Check(intervals_obj) &&
      PyList_Size(intervals_obj) > 0) {
    bool added_legacy_layer = false;
    for (Py_ssize_t ti = 0; ti < track_count; ++ti) {
      auto *track = PyList_GetItem(tracks_obj, ti);
      auto *layers_obj =
          track != nullptr ? PyDict_GetItemString(track, "layers") : nullptr;
      QString semantic_text;
      if (track != nullptr) {
        dict_get_string_optional(track, "interval_semantic", &semantic_text);
      }
      const bool has_curve_layers =
          layers_obj != nullptr && PyList_Check(layers_obj) &&
          PyList_Size(layers_obj) > 0;
      if (!has_curve_layers && semantic_text.isEmpty()) {
        continue;
      }
      if (semantic_text.isEmpty() && added_legacy_layer) {
        continue;
      }
      const auto track_role =
          std::string{"welllog-python/mt-track/"} + std::to_string(ti);
      const auto track_id = derive_presentation_id(
          *document_id, track_role, {*document_id, *axis_id});
      const auto interval_layer_id = derive_presentation_id(
          *document_id,
          std::string{"welllog-python/mt-interval-layer/"} +
              std::to_string(ti),
          {*document_id, *axis_id, track_id});
      presentation_builder.add_interval_layer(IntervalLayerSpec{
          .id = interval_layer_id,
          .track_id = track_id,
          .z_order = 5,  // below curves (z_order 0+) so fills sit behind
          .draw_labels = true,
          .label_font_size = Millimetres{2.5},
          .label_color = RgbaColor{0x33, 0x33, 0x33, 0xff},
          .semantic_filter = semantic_text.isEmpty()
                                 ? std::nullopt
                                 : std::optional<IntervalSemantic>{
                                       interval_semantic_from_text(
                                           semantic_text)},
      });
      if (semantic_text.isEmpty()) {
        added_legacy_layer = true;
      }
    }
  }

  auto presentation = presentation_builder.build();
  if (presentation.document_id().is_nil()) {
    set_welllog_error("WellLogError", "resource_exhausted",
                      "presentation build failed");
    return nullptr;
  }
  const auto pres_result = view->session().execute(
      SetPresentationCommand{std::move(presentation)});
  if (!pres_result.has_value()) {
    set_result_error(pres_result.error(), "presentation preparation");
    return nullptr;
  }
  // Clear multi-well layout so single-well multi-track is primary
  static_cast<void>(view->session().execute(ClearWellLayoutCommand{}));
  view->set_document_id(*document_id);

  auto *report = PyDict_New();
  if (report == nullptr) {
    return nullptr;
  }
  auto *depth_report = buffer_report(*depth_buffer);
  if (depth_report == nullptr ||
      PyDict_SetItemString(report, "depth", depth_report) != 0 ||
      PyDict_SetItemString(report, "curve_count",
                           PyLong_FromLong(static_cast<long>(curves.size()))) !=
          0 ||
      PyDict_SetItemString(
          report, "track_count",
          PyLong_FromLong(static_cast<long>(presentation_track_count))) != 0 ||
      PyDict_SetItemString(
          report, "curve_track_count",
          PyLong_FromLong(static_cast<long>(curve_track_count))) != 0 ||
      PyDict_SetItemString(
          report, "interval_track_count",
          PyLong_FromLong(static_cast<long>(interval_track_count))) != 0 ||
      PyDict_SetItemString(report, "document_id",
                           PyUnicode_FromString(
                               document_id->to_string().c_str())) != 0 ||
      PyDict_SetItemString(report, "render_prepared", Py_True) != 0) {
    Py_XDECREF(depth_report);
    Py_DECREF(report);
    return nullptr;
  }
  Py_DECREF(depth_report);
  return report;
}

[[nodiscard]] PyObject *append_curves_impl(WellLogView *view,
                                            PyObject *payload) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "curve append must run on the Qt GUI thread");
    return nullptr;
  }
  if (payload == nullptr || !PyDict_Check(payload)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "append payload must be a dict");
    return nullptr;
  }
  QString document_id_text;
  if (!dict_get_string(payload, "document_id", &document_id_text)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "append payload needs document_id");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  const auto document = view->session().document(*document_id);
  if (document == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "append target document does not exist");
    return nullptr;
  }
  auto *tails_obj = PyDict_GetItemString(payload, "tails");
  if (tails_obj == nullptr || !PyList_Check(tails_obj) ||
      PyList_Size(tails_obj) <= 0) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "append payload.tails must be a non-empty list");
    return nullptr;
  }

  std::vector<CurveTailBlock> blocks;
  blocks.reserve(static_cast<std::size_t>(PyList_Size(tails_obj)));
  for (Py_ssize_t index = 0; index < PyList_Size(tails_obj); ++index) {
    auto *tail = PyList_GetItem(tails_obj, index);
    if (tail == nullptr || !PyDict_Check(tail)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "each append tail must be a dict");
      return nullptr;
    }
    QString curve_id_text;
    QString axis_id_text;
    if (!dict_get_string(tail, "curve_id", &curve_id_text) ||
        !dict_get_string(tail, "axis_id", &axis_id_text)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "append tail needs curve_id and axis_id");
      return nullptr;
    }
    const auto curve_id = parse_id(curve_id_text, "curve_id");
    const auto axis_id = parse_id(axis_id_text, "axis_id");
    if (!curve_id || !axis_id) {
      return nullptr;
    }
    auto *depth_obj = PyDict_GetItemString(tail, "depth");
    auto *values_obj = PyDict_GetItemString(tail, "values");
    if (depth_obj == nullptr || values_obj == nullptr) {
      set_welllog_error("WellLogValidationError", "invalid_buffer",
                        "append tail needs depth and values buffers");
      return nullptr;
    }
    auto depth = adapt_buffer(depth_obj, "append.depth");
    auto values = adapt_buffer(values_obj, "append.values");
    if (!depth || !values) {
      return nullptr;
    }
    blocks.push_back(CurveTailBlock{
        .curve_id = *curve_id,
        .sampling_axis_id = *axis_id,
        .tail_coordinates = depth->buffer,
        .tail_values = values->buffer,
    });
  }

  QString viewport_mode;
  dict_get_string_optional(payload, "viewport_mode", &viewport_mode);
  if (viewport_mode.compare(QStringLiteral("follow_latest"),
                            Qt::CaseInsensitive) == 0) {
    view->session().set_append_viewport_mode(*document_id,
                                             AppendViewportMode::follow_latest);
  } else if (!viewport_mode.isEmpty()) {
    view->session().set_append_viewport_mode(*document_id,
                                             AppendViewportMode::fixed);
  }
  const auto result = view->session().execute(AppendBatchCommand{
      .document_id = *document_id,
      .target_revision = DocumentRevision{document->revision().value + 1},
      .blocks = std::move(blocks),
  });
  if (!result.has_value()) {
    set_result_error(result.error(), "curve append");
    return nullptr;
  }
  auto *report = PyDict_New();
  if (report == nullptr) {
    return nullptr;
  }
  auto *document_text =
      PyUnicode_FromString(document_id->to_string().c_str());
  auto *revision =
      PyLong_FromUnsignedLongLong(result.value().document_revision.value);
  auto *block_count = PyLong_FromSsize_t(PyList_Size(tails_obj));
  if (document_text == nullptr || revision == nullptr || block_count == nullptr ||
      PyDict_SetItemString(report, "document_id", document_text) != 0 ||
      PyDict_SetItemString(report, "revision", revision) != 0 ||
      PyDict_SetItemString(report, "append_block_count", block_count) != 0 ||
      PyDict_SetItemString(report, "incremental", Py_True) != 0) {
    Py_XDECREF(document_text);
    Py_XDECREF(revision);
    Py_XDECREF(block_count);
    Py_DECREF(report);
    return nullptr;
  }
  Py_DECREF(document_text);
  Py_DECREF(revision);
  Py_DECREF(block_count);
  return report;
}

[[nodiscard]] PyObject *document_metrics_impl(WellLogView *view,
                                               const QString &document_id_text) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "document metrics must run on the Qt GUI thread");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  const auto document = view->session().document(*document_id);
  if (document == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "metrics document does not exist");
    return nullptr;
  }
  auto *curve_lengths =
      PyList_New(static_cast<Py_ssize_t>(document->curves().size()));
  if (curve_lengths == nullptr) {
    return nullptr;
  }
  for (std::size_t index = 0; index < document->curves().size(); ++index) {
    auto *length = PyLong_FromUnsignedLongLong(
        document->curves()[index].values.length());
    if (length == nullptr ||
        PyList_SetItem(curve_lengths, static_cast<Py_ssize_t>(index), length) !=
            0) {
      Py_XDECREF(length);
      Py_DECREF(curve_lengths);
      return nullptr;
    }
  }
  const auto performance = view->session().performance_snapshot(*document_id);
  const auto frame = view->frame_stats();
  auto *report = PyDict_New();
  if (report == nullptr) {
    Py_DECREF(curve_lengths);
    return nullptr;
  }
  const auto prep_state = performance.has_value()
                              ? static_cast<unsigned long>(
                                    performance->preparation_state)
                              : 0UL;
  const auto revision = performance.has_value()
                            ? performance->document_revision.value
                            : document->revision().value;
  auto put = [report](const char *key, PyObject *value) {
    if (value == nullptr || PyDict_SetItemString(report, key, value) != 0) {
      Py_XDECREF(value);
      return false;
    }
    Py_DECREF(value);
    return true;
  };
  if (!put("curve_lengths", curve_lengths) ||
      !put("revision", PyLong_FromUnsignedLongLong(revision)) ||
      !put("preparation_state", PyLong_FromUnsignedLong(prep_state)) ||
      !put("lod_points_avg",
           PyLong_FromUnsignedLongLong(frame.lod_points_avg)) ||
      !put("frame_p95_ms", PyFloat_FromDouble(frame.frame_ms_p95)) ||
      !put("frame_samples", PyLong_FromUnsignedLongLong(frame.sample_count)) ||
      !put("cpu_derived_bytes", PyLong_FromUnsignedLongLong(
                                     performance.has_value()
                                         ? performance->cpu_derived_bytes
                                         : 0ULL)) ||
      !put("completed_tasks", PyLong_FromUnsignedLongLong(
                                  performance.has_value()
                                      ? performance->completed_tasks
                                      : 0ULL)) ||
      !put("cancelled_tasks", PyLong_FromUnsignedLongLong(
                                  performance.has_value()
                                      ? performance->cancelled_tasks
                                      : 0ULL))) {
    Py_DECREF(report);
    return nullptr;
  }
  return report;
}

[[nodiscard]] PyObject *patch_document_impl(WellLogView *view,
                                             PyObject *payload) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "document patch must run on the Qt GUI thread");
    return nullptr;
  }
  if (payload == nullptr || !PyDict_Check(payload)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "patch payload must be a dict");
    return nullptr;
  }
  QString document_id_text;
  if (!dict_get_string(payload, "document_id", &document_id_text)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "patch payload needs document_id");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  const auto document = view->session().document(*document_id);
  if (document == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "patch target document does not exist");
    return nullptr;
  }

  DocumentPatch patch{.base_revision = document->revision(), .edits = {}};
  bool has_edits = false;
  auto *intervals_obj = PyDict_GetItemString(payload, "intervals");
  if (intervals_obj != nullptr) {
    if (!PyList_Check(intervals_obj)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "patch intervals must be a list");
      return nullptr;
    }
    std::vector<EntityId> supplied_ids;
    supplied_ids.reserve(static_cast<std::size_t>(PyList_Size(intervals_obj)));
    std::vector<Interval> supplied_intervals;
    supplied_intervals.reserve(
        static_cast<std::size_t>(PyList_Size(intervals_obj)));
    for (Py_ssize_t index = 0; index < PyList_Size(intervals_obj); ++index) {
      auto *item = PyList_GetItem(intervals_obj, index);
      if (item == nullptr || !PyDict_Check(item)) {
        set_welllog_error("WellLogValidationError", "invalid_document",
                          "each patch interval must be a dict");
        return nullptr;
      }
      QString id_text;
      double top = 0.0;
      double bottom = 0.0;
      if (!dict_get_string(item, "id", &id_text) ||
          !dict_get_float(item, "top_depth", &top) ||
          !dict_get_float(item, "bottom_depth", &bottom) || !(bottom > top)) {
        set_welllog_error("WellLogValidationError", "invalid_document",
                          "patch interval needs a positive top/bottom span");
        return nullptr;
      }
      const auto id = parse_id(id_text, "interval_id");
      if (!id) {
        return nullptr;
      }
      QString fill_text;
      QString label;
      dict_get_string_optional(item, "fill_color", &fill_text);
      dict_get_string_optional(item, "label", &label);
      const auto existing = std::find_if(
          document->intervals().begin(), document->intervals().end(),
          [&id](const Interval &candidate) { return candidate.id == *id; });
      supplied_ids.push_back(*id);
      supplied_intervals.push_back(Interval{
          .id = *id,
          .top_reference_depth = top,
          .bottom_reference_depth = bottom,
          .semantic = interval_semantic_from_dict(item),
          // Preserve native pattern styling during a Workbench interval patch.
          .pattern_id = existing == document->intervals().end()
                            ? EntityId{}
                            : existing->pattern_id,
          .fill_color = parse_hex_color(
              fill_text, RgbaColor{0xcc, 0xcc, 0xcc, 0xff}),
          .label = label.toUtf8().constData(),
      });
    }
    for (const auto &existing : document->intervals()) {
      const bool managed = existing.semantic == IntervalSemantic::lithology ||
                           existing.semantic == IntervalSemantic::facies;
      if (managed && std::find(supplied_ids.begin(), supplied_ids.end(),
                               existing.id) == supplied_ids.end()) {
        patch.edits.emplace_back(RemoveEntity{.id = existing.id});
        has_edits = true;
      }
    }
    for (const auto &interval : supplied_intervals) {
      patch.edits.emplace_back(UpsertEntity{.entity = interval});
      has_edits = true;
    }
  }

  auto *tracks_obj = PyDict_GetItemString(payload, "tracks");
  if (tracks_obj != nullptr) {
    if (!PyList_Check(tracks_obj)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "patch tracks must be a list");
      return nullptr;
    }
    QString axis_id_text;
    if (!dict_get_string(payload, "axis_id", &axis_id_text)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "style patch needs axis_id");
      return nullptr;
    }
    const auto axis_id = parse_id(axis_id_text, "axis_id");
    if (!axis_id) {
      return nullptr;
    }
    std::unordered_map<std::string, const Curve *> curves_by_id;
    for (const auto &curve : document->curves()) {
      curves_by_id.emplace(curve.id.to_string(), &curve);
    }
    for (Py_ssize_t ti = 0; ti < PyList_Size(tracks_obj); ++ti) {
      auto *track = PyList_GetItem(tracks_obj, ti);
      if (track == nullptr || !PyDict_Check(track)) {
        set_welllog_error("WellLogValidationError", "invalid_document",
                          "each patch track must be a dict");
        return nullptr;
      }
      auto *layers_obj = PyDict_GetItemString(track, "layers");
      if (layers_obj == nullptr || !PyList_Check(layers_obj) ||
          PyList_Size(layers_obj) <= 0) {
        continue;  // retained interval-only track has no numeric style patch
      }
      const auto track_role =
          std::string{"welllog-python/mt-track/"} + std::to_string(ti);
      const auto track_id = derive_presentation_id(
          *document_id, track_role, {*document_id, *axis_id});
      auto *first_layer = PyList_GetItem(layers_obj, 0);
      QString first_curve_id_text;
      if (first_layer != nullptr && PyDict_Check(first_layer)) {
        dict_get_string_optional(first_layer, "curve_id", &first_curve_id_text);
      }
      const auto first_curve_id = parse_id(first_curve_id_text, "curve_id");
      if (!first_curve_id ||
          curves_by_id.find(first_curve_id->to_string()) == curves_by_id.end()) {
        set_welllog_error("WellLogValidationError", "invalid_document",
                          "patch track layer curve_id not in document");
        return nullptr;
      }
      double scale_min = 0.0;
      double scale_max = 100.0;
      const bool has_min = dict_get_float(track, "scale_min", &scale_min);
      const bool has_max = dict_get_float(track, "scale_max", &scale_max);
      if (!has_min || !has_max) {
        const auto &values = curves_by_id.at(first_curve_id->to_string())->values;
        double auto_max = 1.0;
        const auto auto_min = buffer_min_max(values, &auto_max);
        if (!has_min) {
          scale_min = auto_min;
        }
        if (!has_max) {
          scale_max = auto_max;
        }
      }
      if (!(scale_max > scale_min)) {
        scale_max = scale_min + 1.0;
      }
      QString scale_mode_text;
      dict_get_string_optional(track, "scale_mode", &scale_mode_text);
      bool scale_reverse = false;
      if (auto *reverse_obj = PyDict_GetItemString(track, "scale_reverse")) {
        scale_reverse = PyObject_IsTrue(reverse_obj) != 0;
      }
      const auto scale_id = derive_presentation_id(
          *document_id,
          std::string{"welllog-python/mt-scale/"} + std::to_string(ti),
          {*document_id, *axis_id, track_id});
      const auto unit =
          QString::fromStdString(curves_by_id.at(first_curve_id->to_string())
                                     ->unit);
      const auto unit_utf8 = unit.toUtf8();
      patch.edits.emplace_back(UpsertEntity{.entity = TrackScaleSpec{
          .id = scale_id,
          .track_id = track_id,
          .mode = scale_mode_text.compare(QStringLiteral("log"),
                                          Qt::CaseInsensitive) == 0
                      ? ScaleMode::logarithmic
                      : ScaleMode::linear,
          .minimum = scale_min,
          .maximum = scale_max,
          .direction = scale_reverse ? ScaleDirection::right_to_left
                                     : ScaleDirection::left_to_right,
          .unit = unit_utf8.constData(),
      }});
      has_edits = true;
      for (Py_ssize_t li = 0; li < PyList_Size(layers_obj); ++li) {
        auto *layer = PyList_GetItem(layers_obj, li);
        if (layer == nullptr || !PyDict_Check(layer)) {
          continue;
        }
        QString curve_id_text;
        if (!dict_get_string(layer, "curve_id", &curve_id_text)) {
          continue;
        }
        const auto curve_id = parse_id(curve_id_text, "curve_id");
        if (!curve_id ||
            curves_by_id.find(curve_id->to_string()) == curves_by_id.end()) {
          set_welllog_error("WellLogValidationError", "invalid_document",
                            "patch curve layer id not in document");
          return nullptr;
        }
        QString color_text;
        dict_get_string_optional(layer, "color", &color_text);
        const auto layer_id = derive_presentation_id(
            *document_id,
            std::string{"welllog-python/mt-layer/"} + std::to_string(ti) +
                "/" + std::to_string(li),
            {*document_id, *axis_id, track_id, scale_id, *curve_id});
        patch.edits.emplace_back(UpsertEntity{.entity = CurveLayerSpec{
            .id = layer_id,
            .track_id = track_id,
            .curve_id = *curve_id,
            .scale_id = scale_id,
            .color = parse_hex_color(
                color_text,
                RgbaColor{.red = 0x19, .green = 0x72, .blue = 0xb8,
                          .alpha = 0xff}),
            .line_width = Millimetres{0.35},
            .z_order = static_cast<std::int32_t>(li),
            .visible = true,
        }});
        has_edits = true;
      }
    }
  }

  if (!has_edits) {
    auto *report = PyDict_New();
    if (report == nullptr) {
      return nullptr;
    }
    auto *revision = PyLong_FromUnsignedLongLong(document->revision().value);
    if (revision == nullptr ||
        PyDict_SetItemString(report, "revision", revision) != 0 ||
        PyDict_SetItemString(report, "patched", Py_False) != 0) {
      Py_XDECREF(revision);
      Py_DECREF(report);
      return nullptr;
    }
    Py_DECREF(revision);
    return report;
  }
  const auto result = view->session().execute(ApplyPatchCommand{
      .document_id = *document_id,
      .patch = std::move(patch),
  });
  if (!result.has_value()) {
    set_result_error(result.error(), "document patch");
    return nullptr;
  }
  auto *report = PyDict_New();
  if (report == nullptr) {
    return nullptr;
  }
  auto *revision =
      PyLong_FromUnsignedLongLong(result.value().document_revision.value);
  if (revision == nullptr ||
      PyDict_SetItemString(report, "revision", revision) != 0 ||
      PyDict_SetItemString(report, "patched", Py_True) != 0) {
    Py_XDECREF(revision);
    Py_DECREF(report);
    return nullptr;
  }
  Py_DECREF(revision);
  return report;
}

[[nodiscard]] PyObject *
submit_multi_well_section_impl(WellLogView *view, PyObject *payload) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "multi-well submission must run on the Qt GUI thread");
    return nullptr;
  }
  if (payload == nullptr || !PyDict_Check(payload)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload must be a dict");
    return nullptr;
  }
  auto *wells_obj = PyDict_GetItemString(payload, "wells");
  if (wells_obj == nullptr || !PyList_Check(wells_obj)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload.wells must be a list");
    return nullptr;
  }
  const auto well_count = PyList_Size(wells_obj);
  if (well_count <= 0) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload.wells must be non-empty");
    return nullptr;
  }

  double gap_mm = 5.0;
  dict_get_float_optional(payload, "gap_mm", &gap_mm);
  double shared_top = 0.0;
  double shared_bottom = 1.0;
  dict_get_float_optional(payload, "shared_top", &shared_top);
  dict_get_float_optional(payload, "shared_bottom", &shared_bottom);
  if (!(shared_bottom > shared_top)) {
    set_welllog_error("WellLogValidationError", "invalid_viewport",
                      "shared_top/shared_bottom must form a positive span");
    return nullptr;
  }

  std::vector<EntityId> document_ids;
  document_ids.reserve(static_cast<std::size_t>(well_count));
  EntityId first_document{};

  for (Py_ssize_t wi = 0; wi < well_count; ++wi) {
    auto *well = PyList_GetItem(wells_obj, wi);
    if (well == nullptr || !PyDict_Check(well)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "each well must be a dict");
      return nullptr;
    }
    QString document_id_text;
    QString depth_unit;
    if (!dict_get_string(well, "document_id", &document_id_text) ||
        !dict_get_string(well, "depth_unit", &depth_unit)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "well needs document_id and depth_unit");
      return nullptr;
    }
    auto *depth_obj = PyDict_GetItemString(well, "depth");
    if (depth_obj == nullptr) {
      set_welllog_error("WellLogValidationError", "invalid_buffer",
                        "well.depth is required");
      return nullptr;
    }
    const auto document_id = parse_id(document_id_text, "document_id");
    if (!document_id) {
      return nullptr;
    }
    auto depth_buffer = adapt_buffer(depth_obj, "depth");
    if (!depth_buffer) {
      return nullptr;
    }
    if (depth_buffer->buffer.length() < 2) {
      set_welllog_error("WellLogValidationError", "invalid_buffer",
                        "depth must have at least 2 samples");
      return nullptr;
    }

    // Multi-track path (#232): well.curves[] + well.tracks[]
    auto *curves_obj = PyDict_GetItemString(well, "curves");
    auto *tracks_obj = PyDict_GetItemString(well, "tracks");
    const bool multi_track =
        curves_obj != nullptr && PyList_Check(curves_obj) &&
        PyList_Size(curves_obj) > 0 && tracks_obj != nullptr &&
        PyList_Check(tracks_obj) && PyList_Size(tracks_obj) > 0;

    QString axis_id_text;
    dict_get_string_optional(well, "axis_id", &axis_id_text);
    if (axis_id_text.isEmpty()) {
      axis_id_text = QUuid::createUuid().toString(QUuid::WithoutBraces);
    }
    const auto axis_id = parse_id(axis_id_text, "axis_id");
    if (!axis_id) {
      return nullptr;
    }

    const auto depth_unit_utf8 = depth_unit.toUtf8();
    const auto first_depth = depth_buffer->buffer.value_as_double(0).value();
    const auto last_depth =
        depth_buffer->buffer
            .value_as_double(depth_buffer->buffer.length() - 1)
            .value();

    WellLogDocumentBuilder builder(*document_id, DocumentRevision{1});
    builder.add_sampling_axis(SamplingAxis{
        .id = *axis_id,
        .coordinates = depth_buffer->buffer,
        .domain = DepthDomain::measured_depth,
        .unit = depth_unit_utf8.constData(),
        .direction = last_depth < first_depth ? AxisDirection::decreasing
                                              : AxisDirection::increasing,
    });

    struct CurveBuf {
      EntityId id;
      QString value_unit;
      AdaptedBuffer values;
    };
    std::vector<CurveBuf> curve_bufs;
    std::unordered_map<std::string, std::size_t> curve_index;

    if (multi_track) {
      for (Py_ssize_t ci = 0; ci < PyList_Size(curves_obj); ++ci) {
        auto *curve = PyList_GetItem(curves_obj, ci);
        if (curve == nullptr || !PyDict_Check(curve)) {
          set_welllog_error("WellLogValidationError", "invalid_document",
                            "each curve must be a dict");
          return nullptr;
        }
        QString curve_id_text;
        QString mnemonic;
        QString value_unit;
        if (!dict_get_string(curve, "curve_id", &curve_id_text) ||
            !dict_get_string(curve, "mnemonic", &mnemonic) ||
            !dict_get_string(curve, "value_unit", &value_unit)) {
          set_welllog_error("WellLogValidationError", "invalid_document",
                            "curve needs curve_id, mnemonic, value_unit");
          return nullptr;
        }
        if (value_unit.isEmpty()) {
          value_unit = QStringLiteral("unit");
        }
        auto *values_obj = PyDict_GetItemString(curve, "values");
        if (values_obj == nullptr) {
          set_welllog_error("WellLogValidationError", "invalid_buffer",
                            "curve.values is required");
          return nullptr;
        }
        const auto curve_id = parse_id(curve_id_text, "curve_id");
        if (!curve_id) {
          return nullptr;
        }
        const auto mnemonic_utf8 = mnemonic.toUtf8();
        const auto value_unit_utf8 = value_unit.toUtf8();
        auto value_buffer = add_curve_with_optional_axis(
            builder, curve, values_obj, *curve_id, mnemonic_utf8.constData(),
            value_unit_utf8.constData(), depth_buffer->buffer, *axis_id,
            depth_unit_utf8.constData());
        if (!value_buffer) {
          return nullptr;
        }
        curve_index[curve_id->to_string()] = curve_bufs.size();
        curve_bufs.push_back(CurveBuf{*curve_id, value_unit,
                                      std::move(*value_buffer)});
      }
    } else {
      // Legacy single-curve well (#170)
      QString curve_id_text;
      QString mnemonic;
      QString value_unit;
      if (!dict_get_string(well, "curve_id", &curve_id_text) ||
          !dict_get_string(well, "mnemonic", &mnemonic) ||
          !dict_get_string(well, "value_unit", &value_unit)) {
        set_welllog_error("WellLogValidationError", "invalid_document",
                          "legacy well needs curve_id, mnemonic, value_unit "
                          "(or multi-track curves/tracks)");
        return nullptr;
      }
      auto *values_obj = PyDict_GetItemString(well, "values");
      if (values_obj == nullptr) {
        set_welllog_error("WellLogValidationError", "invalid_buffer",
                          "well.values is required for legacy wells");
        return nullptr;
      }
      const auto curve_id = parse_id(curve_id_text, "curve_id");
      if (!curve_id) {
        return nullptr;
      }
      auto value_buffer = adapt_buffer(values_obj, "values");
      if (!value_buffer) {
        return nullptr;
      }
      if (value_buffer->buffer.length() != depth_buffer->buffer.length()) {
        set_welllog_error("WellLogValidationError", "length_mismatch",
                          "depth and values must have the same length");
        return nullptr;
      }
      const auto mnemonic_utf8 = mnemonic.toUtf8();
      const auto value_unit_utf8 = value_unit.toUtf8();
      builder.add_curve(Curve{
          .id = *curve_id,
          .mnemonic = mnemonic_utf8.constData(),
          .display_name = mnemonic_utf8.constData(),
          .unit = value_unit_utf8.constData(),
          .sampling_axis_id = *axis_id,
          .values = value_buffer->buffer,
          .nulls = {},
      });
      curve_index[curve_id->to_string()] = 0;
      curve_bufs.push_back(
          CurveBuf{*curve_id, value_unit, std::move(*value_buffer)});
    }

    // Optional markers list: [{id, depth, label}, ...]
    auto *markers_obj = PyDict_GetItemString(well, "markers");
    if (markers_obj != nullptr && PyList_Check(markers_obj)) {
      const auto marker_count = PyList_Size(markers_obj);
      for (Py_ssize_t mi = 0; mi < marker_count; ++mi) {
        auto *marker = PyList_GetItem(markers_obj, mi);
        if (marker == nullptr || !PyDict_Check(marker)) {
          continue;
        }
        QString marker_id_text;
        double marker_depth = 0.0;
        QString label;
        if (!dict_get_string(marker, "id", &marker_id_text) ||
            !dict_get_float(marker, "depth", &marker_depth)) {
          continue;
        }
        dict_get_string_optional(marker, "label", &label);
        const auto marker_id = parse_id(marker_id_text, "marker_id");
        if (!marker_id) {
          PyErr_Clear();
          continue;
        }
        const auto label_utf8 = label.toUtf8();
        builder.add_marker(Marker{
            .id = *marker_id,
            .reference_depth = marker_depth,
            .semantic = MarkerSemantic::formation_top,
            .label = label_utf8.constData(),
        });
      }
    }
    auto built = builder.build();
    if (built.id().is_nil()) {
      set_welllog_error("WellLogError", "resource_exhausted",
                        "document build failed");
      return nullptr;
    }
    const auto doc_result =
        view->session().execute(SetDocumentCommand{std::move(built)});
    if (!doc_result.has_value()) {
      set_result_error(doc_result.error(), "document submission");
      return nullptr;
    }

    double well_width_mm = multi_track ? 80.0 : 30.0;
    dict_get_float_optional(well, "width_mm", &well_width_mm);

    ScenePresentationBuilder presentation_builder(
        *document_id,
        ReferenceDepthRange{
            .domain = DepthDomain::measured_depth,
            .unit = depth_unit_utf8.constData(),
            .top = shared_top,
            .bottom = shared_bottom,
        },
        Millimetres{well_width_mm}, "welllog-python-multi-well");
    // Optional per-well transform points before build. Both historical keys
    // are accepted: "transform_points" (multi-well) and "depth_transform"
    // (single-well) — the Desktop host submits "depth_transform" uniformly.
    auto *xform_obj = PyDict_GetItemString(well, "transform_points");
    if (xform_obj == nullptr) {
      xform_obj = PyDict_GetItemString(well, "depth_transform");
    }
    DepthTransform transform{};
    if (xform_obj != nullptr && PyList_Check(xform_obj)) {
      const auto npts = PyList_Size(xform_obj);
      for (Py_ssize_t pi = 0; pi < npts; ++pi) {
        auto *pt = PyList_GetItem(xform_obj, pi);
        if (pt == nullptr || !PyDict_Check(pt)) {
          continue;
        }
        double ref = 0.0;
        double disp = 0.0;
        if (!dict_get_float(pt, "reference", &ref) ||
            !dict_get_float(pt, "display", &disp)) {
          continue;
        }
        transform.control_points.push_back(DepthControlPoint{
            .reference_depth = ref,
            .display_depth = disp,
        });
      }
      if (!transform.control_points.empty()) {
        transform.version = 1;
        presentation_builder.set_depth_transform(transform);
      }
    }

    EntityId first_track_id{};
    int track_z = 0;
    int tracks_added = 0;

    if (multi_track) {
      for (Py_ssize_t ti = 0; ti < PyList_Size(tracks_obj); ++ti) {
        auto *track = PyList_GetItem(tracks_obj, ti);
        if (track == nullptr || !PyDict_Check(track)) {
          continue;
        }
        auto *layers_obj = PyDict_GetItemString(track, "layers");
        if (layers_obj == nullptr || !PyList_Check(layers_obj) ||
            PyList_Size(layers_obj) <= 0) {
          continue;
        }
        double width_mm = 40.0;
        dict_get_float_optional(track, "width_mm", &width_mm);
        if (width_mm < 5.0) {
          width_mm = 5.0;
        }
        const auto track_role =
            std::string{"welllog-python/mw-track/"} + std::to_string(ti);
        const auto track_id = derive_presentation_id(
            *document_id, track_role, {*document_id, *axis_id});
        if (first_track_id.is_nil()) {
          first_track_id = track_id;
        }
        presentation_builder.add_track(TrackSpec{
            .id = track_id,
            .width = Millimetres{width_mm},
            .z_order = track_z++,
            .header = TrackHeaderSpec{.height = Millimetres{6.0},
                                      .font_size = Millimetres{2.0}},
        });
        auto *first_layer = PyList_GetItem(layers_obj, 0);
        QString first_curve_id_text;
        if (first_layer != nullptr && PyDict_Check(first_layer)) {
          dict_get_string_optional(first_layer, "curve_id", &first_curve_id_text);
        }
        double scale_min = 0.0;
        double scale_max = 100.0;
        bool have_min = dict_get_float(track, "scale_min", &scale_min);
        if (!have_min) {
          PyErr_Clear();
        }
        bool have_max = dict_get_float(track, "scale_max", &scale_max);
        if (!have_max) {
          PyErr_Clear();
        }
        auto it = curve_index.find(first_curve_id_text.toStdString());
        if ((!have_min || !have_max) && it != curve_index.end()) {
          double auto_max = 1.0;
          const double auto_min =
              buffer_min_max(curve_bufs[it->second].values.buffer, &auto_max);
          if (!have_min) {
            scale_min = auto_min;
          }
          if (!have_max) {
            scale_max = auto_max;
          }
        }
        if (!(scale_max > scale_min)) {
          scale_max = scale_min + 1.0;
        }
        QString scale_mode_text;
        dict_get_string_optional(track, "scale_mode", &scale_mode_text);
        const ScaleMode scale_mode =
            scale_mode_text.compare(QStringLiteral("log"), Qt::CaseInsensitive) ==
                    0
                ? ScaleMode::logarithmic
                : ScaleMode::linear;
        QString scale_unit = QStringLiteral("unit");
        if (it != curve_index.end()) {
          scale_unit = curve_bufs[it->second].value_unit;
        }
        const auto scale_role =
            std::string{"welllog-python/mw-scale/"} + std::to_string(ti);
        const auto scale_id = derive_presentation_id(
            *document_id, scale_role, {*document_id, *axis_id, track_id});
        const auto scale_unit_utf8 = scale_unit.toUtf8();
        presentation_builder.add_scale(TrackScaleSpec{
            .id = scale_id,
            .track_id = track_id,
            .mode = scale_mode,
            .minimum = scale_min,
            .maximum = scale_max,
            .direction = ScaleDirection::left_to_right,
            .unit = scale_unit_utf8.constData(),
        });
        for (Py_ssize_t li = 0; li < PyList_Size(layers_obj); ++li) {
          auto *layer = PyList_GetItem(layers_obj, li);
          if (layer == nullptr || !PyDict_Check(layer)) {
            continue;
          }
          QString layer_curve_id_text;
          if (!dict_get_string(layer, "curve_id", &layer_curve_id_text)) {
            continue;
          }
          const auto layer_curve_id = parse_id(layer_curve_id_text, "curve_id");
          if (!layer_curve_id) {
            PyErr_Clear();
            continue;
          }
          if (curve_index.find(layer_curve_id->to_string()) ==
              curve_index.end()) {
            continue;
          }
          QString color_text;
          dict_get_string_optional(layer, "color", &color_text);
          const auto color = parse_hex_color(
              color_text,
              RgbaColor{.red = 0x19, .green = 0x72, .blue = 0xb8, .alpha = 0xff});
          const auto layer_role = std::string{"welllog-python/mw-layer/"} +
                                  std::to_string(ti) + "/" +
                                  std::to_string(li);
          const auto layer_id = derive_presentation_id(
              *document_id, layer_role,
              {*document_id, *axis_id, track_id, scale_id, *layer_curve_id});
          presentation_builder.add_curve_layer(CurveLayerSpec{
              .id = layer_id,
              .track_id = track_id,
              .curve_id = *layer_curve_id,
              .scale_id = scale_id,
              .color = color,
              .line_width = Millimetres{0.3},
              .z_order = static_cast<std::int32_t>(li),
              .visible = true,
          });
        }
        ++tracks_added;
      }
    } else {
      // Legacy single track
      const auto &cb = curve_bufs[0];
      double minimum = 0.0;
      double maximum = 1.0;
      minimum = buffer_min_max(cb.values.buffer, &maximum);
      double width_mm = well_width_mm;
      dict_get_float_optional(well, "width_mm", &width_mm);
      const auto track_id = derive_presentation_id(
          *document_id, "welllog-python/multi-track",
          {*document_id, *axis_id, cb.id});
      first_track_id = track_id;
      const auto scale_id = derive_presentation_id(
          *document_id, "welllog-python/multi-scale",
          {*document_id, *axis_id, cb.id, track_id});
      const auto layer_id = derive_presentation_id(
          *document_id, "welllog-python/multi-layer",
          {*document_id, *axis_id, cb.id, track_id, scale_id});
      const auto value_unit_utf8 = cb.value_unit.toUtf8();
      presentation_builder.add_track(TrackSpec{
          .id = track_id, .width = Millimetres{width_mm}, .z_order = 0});
      presentation_builder.add_scale(TrackScaleSpec{
          .id = scale_id,
          .track_id = track_id,
          .mode = ScaleMode::linear,
          .minimum = minimum,
          .maximum = maximum,
          .direction = ScaleDirection::left_to_right,
          .unit = value_unit_utf8.constData(),
      });
      presentation_builder.add_curve_layer(CurveLayerSpec{
          .id = layer_id,
          .track_id = track_id,
          .curve_id = cb.id,
          .scale_id = scale_id,
          .color =
              RgbaColor{.red = 0x19, .green = 0x72, .blue = 0xb8, .alpha = 0xff},
          .line_width = Millimetres{0.35},
          .z_order = 0,
          .visible = true,
      });
      tracks_added = 1;
    }

    if (tracks_added < 1) {
      set_welllog_error("WellLogValidationError", "invalid_presentation",
                        "well has no drawable tracks");
      return nullptr;
    }

    if (markers_obj != nullptr && PyList_Check(markers_obj) &&
        PyList_Size(markers_obj) > 0 && !first_track_id.is_nil()) {
      bool marker_symbols = false;
      auto *symbols_flag = PyDict_GetItemString(well, "marker_symbols");
      if (symbols_flag != nullptr && PyObject_IsTrue(symbols_flag) == 1) {
        marker_symbols = true;
      }
      PyErr_Clear();
      const auto marker_layer_id = derive_presentation_id(
          *document_id, "welllog-python/multi-marker-layer",
          {*document_id, *axis_id, first_track_id});
      presentation_builder.add_marker_layer(MarkerLayerSpec{
          .id = marker_layer_id,
          .track_id = first_track_id,
          .z_order = 50,
          .line_color = RgbaColor{200, 40, 40, 255},
          .line_width = Millimetres{0.4},
          .draw_labels = false,
          .draw_symbols = marker_symbols,
      });
    }
    auto presentation = presentation_builder.build();
    if (presentation.document_id().is_nil()) {
      set_welllog_error("WellLogError", "resource_exhausted",
                        "presentation build failed");
      return nullptr;
    }
    const auto pres_result = view->session().execute(
        SetPresentationCommand{std::move(presentation)});
    if (!pres_result.has_value()) {
      set_result_error(pres_result.error(), "presentation preparation");
      return nullptr;
    }
    if (!transform.control_points.empty()) {
      const auto tr = view->session().execute(SetDepthTransformCommand{
          .document_id = *document_id,
          .transform = transform,
      });
      if (!tr.has_value()) {
        set_result_error(tr.error(), "depth transform");
        return nullptr;
      }
    }
    document_ids.push_back(*document_id);
    if (first_document.is_nil()) {
      first_document = *document_id;
    }
  }

  std::vector<WellPlacement> placements;
  placements.reserve(document_ids.size());
  for (const auto &id : document_ids) {
    placements.push_back(WellPlacement{.document_id = id});
  }
  const auto layout_result = view->session().execute(SetWellLayoutCommand{
      .wells = std::move(placements),
      .gap = Millimetres{gap_mm},
      .pack_left_to_right = true,
  });
  if (!layout_result.has_value()) {
    set_result_error(layout_result.error(), "well layout");
    return nullptr;
  }
  const auto shared_result =
      view->session().execute(SetSharedDepthViewportCommand{
          .viewport = DepthViewport{.top = shared_top, .bottom = shared_bottom},
          .pixel_height = 200,
      });
  if (!shared_result.has_value()) {
    set_result_error(shared_result.error(), "shared viewport");
    return nullptr;
  }

  // Overlays
  auto *overlays_obj = PyDict_GetItemString(payload, "overlays");
  if (overlays_obj != nullptr && PyList_Check(overlays_obj)) {
    std::vector<CrossWellOverlay> overlays;
    const auto oc = PyList_Size(overlays_obj);
    for (Py_ssize_t oi = 0; oi < oc; ++oi) {
      auto *item = PyList_GetItem(overlays_obj, oi);
      if (item == nullptr || !PyDict_Check(item)) {
        continue;
      }
      QString id_t, left_d, right_d, left_m, right_m, left_b, right_b, kind;
      if (!dict_get_string(item, "id", &id_t) ||
          !dict_get_string(item, "left_document_id", &left_d) ||
          !dict_get_string(item, "right_document_id", &right_d) ||
          !dict_get_string(item, "left_marker_id", &left_m) ||
          !dict_get_string(item, "right_marker_id", &right_m)) {
        continue;
      }
      dict_get_string_optional(item, "kind", &kind);
      dict_get_string_optional(item, "left_bottom_marker_id", &left_b);
      dict_get_string_optional(item, "right_bottom_marker_id", &right_b);
      const auto oid = parse_id(id_t, "overlay_id");
      const auto ld = parse_id(left_d, "left_document_id");
      const auto rd = parse_id(right_d, "right_document_id");
      const auto lm = parse_id(left_m, "left_marker_id");
      const auto rm = parse_id(right_m, "right_marker_id");
      if (!oid || !ld || !rd || !lm || !rm) {
        PyErr_Clear();
        continue;
      }
      CrossWellOverlay overlay{
          .id = *oid,
          .kind = kind == QStringLiteral("correlation_band")
                      ? CrossWellOverlay::Kind::correlation_band
                      : CrossWellOverlay::Kind::horizon_line,
          .left_document_id = *ld,
          .right_document_id = *rd,
          .left_marker_id = *lm,
          .right_marker_id = *rm,
      };
      if (overlay.kind == CrossWellOverlay::Kind::correlation_band) {
        const auto lb = parse_id(left_b, "left_bottom_marker_id");
        const auto rb = parse_id(right_b, "right_bottom_marker_id");
        if (!lb || !rb) {
          PyErr_Clear();
          continue;
        }
        overlay.left_bottom_marker_id = *lb;
        overlay.right_bottom_marker_id = *rb;
      }
      overlays.push_back(overlay);
    }
    if (!overlays.empty()) {
      const auto ov = view->session().execute(
          SetCrossWellOverlaysCommand{.overlays = std::move(overlays)});
      if (!ov.has_value()) {
        set_result_error(ov.error(), "cross-well overlays");
        return nullptr;
      }
    }
  }

  view->set_document_id(first_document);
  auto *report = PyDict_New();
  if (report == nullptr) {
    return nullptr;
  }
  auto *ids = PyList_New(static_cast<Py_ssize_t>(document_ids.size()));
  if (ids == nullptr) {
    Py_DECREF(report);
    return nullptr;
  }
  for (std::size_t i = 0; i < document_ids.size(); ++i) {
    auto *s = PyUnicode_FromString(document_ids[i].to_string().c_str());
    if (s == nullptr ||
        PyList_SetItem(ids, static_cast<Py_ssize_t>(i), s) != 0) {
      Py_XDECREF(s);
      Py_DECREF(ids);
      Py_DECREF(report);
      return nullptr;
    }
  }
  if (PyDict_SetItemString(report, "document_ids", ids) != 0 ||
      PyDict_SetItemString(report, "well_count",
                           PyLong_FromLong(static_cast<long>(document_ids.size()))) !=
          0 ||
      PyDict_SetItemString(report, "render_prepared", Py_True) != 0) {
    Py_DECREF(ids);
    Py_DECREF(report);
    return nullptr;
  }
  Py_DECREF(ids);
  return report;
}

[[nodiscard]] PyObject *clear_multi_well_section_impl(WellLogView *view) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "clear_multi_well_section must run on the Qt GUI thread");
    return nullptr;
  }
  static_cast<void>(
      view->session().execute(SetCrossWellOverlaysCommand{.overlays = {}}));
  static_cast<void>(view->session().execute(ClearWellLayoutCommand{}));
  view->set_document_id(EntityId{});
  Py_RETURN_NONE;
}

// SVG export of a single-well prepared scene (T1 / #273). The engine
// renders to an in-memory SvgDocument; we copy its text out as PyBytes so
// the host controls filesystem writes (atomic save, cancellation).
// When ``export_pixel_height > 0`` the scene is re-prepared at that
// aggregate density (T3 / #275) so fixed-page pagination resolves the
// correct per-page curve detail.
[[nodiscard]] PyObject *
export_scene_svg_impl(WellLogView *view, const QString &document_id_text,
                      std::uint64_t export_pixel_height) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "SVG export must run on the Qt GUI thread");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  // Acquire the prepared scene for this document. A null shared_ptr means
  // the document has no prepared scene yet (not submitted / not rendered).
  auto scene = view->session().prepared_scene(*document_id);
  if (scene == nullptr) {
    set_welllog_error("WellLogValidationError", "document_not_found",
                      "no prepared scene for the given document_id");
    return nullptr;
  }
  // Optional export-density re-prepare (T3 / #275). The host computes the
  // target height via PaginatedSvgExporter::required_aggregate_pixel_height
  // and passes it here; we re-prepare without disturbing the interactive
  // scene.
  std::shared_ptr<const PreparedScene> export_scene = scene;
  if (export_pixel_height > 0) {
    auto reprepared =
        view->session().prepare_for_export(*document_id, export_pixel_height);
    if (!reprepared.has_value()) {
      set_result_error(reprepared.error(), "export-density prepare");
      return nullptr;
    }
    export_scene =
        std::make_shared<const PreparedScene>(std::move(reprepared).value());
  }
  const auto result = SvgExporter::write(*export_scene);
  if (!result.has_value()) {
    set_result_error(result.error(), "SVG export");
    return nullptr;
  }
  const auto svg = result.value().text();
  return PyBytes_FromStringAndSize(svg.data(),
                                   static_cast<Py_ssize_t>(svg.size()));
}

// PDF export of a single-well prepared scene (T2 / #274). Mirrors the SVG
// path but PdfSceneExporter needs an ExportSnapshot (carries depth
// transform, font fingerprint, pattern versions, page spec) and an
// optional TextEngine (for pagination metadata text; without it the
// metadata band text is omitted — geometry bands still render).
[[nodiscard]] PyObject *
export_scene_pdf_impl(WellLogView *view, const QString &document_id_text,
                      std::uint64_t export_pixel_height,
                      bool searchable_text, bool crop_marks,
                      bool layered_pdf, bool show_depth_ruler) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "PDF export must run on the Qt GUI thread");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  const auto scene = view->session().prepared_scene(*document_id);
  if (scene == nullptr) {
    set_welllog_error("WellLogValidationError", "document_not_found",
                      "no prepared scene for the given document_id");
    return nullptr;
  }
  // Optional export-density re-prepare (T3 / #275), mirroring the SVG path.
  std::shared_ptr<const PreparedScene> export_scene = scene;
  if (export_pixel_height > 0) {
    auto reprepared =
        view->session().prepare_for_export(*document_id, export_pixel_height);
    if (!reprepared.has_value()) {
      set_result_error(reprepared.error(), "export-density prepare");
      return nullptr;
    }
    export_scene =
        std::make_shared<const PreparedScene>(std::move(reprepared).value());
  }
  // Build a continuous-mode snapshot from the scene's own metadata so the
  // export is reproducible without the host supplying extra fields.
  ExportSnapshot snapshot{
      .document_id = export_scene->document_id(),
      .document_revision = export_scene->document_revision(),
      .presentation_version = export_scene->presentation_version(),
      .depth_transform = export_scene->depth_transform(),
      .font_asset_fingerprint =
          std::string{export_scene->font_asset_fingerprint()},
      .pattern_versions = {},
      .page = ExportPageSpec{
          .mode = PaginationMode::continuous,
          .page_width = export_scene->physical_width(),
          .page_height = export_scene->physical_height(),
          .show_depth_ruler = show_depth_ruler,
          .crop_marks = crop_marks,
          .layered_pdf = layered_pdf,
      },
  };
  snapshot.pattern_versions.reserve(export_scene->patterns().size());
  for (const auto &pattern : export_scene->patterns()) {
    snapshot.pattern_versions.push_back(pattern.version);
  }
  // Pass the session's installed text engine (if any) so pagination
  // metadata band text renders; geometry bands always render regardless.
  // Hold the shared_ptr for the write's duration so a concurrent
  // set_text_engine cannot free the engine mid-export (review D-001).
  const auto text_engine = view->session().text_engine();
  const auto result = PdfSceneExporter::write(
      *export_scene, snapshot, {}, text_engine.get(), nullptr, searchable_text);
  if (!result.has_value()) {
    set_result_error(result.error(), "PDF export");
    return nullptr;
  }
  const auto pdf = result.value().bytes();
  return PyBytes_FromStringAndSize(pdf.data(),
                                   static_cast<Py_ssize_t>(pdf.size()));
}

[[nodiscard]] PyObject *
export_scene_cgm_impl(WellLogView *view, const QString &document_id_text,
                      double page_height_mm) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "CGM export must run on the Qt GUI thread");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  const auto scene = view->session().prepared_scene(*document_id);
  if (scene == nullptr) {
    set_welllog_error("WellLogValidationError", "document_not_found",
                      "no prepared scene for the given document_id");
    return nullptr;
  }
  CgmExportDiagnostics diag;
  CgmExportOptions opt{};
  if (page_height_mm > 0.0 && std::isfinite(page_height_mm)) {
    opt.page_height_mm = page_height_mm;
  }
  const auto result = CgmSceneExporter::write(*scene, opt, &diag);
  if (!result.has_value()) {
    set_result_error(result.error(), "CGM export");
    return nullptr;
  }
  const auto cgm = result.value().bytes();
  return PyBytes_FromStringAndSize(cgm.data(),
                                   static_cast<Py_ssize_t>(cgm.size()));
}

} // namespace

PyObject *submit_multi_track(WellLogView *view, PyObject *payload) noexcept {
  try {
    return submit_multi_track_impl(view, payload);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (const std::exception &exc) {
    set_welllog_error("WellLogError", "internal_error", exc.what());
    return nullptr;
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during multi-track submission");
    return nullptr;
  }
}

PyObject *append_curves(WellLogView *view, PyObject *payload) noexcept {
  try {
    return append_curves_impl(view, payload);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (const std::exception &exc) {
    set_welllog_error("WellLogError", "internal_error", exc.what());
    return nullptr;
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during curve append");
    return nullptr;
  }
}

PyObject *patch_document(WellLogView *view, PyObject *payload) noexcept {
  try {
    return patch_document_impl(view, payload);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (const std::exception &exc) {
    set_welllog_error("WellLogError", "internal_error", exc.what());
    return nullptr;
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during document patch");
    return nullptr;
  }
}

PyObject *document_metrics(WellLogView *view,
                           const QString &document_id) noexcept {
  try {
    return document_metrics_impl(view, document_id);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (const std::exception &exc) {
    set_welllog_error("WellLogError", "internal_error", exc.what());
    return nullptr;
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure reading document metrics");
    return nullptr;
  }
}

PyObject *poll_session(WellLogView *view) noexcept {
  try {
    if (view == nullptr) {
      set_welllog_error("WellLogValidationError", "invalid_view",
                        "WellLogView is no longer valid");
      return nullptr;
    }
    if (QThread::currentThread() != view->thread()) {
      set_welllog_error("WellLogThreadError", "thread_violation",
                        "session polling must run on the Qt GUI thread");
      return nullptr;
    }
    view->session().poll_async();
    Py_RETURN_NONE;
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (const std::exception &exc) {
    set_welllog_error("WellLogError", "internal_error", exc.what());
    return nullptr;
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure polling session");
    return nullptr;
  }
}

PyObject *submit_multi_well_section(WellLogView *view,
                                    PyObject *payload) noexcept {
  try {
    return submit_multi_well_section_impl(view, payload);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during multi-well submission");
    return nullptr;
  }
}

PyObject *clear_multi_well_section(WellLogView *view) noexcept {
  try {
    return clear_multi_well_section_impl(view);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure clearing multi-well section");
    return nullptr;
  }
}

PyObject *export_scene_svg(WellLogView *view, const QString &document_id,
                           std::uint64_t export_pixel_height) noexcept {
  try {
    return export_scene_svg_impl(view, document_id, export_pixel_height);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during SVG export");
    return nullptr;
  }
}

PyObject *export_scene_pdf(WellLogView *view, const QString &document_id,
                           std::uint64_t export_pixel_height,
                           bool searchable_text, bool crop_marks,
                           bool layered_pdf, bool show_depth_ruler) noexcept {
  try {
    return export_scene_pdf_impl(view, document_id, export_pixel_height,
                                 searchable_text, crop_marks, layered_pdf,
                                 show_depth_ruler);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during PDF export");
    return nullptr;
  }
}

PyObject *export_scene_cgm(WellLogView *view, const QString &document_id,
                           double page_height_mm) noexcept {
  try {
    return export_scene_cgm_impl(view, document_id, page_height_mm);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during CGM export");
    return nullptr;
  }
}


// --- Track/Data workflow commands + hover/selection introspection ----------
//
// apply_track_command maps one payload dict to one C++ track command
// (track_commands.hpp). Ids for created entities are generated HERE (QUuid)
// and passed explicitly so the report can return them — the host addresses
// later edits with them.

namespace {

bool dict_get_bool_optional(PyObject *dict, const char *key, bool *out) {
  auto *item = PyDict_GetItemString(dict, key);
  if (item == nullptr) {
    return false;
  }
  const auto truth = PyObject_IsTrue(item);
  if (truth < 0) {
    PyErr_Clear();
    return false;
  }
  *out = truth != 0;
  return true;
}

[[nodiscard]] std::optional<std::vector<EntityId>>
dict_get_id_list(PyObject *dict, const char *key) {
  auto *item = PyDict_GetItemString(dict, key);
  if (item == nullptr || !PyList_Check(item)) {
    return std::nullopt;
  }
  std::vector<EntityId> ids;
  const auto count = PyList_Size(item);
  ids.reserve(static_cast<std::size_t>(count));
  for (Py_ssize_t index = 0; index < count; ++index) {
    auto *entry = PyList_GetItem(item, index);
    QString text;
    if (entry == nullptr || !PyUnicode_Check(entry)) {
      continue;
    }
    Py_ssize_t size = 0;
    const char *utf8 = PyUnicode_AsUTF8AndSize(entry, &size);
    if (utf8 == nullptr) {
      PyErr_Clear();
      continue;
    }
    text = QString::fromUtf8(utf8, static_cast<qsizetype>(size));
    const auto parsed = parse_id(text, key);
    if (!parsed) {
      return std::nullopt;
    }
    ids.push_back(*parsed);
  }
  return ids;
}

[[nodiscard]] EntityId generate_bridge_id() {
  const auto text =
      QUuid::createUuid().toString(QUuid::WithoutBraces).toStdString();
  return EntityId::parse(text).value();
}

// report = {"revision": n, "state_version": n, ...optional generated ids}
[[nodiscard]] PyObject *
command_report(const CommandReceipt &receipt) {
  auto *report = PyDict_New();
  if (report == nullptr) {
    return nullptr;
  }
  auto *revision =
      PyLong_FromUnsignedLongLong(receipt.document_revision.value);
  if (revision == nullptr ||
      PyDict_SetItemString(report, "revision", revision) != 0) {
    Py_XDECREF(revision);
    Py_DECREF(report);
    return nullptr;
  }
  Py_DECREF(revision);
  auto *version = PyLong_FromUnsignedLongLong(receipt.state_version);
  if (version == nullptr ||
      PyDict_SetItemString(report, "state_version", version) != 0) {
    Py_XDECREF(version);
    Py_DECREF(report);
    return nullptr;
  }
  Py_DECREF(version);
  return report;
}

[[nodiscard]] bool report_set_id(PyObject *report, const char *key,
                                 EntityId value) {
  auto *text = PyUnicode_FromString(value.to_string().c_str());
  if (text == nullptr) {
    return false;
  }
  const auto ok = PyDict_SetItemString(report, key, text) == 0;
  Py_DECREF(text);
  return ok;
}

[[nodiscard]] PyObject *apply_track_command_impl(WellLogView *view,
                                                 PyObject *payload) {
  if (view == nullptr) {
    set_welllog_error("WellLogValidationError", "invalid_view",
                      "WellLogView is no longer valid");
    return nullptr;
  }
  if (QThread::currentThread() != view->thread()) {
    set_welllog_error("WellLogThreadError", "thread_violation",
                      "track commands must run on the Qt GUI thread");
    return nullptr;
  }
  if (payload == nullptr || !PyDict_Check(payload)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload must be a dict");
    return nullptr;
  }
  QString op;
  if (!dict_get_string(payload, "op", &op)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload.op must be a string");
    return nullptr;
  }
  QString document_id_text;
  if (!dict_get_string(payload, "document_id", &document_id_text)) {
    set_welllog_error("WellLogValidationError", "invalid_document",
                      "payload.document_id is required");
    return nullptr;
  }
  const auto document_id = parse_id(document_id_text, "document_id");
  if (!document_id) {
    return nullptr;
  }
  auto &session = view->session();

  const auto require_id_field =
      [&](const char *key, const char *role) -> std::optional<EntityId> {
    QString text;
    if (!dict_get_string(payload, key, &text)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        (std::string{"payload."} + key + " is required")
                            .c_str());
      return std::nullopt;
    }
    return parse_id(text, role);
  };
  const auto optional_id_field =
      [&](const char *key) -> EntityId {
    QString text;
    if (!dict_get_string(payload, key, &text) || text.isEmpty()) {
      return EntityId{};
    }
    return *parse_id(text, key);
  };

  if (op == QStringLiteral("add_track")) {
    double width_mm = 40.0;
    dict_get_float_optional(payload, "width_mm", &width_mm);
    bool visible = true;
    dict_get_bool_optional(payload, "visible", &visible);
    double header_height_mm = 0.0;
    dict_get_float_optional(payload, "header_height_mm", &header_height_mm);
    double header_font_size_mm = 2.5;
    dict_get_float_optional(payload, "header_font_size_mm",
                            &header_font_size_mm);
    const auto track_id = optional_id_field("track_id");
    const auto effective_track_id =
        track_id.is_nil() ? generate_bridge_id() : track_id;
    const auto result = session.execute(AddTrackCommand{
        .document_id = *document_id,
        .track_id = effective_track_id,
        .width = Millimetres{width_mm},
        .z_order = std::nullopt,
        .header = TrackHeaderSpec{.height = Millimetres{header_height_mm},
                                  .font_size =
                                      Millimetres{header_font_size_mm}},
        .visible = visible,
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "add_track");
      return nullptr;
    }
    auto *report = command_report(result.value());
    if (report == nullptr ||
        !report_set_id(report, "track_id", effective_track_id)) {
      Py_XDECREF(report);
      return nullptr;
    }
    return report;
  }
  if (op == QStringLiteral("remove_track")) {
    const auto track_id = require_id_field("track_id", "track_id");
    if (!track_id) {
      return nullptr;
    }
    const auto result = session.execute(RemoveTrackCommand{
        .document_id = *document_id, .track_id = *track_id});
    if (!result.has_value()) {
      set_result_error(result.error(), "remove_track");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("reorder_tracks")) {
    const auto ordered = dict_get_id_list(payload, "track_ids");
    if (!ordered.has_value()) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "payload.track_ids must be a list of UUIDs");
      return nullptr;
    }
    const auto result = session.execute(ReorderTracksCommand{
        .document_id = *document_id, .ordered_track_ids = *ordered});
    if (!result.has_value()) {
      set_result_error(result.error(), "reorder_tracks");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("resize_track")) {
    const auto track_id = require_id_field("track_id", "track_id");
    if (!track_id) {
      return nullptr;
    }
    double width_mm = 0.0;
    if (!dict_get_float(payload, "width_mm", &width_mm) || width_mm <= 0.0) {
      set_welllog_error("WellLogValidationError", "invalid_presentation",
                        "payload.width_mm must be a positive number");
      return nullptr;
    }
    const auto result = session.execute(ResizeTrackCommand{
        .document_id = *document_id,
        .track_id = *track_id,
        .width = Millimetres{width_mm},
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "resize_track");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("set_track_header")) {
    const auto track_id = require_id_field("track_id", "track_id");
    if (!track_id) {
      return nullptr;
    }
    double height_mm = 0.0;
    double font_size_mm = 2.5;
    dict_get_float_optional(payload, "height_mm", &height_mm);
    dict_get_float_optional(payload, "font_size_mm", &font_size_mm);
    const auto result = session.execute(SetTrackHeaderCommand{
        .document_id = *document_id,
        .track_id = *track_id,
        .header = TrackHeaderSpec{.height = Millimetres{height_mm},
                                  .font_size = Millimetres{font_size_mm}},
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "set_track_header");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("set_track_visibility")) {
    const auto track_id = require_id_field("track_id", "track_id");
    if (!track_id) {
      return nullptr;
    }
    bool visible = true;
    dict_get_bool_optional(payload, "visible", &visible);
    const auto result = session.execute(SetTrackVisibilityCommand{
        .document_id = *document_id, .track_id = *track_id,
        .visible = visible});
    if (!result.has_value()) {
      set_result_error(result.error(), "set_track_visibility");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("bind_curve")) {
    const auto curve_id = require_id_field("curve_id", "curve_id");
    if (!curve_id) {
      return nullptr;
    }
    const auto track_id = require_id_field("track_id", "track_id");
    if (!track_id) {
      return nullptr;
    }
    const auto scale_id = optional_id_field("scale_id");
    const auto layer_id_raw = optional_id_field("layer_id");
    const auto layer_id =
        layer_id_raw.is_nil() ? generate_bridge_id() : layer_id_raw;
    bool auto_range = true;
    dict_get_bool_optional(payload, "auto_range", &auto_range);
    QString color_text;
    dict_get_string_optional(payload, "color", &color_text);
    double line_width_mm = 0.35;
    dict_get_float_optional(payload, "line_width_mm", &line_width_mm);
    const auto result = session.execute(BindCurveToTrackCommand{
        .document_id = *document_id,
        .curve_id = *curve_id,
        .track_id = *track_id,
        .layer_id = layer_id,
        .scale_id = scale_id,
        .auto_range = auto_range,
        .color = parse_hex_color(
            color_text,
            RgbaColor{.red = 0x1F, .green = 0x72, .blue = 0xB8,
                      .alpha = 0xFF}),
        .line_width = Millimetres{line_width_mm},
        .z_order = std::nullopt,
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "bind_curve");
      return nullptr;
    }
    auto *report = command_report(result.value());
    if (report == nullptr || !report_set_id(report, "layer_id", layer_id)) {
      Py_XDECREF(report);
      return nullptr;
    }
    if (!scale_id.is_nil()) {
      if (!report_set_id(report, "scale_id", scale_id)) {
        Py_DECREF(report);
        return nullptr;
      }
    }
    return report;
  }
  if (op == QStringLiteral("unbind_curve")) {
    const auto layer_id = require_id_field("layer_id", "layer_id");
    if (!layer_id) {
      return nullptr;
    }
    const auto result = session.execute(UnbindCurveFromTrackCommand{
        .document_id = *document_id, .layer_id = *layer_id});
    if (!result.has_value()) {
      set_result_error(result.error(), "unbind_curve");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("move_curve_layer")) {
    const auto layer_id = require_id_field("layer_id", "layer_id");
    if (!layer_id) {
      return nullptr;
    }
    const auto target_track_id =
        require_id_field("target_track_id", "target_track_id");
    if (!target_track_id) {
      return nullptr;
    }
    const auto result = session.execute(MoveCurveLayerCommand{
        .document_id = *document_id,
        .layer_id = *layer_id,
        .target_track_id = *target_track_id,
        .target_scale_id = optional_id_field("target_scale_id"),
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "move_curve_layer");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("duplicate_curve_layer")) {
    const auto layer_id = require_id_field("layer_id", "layer_id");
    if (!layer_id) {
      return nullptr;
    }
    const auto new_layer_raw = optional_id_field("new_layer_id");
    const auto new_layer_id =
        new_layer_raw.is_nil() ? generate_bridge_id() : new_layer_raw;
    const auto result = session.execute(DuplicateCurveLayerCommand{
        .document_id = *document_id,
        .layer_id = *layer_id,
        .new_layer_id = new_layer_id,
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "duplicate_curve_layer");
      return nullptr;
    }
    auto *report = command_report(result.value());
    if (report == nullptr ||
        !report_set_id(report, "new_layer_id", new_layer_id)) {
      Py_XDECREF(report);
      return nullptr;
    }
    return report;
  }
  if (op == QStringLiteral("reorder_curve_layers")) {
    const auto track_id = require_id_field("track_id", "track_id");
    if (!track_id) {
      return nullptr;
    }
    const auto ordered = dict_get_id_list(payload, "layer_ids");
    if (!ordered.has_value()) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "payload.layer_ids must be a list of UUIDs");
      return nullptr;
    }
    const auto result = session.execute(ReorderCurveLayersCommand{
        .document_id = *document_id,
        .track_id = *track_id,
        .ordered_layer_ids = *ordered,
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "reorder_curve_layers");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("set_layer_visibility")) {
    const auto layer_id = require_id_field("layer_id", "layer_id");
    if (!layer_id) {
      return nullptr;
    }
    bool visible = true;
    dict_get_bool_optional(payload, "visible", &visible);
    const auto result = session.execute(SetCurveLayerVisibilityCommand{
        .document_id = *document_id, .layer_id = *layer_id,
        .visible = visible});
    if (!result.has_value()) {
      set_result_error(result.error(), "set_layer_visibility");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("set_layer_style")) {
    const auto layer_id = require_id_field("layer_id", "layer_id");
    if (!layer_id) {
      return nullptr;
    }
    QString color_text;
    dict_get_string_optional(payload, "color", &color_text);
    const bool has_color = !color_text.isEmpty();
    double line_width_mm = 0.0;
    const bool has_width =
        dict_get_float(payload, "line_width_mm", &line_width_mm) &&
        line_width_mm > 0.0;
    const auto result = session.execute(SetCurveLayerStyleCommand{
        .document_id = *document_id,
        .layer_id = *layer_id,
        .color =
            has_color
                ? std::optional<RgbaColor>{parse_hex_color(
                      color_text,
                      RgbaColor{.red = 0, .green = 0, .blue = 0, .alpha = 255})}
                : std::optional<RgbaColor>{},
        .line_width = has_width
                          ? std::optional<Millimetres>{Millimetres{
                                line_width_mm}}
                          : std::optional<Millimetres>{},
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "set_layer_style");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("set_scale")) {
    const auto scale_id = require_id_field("scale_id", "scale_id");
    if (!scale_id) {
      return nullptr;
    }
    QString mode_text;
    dict_get_string_optional(payload, "mode", &mode_text);
    std::optional<ScaleMode> mode;
    if (!mode_text.isEmpty()) {
      mode = mode_text == QStringLiteral("logarithmic")
                 ? ScaleMode::logarithmic
                 : ScaleMode::linear;
    }
    QString direction_text;
    dict_get_string_optional(payload, "direction", &direction_text);
    std::optional<ScaleDirection> direction;
    if (!direction_text.isEmpty()) {
      direction = direction_text == QStringLiteral("right_to_left")
                      ? ScaleDirection::right_to_left
                      : ScaleDirection::left_to_right;
    }
    double minimum = 0.0;
    double maximum = 0.0;
    const auto has_minimum =
        dict_get_float(payload, "minimum", &minimum) && std::isfinite(minimum);
    const auto has_maximum =
        dict_get_float(payload, "maximum", &maximum) && std::isfinite(maximum);
    QString unit_text;
    dict_get_string_optional(payload, "unit", &unit_text);
    std::optional<std::string> unit;
    if (!unit_text.isEmpty()) {
      unit = unit_text.toStdString();
    }
    const auto result = session.execute(SetTrackScaleCommand{
        .document_id = *document_id,
        .scale_id = *scale_id,
        .mode = mode,
        .minimum = has_minimum ? std::optional<double>{minimum}
                               : std::optional<double>{},
        .maximum = has_maximum ? std::optional<double>{maximum}
                               : std::optional<double>{},
        .direction = direction,
        .unit = unit,
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "set_scale");
      return nullptr;
    }
    return command_report(result.value());
  }
  if (op == QStringLiteral("auto_range_scale")) {
    const auto scale_id = require_id_field("scale_id", "scale_id");
    if (!scale_id) {
      return nullptr;
    }
    const auto result = session.execute(AutoRangeTrackScaleCommand{
        .document_id = *document_id, .scale_id = *scale_id});
    if (!result.has_value()) {
      set_result_error(result.error(), "auto_range_scale");
      return nullptr;
    }
    return command_report(result.value());
  }
  set_welllog_error("WellLogValidationError", "invalid_document",
                    (std::string{"unknown track command op: "} +
                     op.toStdString())
                        .c_str());
  return nullptr;
}

[[nodiscard]] PyObject *hover_info_impl(WellLogView *view) {
  const auto pick = view->hover_pick();
  if (!pick.has_value()) {
    Py_RETURN_NONE;
  }
  const auto &session = view->session();
  const auto document = session.document(pick->document_id);
  const auto *presentation = session.presentation(pick->document_id);
  if (document == nullptr || presentation == nullptr) {
    Py_RETURN_NONE;
  }
  const auto info =
      resolve_curve_pick(*document, *presentation, *pick);
  if (!info.has_value()) {
    Py_RETURN_NONE;
  }
  auto *dict = PyDict_New();
  if (dict == nullptr) {
    return nullptr;
  }
  const auto set_str = [&](const char *key, std::string_view value) {
    auto *text = PyUnicode_FromStringAndSize(
        value.data(), static_cast<Py_ssize_t>(value.size()));
    if (text == nullptr || PyDict_SetItemString(dict, key, text) != 0) {
      Py_XDECREF(text);
      return false;
    }
    Py_DECREF(text);
    return true;
  };
  const auto set_double = [&](const char *key, double value) {
    auto *number = PyFloat_FromDouble(value);
    if (number == nullptr || PyDict_SetItemString(dict, key, number) != 0) {
      Py_XDECREF(number);
      return false;
    }
    Py_DECREF(number);
    return true;
  };
  const auto set_id = [&](const char *key, EntityId value) {
    return set_str(key, value.to_string());
  };
  if (!set_id("document_id", info->document_id) ||
      !set_id("track_id", info->track_id) ||
      !set_id("layer_id", info->layer_id) ||
      !set_id("curve_id", info->curve_id) ||
      !set_id("scale_id", info->scale_id) ||
      !set_id("sampling_axis_id", info->sampling_axis_id) ||
      !set_str("mnemonic", info->mnemonic) ||
      !set_str("display_name", info->display_name) ||
      !set_str("unit", info->unit) ||
      !set_str("scale_unit", info->scale_unit) ||
      !set_double("reference_depth", info->reference_depth) ||
      !set_double("display_depth", info->display_depth) ||
      !set_double("raw_value", info->raw_value) ||
      !set_double("scale_minimum", info->scale_minimum) ||
      !set_double("scale_maximum", info->scale_maximum)) {
    Py_DECREF(dict);
    return nullptr;
  }
  auto *sample_index =
      PyLong_FromUnsignedLongLong(info->sample_index);
  if (sample_index == nullptr ||
      PyDict_SetItemString(dict, "sample_index", sample_index) != 0) {
    Py_XDECREF(sample_index);
    Py_DECREF(dict);
    return nullptr;
  }
  Py_DECREF(sample_index);
  const char *qc = "valid";
  switch (info->qc_state) {
  case QcState::suspect:
    qc = "suspect";
    break;
  case QcState::invalid:
    qc = "invalid";
    break;
  case QcState::user_excluded:
    qc = "user_excluded";
    break;
  case QcState::valid:
    break;
  }
  if (!set_str("qc_state", qc) ||
      !set_str("scale_mode",
               info->scale_mode == ScaleMode::logarithmic ? "logarithmic"
                                                          : "linear") ||
      !set_str("scale_direction",
               info->scale_direction == ScaleDirection::right_to_left
                   ? "right_to_left"
                   : "left_to_right")) {
    Py_DECREF(dict);
    return nullptr;
  }
  auto *derived = info->derived ? Py_True : Py_False;
  if (PyDict_SetItemString(dict, "derived", derived) != 0) {
    Py_DECREF(dict);
    return nullptr;
  }
  if (info->derived) {
    if (!set_str("algorithm_id", info->algorithm_id) ||
        !set_str("algorithm_version", info->algorithm_version)) {
      Py_DECREF(dict);
      return nullptr;
    }
    auto *stale = info->derived_freshness == DerivedFreshness::stale
                      ? Py_True
                      : Py_False;
    if (PyDict_SetItemString(dict, "derived_stale", stale) != 0) {
      Py_DECREF(dict);
      return nullptr;
    }
  }
  return dict;
}

[[nodiscard]] PyObject *selection_state_impl(WellLogView *view) {
  const auto document_id = view->document_id();
  if (!document_id.has_value()) {
    Py_RETURN_NONE;
  }
  const auto &session = view->session();
  const auto selection = session.selection(*document_id);
  if (!selection.has_value()) {
    Py_RETURN_NONE;
  }
  auto *dict = PyDict_New();
  if (dict == nullptr) {
    return nullptr;
  }
  const auto set_double = [&](const char *key, double value) {
    auto *number = PyFloat_FromDouble(value);
    if (number == nullptr || PyDict_SetItemString(dict, key, number) != 0) {
      Py_XDECREF(number);
      return false;
    }
    Py_DECREF(number);
    return true;
  };
  auto *axis_id =
      PyUnicode_FromString(selection->sampling_axis_id.to_string().c_str());
  auto *first = PyLong_FromUnsignedLongLong(selection->first_row);
  auto *last = PyLong_FromUnsignedLongLong(selection->last_row);
  auto *valid = selection->valid ? Py_True : Py_False;
  if (axis_id == nullptr || first == nullptr || last == nullptr ||
      PyDict_SetItemString(dict, "sampling_axis_id", axis_id) != 0 ||
      PyDict_SetItemString(dict, "first_row", first) != 0 ||
      PyDict_SetItemString(dict, "last_row", last) != 0 ||
      PyDict_SetItemString(dict, "valid", valid) != 0 ||
      !set_double("top", selection->reference_depth_range.top) ||
      !set_double("bottom", selection->reference_depth_range.bottom)) {
    Py_XDECREF(axis_id);
    Py_XDECREF(first);
    Py_XDECREF(last);
    Py_DECREF(dict);
    return nullptr;
  }
  Py_DECREF(axis_id);
  Py_DECREF(first);
  Py_DECREF(last);
  return dict;
}

[[nodiscard]] PyObject *presentation_state_impl(WellLogView *view,
                                                const QString &document_id) {
  const auto parsed = parse_id(document_id, "document_id");
  if (!parsed) {
    return nullptr;
  }
  const auto *presentation = view->session().presentation(*parsed);
  if (presentation == nullptr) {
    Py_RETURN_NONE;
  }
  const PresentationBindingIndex index{*presentation};
  auto *dict = PyDict_New();
  if (dict == nullptr) {
    return nullptr;
  }
  auto *tracks = PyList_New(0);
  auto *scales = PyList_New(0);
  auto *layers = PyList_New(0);
  if (tracks == nullptr || scales == nullptr || layers == nullptr) {
    Py_XDECREF(tracks);
    Py_XDECREF(scales);
    Py_XDECREF(layers);
    Py_DECREF(dict);
    return nullptr;
  }
  const auto make_entry = []() -> PyObject * { return PyDict_New(); };
  const auto put_double = [](PyObject *entry, const char *key,
                             double value) -> bool {
    auto *number = PyFloat_FromDouble(value);
    if (number == nullptr || PyDict_SetItemString(entry, key, number) != 0) {
      Py_XDECREF(number);
      return false;
    }
    Py_DECREF(number);
    return true;
  };
  const auto put_id = [](PyObject *entry, const char *key,
                         EntityId value) -> bool {
    auto *text = PyUnicode_FromString(value.to_string().c_str());
    if (text == nullptr || PyDict_SetItemString(entry, key, text) != 0) {
      Py_XDECREF(text);
      return false;
    }
    Py_DECREF(text);
    return true;
  };
  const auto put_long = [](PyObject *entry, const char *key,
                           long long value) -> bool {
    auto *number = PyLong_FromLongLong(value);
    if (number == nullptr || PyDict_SetItemString(entry, key, number) != 0) {
      Py_XDECREF(number);
      return false;
    }
    Py_DECREF(number);
    return true;
  };
  const auto put_bool = [](PyObject *entry, const char *key,
                           bool value) -> bool {
    auto *flag = value ? Py_True : Py_False;
    return PyDict_SetItemString(entry, key, flag) == 0;
  };
  for (const auto *track : index.tracks_in_z_order()) {
    auto *entry = make_entry();
    if (entry == nullptr ||
        !put_id(entry, "id", track->id) ||
        !put_double(entry, "width_mm", track->width.value) ||
        !put_long(entry, "z_order", track->z_order) ||
        !put_bool(entry, "visible", track->visible) ||
        !put_double(entry, "header_height_mm", track->header.height.value)) {
      Py_XDECREF(entry);
      goto fail;
    }
    if (PyList_Append(tracks, entry) != 0) {
      Py_DECREF(entry);
      goto fail;
    }
    Py_DECREF(entry);
  }
  for (const auto &scale : presentation->scales()) {
    auto *entry = make_entry();
    if (entry == nullptr ||
        !put_id(entry, "id", scale.id) ||
        !put_id(entry, "track_id", scale.track_id) ||
        !put_double(entry, "minimum", scale.minimum) ||
        !put_double(entry, "maximum", scale.maximum)) {
      Py_XDECREF(entry);
      goto fail;
    }
    {
      auto *mode = PyUnicode_FromString(
          scale.mode == ScaleMode::logarithmic ? "logarithmic" : "linear");
      auto *unit =
          PyUnicode_FromString(scale.unit.c_str());
      auto *direction =
          PyUnicode_FromString(scale.direction ==
                                       ScaleDirection::right_to_left
                                   ? "right_to_left"
                                   : "left_to_right");
      if (mode == nullptr || unit == nullptr || direction == nullptr ||
          PyDict_SetItemString(entry, "mode", mode) != 0 ||
          PyDict_SetItemString(entry, "unit", unit) != 0 ||
          PyDict_SetItemString(entry, "direction", direction) != 0) {
        Py_XDECREF(mode);
        Py_XDECREF(unit);
        Py_XDECREF(direction);
        Py_DECREF(entry);
        goto fail;
      }
      Py_DECREF(mode);
      Py_DECREF(unit);
      Py_DECREF(direction);
    }
    if (PyList_Append(scales, entry) != 0) {
      Py_DECREF(entry);
      goto fail;
    }
    Py_DECREF(entry);
  }
  for (const auto &layer : presentation->curve_layers()) {
    auto *entry = make_entry();
    if (entry == nullptr ||
        !put_id(entry, "id", layer.id) ||
        !put_id(entry, "track_id", layer.track_id) ||
        !put_id(entry, "curve_id", layer.curve_id) ||
        !put_id(entry, "scale_id", layer.scale_id) ||
        !put_double(entry, "line_width_mm", layer.line_width.value) ||
        !put_long(entry, "z_order", layer.z_order) ||
        !put_bool(entry, "visible", layer.visible)) {
      Py_XDECREF(entry);
      goto fail;
    }
    {
      char color[10];
      std::snprintf(color, sizeof(color), "#%02x%02x%02x%02x",
                    layer.color.red, layer.color.green, layer.color.blue,
                    layer.color.alpha);
      auto *text = PyUnicode_FromString(color);
      if (text == nullptr ||
          PyDict_SetItemString(entry, "color", text) != 0) {
        Py_XDECREF(text);
        Py_DECREF(entry);
        goto fail;
      }
      Py_DECREF(text);
    }
    if (PyList_Append(layers, entry) != 0) {
      Py_DECREF(entry);
      goto fail;
    }
    Py_DECREF(entry);
  }
  if (PyDict_SetItemString(dict, "tracks", tracks) != 0 ||
      PyDict_SetItemString(dict, "scales", scales) != 0 ||
      PyDict_SetItemString(dict, "curve_layers", layers) != 0) {
    goto fail;
  }
  Py_DECREF(tracks);
  Py_DECREF(scales);
  Py_DECREF(layers);
  return dict;

fail:
  Py_DECREF(tracks);
  Py_DECREF(scales);
  Py_DECREF(layers);
  Py_DECREF(dict);
  return nullptr;
}

} // namespace

PyObject *apply_track_command(WellLogView *view, PyObject *payload) noexcept {
  try {
    return apply_track_command_impl(view, payload);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (const std::exception &exc) {
    set_welllog_error("WellLogError", "internal_error", exc.what());
    return nullptr;
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during track command");
    return nullptr;
  }
}

PyObject *hover_info(WellLogView *view) noexcept {
  try {
    return hover_info_impl(view);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during hover inspect");
    return nullptr;
  }
}

PyObject *selection_state(WellLogView *view) noexcept {
  try {
    return selection_state_impl(view);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure reading selection");
    return nullptr;
  }
}

PyObject *set_row_selection(WellLogView *view, const QString &axis_id,
                            unsigned long long first_row,
                            unsigned long long last_row) noexcept {
  try {
    const auto document_id = view->document_id();
    if (!document_id.has_value()) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "view has no document");
      return nullptr;
    }
    const auto axis = parse_id(axis_id, "axis_id");
    if (!axis) {
      return nullptr;
    }
    const auto result = view->session().execute(SetRowSelectionCommand{
        .document_id = *document_id,
        .sampling_axis_id = *axis,
        .first_row = first_row,
        .last_row = last_row,
    });
    if (!result.has_value()) {
      set_result_error(result.error(), "set_row_selection");
      return nullptr;
    }
    auto *dict = PyDict_New();
    if (dict == nullptr) {
      return nullptr;
    }
    auto *first = PyLong_FromUnsignedLongLong(first_row);
    auto *last = PyLong_FromUnsignedLongLong(last_row);
    if (first == nullptr || last == nullptr ||
        PyDict_SetItemString(dict, "first_row", first) != 0 ||
        PyDict_SetItemString(dict, "last_row", last) != 0) {
      Py_XDECREF(first);
      Py_XDECREF(last);
      Py_DECREF(dict);
      return nullptr;
    }
    Py_DECREF(first);
    Py_DECREF(last);
    return dict;
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure during row selection");
    return nullptr;
  }
}

PyObject *presentation_state(WellLogView *view,
                             const QString &document_id) noexcept {
  try {
    return presentation_state_impl(view, document_id);
  } catch (const std::bad_alloc &) {
    return PyErr_NoMemory();
  } catch (...) {
    set_welllog_error("WellLogError", "internal_error",
                      "unexpected native failure reading presentation");
    return nullptr;
  }
}

} // namespace welllog::python
