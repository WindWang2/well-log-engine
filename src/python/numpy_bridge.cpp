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

#include <QByteArray>
#include <QThread>
#include <QUuid>

#include <algorithm>
#include <cmath>
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
    return;
  }
  auto *type = PyObject_GetAttrString(module, type_name);
  Py_DECREF(module);
  if (type == nullptr) {
    return;
  }
  auto *instance = PyObject_CallFunction(type, "ss", message, code);
  if (instance != nullptr) {
    PyErr_SetObject(type, instance);
    Py_DECREF(instance);
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
  const auto axis_uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto curve_axis_id = parse_id(axis_uuid, "curve axis_id");
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
    PyTuple_SetItem(tuple, 0, PyFloat_FromDouble(ticks.step));
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
    PyTuple_SetItem(tuple, 0, PyFloat_FromDouble(ticks.step));
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
          .semantic = IntervalSemantic::custom,
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
  for (Py_ssize_t ti = 0; ti < track_count; ++ti) {
    auto *track = PyList_GetItem(tracks_obj, ti);
    if (track == nullptr || !PyDict_Check(track)) {
      set_welllog_error("WellLogValidationError", "invalid_document",
                        "each track must be a dict");
      return nullptr;
    }
    auto *layers_obj = PyDict_GetItemString(track, "layers");
    if (layers_obj == nullptr || !PyList_Check(layers_obj) ||
        PyList_Size(layers_obj) <= 0) {
      // Empty layers = skip (e.g. host depth-role track with no curves)
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

  // Interval layer (T4 / #276): one per curve track when the payload
  // carried intervals. The engine draws ALL document intervals on every
  // interval layer (no semantic filter), so one per track suffices.
  if (intervals_obj != nullptr && PyList_Check(intervals_obj) &&
      PyList_Size(intervals_obj) > 0) {
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
      const auto interval_layer_id = derive_presentation_id(
          *document_id, "welllog-python/mt-interval-layer",
          {*document_id, *axis_id, track_id});
      presentation_builder.add_interval_layer(IntervalLayerSpec{
          .id = interval_layer_id,
          .track_id = track_id,
          .z_order = 5,  // below curves (z_order 0+) so fills sit behind
          .draw_labels = true,
          .label_font_size = Millimetres{2.5},
          .label_color = RgbaColor{0x33, 0x33, 0x33, 0xff},
      });
      break;  // one interval layer renders all intervals
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
          PyLong_FromLong(static_cast<long>(curve_track_count))) != 0 ||
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
          if (!layer_curve_id ||
              curve_index.find(layer_curve_id->to_string()) ==
                  curve_index.end()) {
            if (layer_curve_id) {
              PyErr_Clear();
            }
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

} // namespace welllog::python
