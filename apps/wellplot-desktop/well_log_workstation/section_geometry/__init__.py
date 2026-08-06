"""Section geometry modules (pure numpy, headless — Phase-2, T4 / #248).

The reservoir section (油藏剖面) host renders fault / contact / tie-quad
geometry computed by these modules in section 2D space (x = across-section
cumulative distance, y = depth). No Qt imports - pytest-runnable headless.
"""

from __future__ import annotations

from well_log_workstation.section_geometry.contact_2d import (
    ContactSegment2D,
    contact_polyline,
)
from well_log_workstation.section_geometry.fault_section import (
    SectionFault2D,
    apply_fault_throw_to_quad,
    apply_faults_to_quad,
    fault_polyline,
    fault_x,
    faults_from_json,
    faults_to_json,
)
from well_log_workstation.section_geometry.tie_polygons import (
    TieQuad2D,
    tie_quads,
)
from well_log_workstation.section_geometry.trajectory_2d import (
    project_trajectory_2d,
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
    "ContactSegment2D",
    "contact_polyline",
    "TieQuad2D",
    "tie_quads",
    "project_trajectory_2d",
]
