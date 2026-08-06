"""Section geometry modules (pure numpy, headless — Phase-2, T4 / #248).

The reservoir section (油藏剖面) host renders fault / contact / tie-quad
geometry computed by these modules in section 2D space (x = across-section
cumulative distance, y = depth). No Qt imports - pytest-runnable headless.
"""

from __future__ import annotations

from well_log_workstation.section_geometry.fault_2d import (
    FaultSegment2D,
    curtain_slice_fault,
)
from well_log_workstation.section_geometry.contact_2d import (
    ContactSegment2D,
    contact_polyline,
)
from well_log_workstation.section_geometry.tie_polygons import (
    TieQuad2D,
    tie_quads,
)
from well_log_workstation.section_geometry.trajectory_2d import (
    project_trajectory_2d,
)

__all__ = [
    "FaultSegment2D",
    "curtain_slice_fault",
    "ContactSegment2D",
    "contact_polyline",
    "TieQuad2D",
    "tie_quads",
    "project_trajectory_2d",
]
