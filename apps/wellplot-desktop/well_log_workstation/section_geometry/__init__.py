"""Section geometry modules (pure numpy, headless — Phase-2, T4 / #248).

The reservoir section (油藏剖面) host renders fault / contact / tie-quad
geometry computed by these modules in section 2D space (x = across-section
cumulative distance, y = depth). No Qt imports - pytest-runnable headless.
"""

from __future__ import annotations

from well_log_workstation.section_geometry.contact_section import (
    FluidContact2D,
    contact_depth_at,
    contact_segment_2d,
    contacts_from_json,
    contacts_to_json,
    split_quad_by_contact,
    split_quad_composite,
)
from well_log_workstation.section_geometry.fault_section import (
    SectionFault2D,
    apply_fault_throw_to_quad,
    apply_faults_to_quad,
    fault_polyline,
    fault_x,
    faults_from_json,
    faults_to_json,
    split_quad_by_fault,
)
from well_log_workstation.section_geometry.tie_polygons import (
    TieQuad2D,
    tie_quads,
)
from well_log_workstation.section_geometry.trajectory_2d import (
    normalize_offsets,
    project_closure_to_section,
    project_trajectory_2d,
    section_trajectory_polyline,
)

__all__ = [
    # Faults — 2D position+throw model (FRS §3.3 / P1-A). The legacy 3D-CRS
    # ``curtain_slice_fault`` path is kept in fault_2d.py but no longer
    # exported; it had no 3D context in the host.
    "SectionFault2D",
    "apply_fault_throw_to_quad",
    "apply_faults_to_quad",
    "fault_polyline",
    "fault_x",
    "faults_from_json",
    "faults_to_json",
    "split_quad_by_fault",
    # Fluid contacts — 2D per-well depth model (FRS §3.3 / P1-B). The legacy
    # 3D-CRS ``contact_polyline`` is kept in contact_2d.py but no longer
    # exported.
    "FluidContact2D",
    "contact_depth_at",
    "contact_segment_2d",
    "split_quad_by_contact",
    "split_quad_composite",
    "contacts_from_json",
    "contacts_to_json",
    "TieQuad2D",
    "tie_quads",
    "project_trajectory_2d",
    "project_closure_to_section",
    "normalize_offsets",
    "section_trajectory_polyline",
]
