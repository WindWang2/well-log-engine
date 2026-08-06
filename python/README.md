# WellLogEngine Python bindings

`welllog-engine` embeds the native C++ `WellLogView` in PySide6 applications.
Curve samples are submitted through the Python Buffer Protocol and retained by
the immutable native document without copying when dtype, shape, byte order, and
stride are compatible.

Arrays passed to `WellLogView.submit_curve()` / `submit_multi_track()` /
`submit_multi_well_section()` must be marked read-only before submission and
must not be mutated through another alias until the document-owning view is
released. Writable buffers are rejected instead of being silently copied. The
returned report records `access_mode`, dtype, stride, length, and the retained
source address for each buffer.

### Multi-track and multi-well (#225)

- `submit_multi_track(payload)` — one well, multiple tracks/layers/markers  
- `submit_multi_well_section(payload)` — several wells + shared depth viewport  
- `clear_multi_well_section()` — clear layout/overlays  

See workstation bridge helpers in `well_log_workstation/engine_bridge.py` and
`docs/research/2026-08-03-welllogengine-python-bindings-225.md`.
