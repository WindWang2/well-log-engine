"""Core domain model (Epic C / FRS §3.x 岩心) — runs + samples + photo linkage.

Covers:
* CoreRun/CoreSample structure — explicit depth domain + unit on every
  depth-bearing object (C5), stratigraphic unit linkage (C1), physical
  properties, lab-result + attachment references, photo-segment linkage
  (photo stays the CorePhotoModel's job — core references it by id);
* validation — reversed intervals, un-declared depth domains, duplicate ids;
* persistence round-trip + missing/corrupt tolerance;
* linkage against the existing photo model (photos are associated objects).
"""

from __future__ import annotations

import os
from pathlib import Path

import pytest

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")

from well_log_workstation.core_model import (
    CoreModel,
    CoreRun,
    CoreSample,
    DEPTH_DOMAINS,
    load_core_for_well,
    save_core_for_well,
)


def _model() -> CoreModel:
    return CoreModel(
        well_id="w1",
        runs=[
            CoreRun(
                id="r1",
                top=2100.0,
                bottom=2120.0,
                recovery_m=18.5,
                label="第一筒",
                samples=[
                    CoreSample(
                        id="s1",
                        depth=2100.5,
                        description="细粒砂岩",
                        lithology_unit_id="f1",
                        porosity=0.12,
                        permeability_md=8.5,
                        density_gcc=2.35,
                        lab_report_ref="lab-2024-01",
                        photo_segment_id="p1",
                    ),
                ],
                photo_segment_ids=["p1", "p2"],
            ),
            CoreRun(
                id="r2",
                top=2150.0,
                bottom=2155.0,
                recovery_m=5.0,
                label="第二筒",
            ),
        ],
    )


def test_depth_domains_explicit() -> None:
    assert DEPTH_DOMAINS == ("MD", "TVD", "TVDSS")


def test_structure_and_linkage() -> None:
    m = _model()
    run = m.runs[0]
    assert run.thickness() == pytest.approx(20.0)
    assert run.samples[0].lithology_unit_id == "f1"  # → stratigraphy dict
    assert run.samples[0].photo_segment_id == "p1"  # → CorePhotoModel
    assert run.photo_segment_ids == ["p1", "p2"]
    assert run.samples[0].depth_domain == "MD"
    assert run.samples[0].unit == "m"


def test_validation() -> None:
    assert _model().validate() == []

    bad = CoreModel(
        well_id="w1",
        runs=[
            CoreRun(id="r1", top=2120.0, bottom=2100.0),  # reversed
            CoreRun(
                id="r2", top=2100.0, bottom=2110.0,
                depth_domain="?",  # un-declared domain
                samples=[CoreSample(id="s1", depth=2105.0),
                         CoreSample(id="s1", depth=2106.0)],  # dup sample
            ),
            CoreRun(id="r2", top=2110.0, bottom=2120.0),  # dup run
        ],
    )
    problems = bad.validate()
    assert any("顶深大于底深" in p for p in problems)
    assert any("深度域" in p for p in problems)
    assert any("重复 run" in p for p in problems)
    assert any("重复样品" in p for p in problems)


def test_persistence_round_trip(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws", name="CORE")
    well = add_well(ws, name="W1", path="wells/w1.las")
    save_core_for_well(ws, well.id, _model())
    loaded, diags = load_core_for_well(ws, well.id)
    assert diags == []
    assert len(loaded.runs) == 2
    # Runs sorted by top.
    assert loaded.runs[0].top == pytest.approx(2100.0)
    assert loaded.runs[1].top == pytest.approx(2150.0)
    s = loaded.runs[0].samples[0]
    assert s.porosity == pytest.approx(0.12)
    assert s.permeability_md == pytest.approx(8.5)
    assert s.photo_segment_id == "p1"
    assert s.lab_report_ref == "lab-2024-01"


def test_missing_and_corrupt_files(tmp_path: Path) -> None:
    from well_log_workstation.workspace import add_well, create_workspace

    ws = create_workspace(tmp_path / "ws2", name="CORE2")
    well = add_well(ws, name="W1", path="wells/w1.las")
    m, diags = load_core_for_well(ws, well.id)
    assert m.runs == [] and diags == []
    from well_log_workstation.core_model import core_file_path

    corrupt = core_file_path(ws, well.id)
    corrupt.parent.mkdir(parents=True, exist_ok=True)
    corrupt.write_text("{bad", encoding="utf-8")
    m2, diags2 = load_core_for_well(ws, well.id)
    assert m2.runs == [] and diags2 and "损坏" in diags2[0]


def test_photo_segments_are_associated_objects(tmp_path: Path) -> None:
    """The photo model stays the owner of photos; core links by id."""
    from well_log_workstation.core_photo_model import (
        CorePhotoModel,
        CorePhotoSegment,
    )

    photos = CorePhotoModel(
        well_id="w1",
        segments=[
            CorePhotoSegment(id="p1", top=2100.0, bottom=2103.0,
                             image_path="photos/2100.jpg", label="顶部"),
            CorePhotoSegment(id="p2", top=2103.0, bottom=2106.0,
                             image_path="photos/2103.jpg", label="中段"),
        ],
    )
    m = _model()
    run = m.runs[0]
    for seg_id in run.photo_segment_ids:
        assert any(s.id == seg_id for s in photos.segments), (
            f"photo segment {seg_id} must resolve in the photo model"
        )
    assert photos.segments[0].id == run.samples[0].photo_segment_id
