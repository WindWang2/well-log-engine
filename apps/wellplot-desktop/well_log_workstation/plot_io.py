"""Import / export single-well **plot definitions** (XML / Excel).

Does **not** embed curve samples — only plot metadata, display_set, and
data_bindings (model A: data on well). Excel is a minimal OOXML spreadsheet
so openpyxl is not required.
"""

from __future__ import annotations

import csv
import io
import uuid
import zipfile
from pathlib import Path
from typing import Any
from xml.etree import ElementTree as ET

from well_log_workstation.plot_document import (
    PlotDataBinding,
    PlotDocument,
    sync_data_bindings,
)
from well_log_workstation.workspace import Workspace, WorkspaceError, add_plot, save_workspace


def export_plot_xml(doc: PlotDocument, path: Path | str) -> Path:
    """Write plot definition XML (bindings + display set; no samples)."""
    out = Path(path)
    root = ET.Element(
        "WellPlot",
        {
            "schemaVersion": "1",
            "id": doc.id,
            "name": doc.name,
            "type": doc.type,
            "template_id": str(doc.template_id or ""),
        },
    )
    wells_el = ET.SubElement(root, "Wells")
    for wid in doc.well_ids:
        ET.SubElement(wells_el, "Well", {"id": wid})
    ds_el = ET.SubElement(root, "DisplaySet")
    for lid in doc.display_set:
        ET.SubElement(ds_el, "Leaf", {"id": lid})
    binds_el = ET.SubElement(root, "DataBindings")
    for b in doc.data_bindings:
        ET.SubElement(
            binds_el,
            "Binding",
            {
                "binding_id": b.binding_id,
                "plot_id": b.plot_id or doc.id,
                "well_id": b.well_id,
                "leaf_id": b.leaf_id,
                "mnemonic": b.mnemonic,
                "source_id": b.source_id,
            },
        )
    tree = ET.ElementTree(root)
    ET.indent(tree, space="  ")
    out.write_bytes(
        b'<?xml version="1.0" encoding="UTF-8"?>\n'
        + ET.tostring(root, encoding="utf-8")
    )
    return out


def import_plot_xml(
    workspace: Workspace,
    path: Path | str,
    *,
    plot_id: str | None = None,
) -> PlotDocument:
    """Create/update a single-well plot from XML definition."""
    text = Path(path).read_text(encoding="utf-8")
    root = ET.fromstring(text)
    if root.tag != "WellPlot":
        raise WorkspaceError(f"XML 根元素应为 WellPlot，得到 {root.tag}")
    name = root.get("name") or "导入井图"
    template_id = root.get("template_id") or "std-gr-rt-den"
    well_ids = [w.get("id") or "" for w in root.findall("./Wells/Well")]
    well_ids = [w for w in well_ids if w]
    if not well_ids:
        raise WorkspaceError("XML 未声明 Wells/Well@id")
    for wid in well_ids:
        if not any(w.id == wid for w in workspace.wells):
            raise WorkspaceError(
                f"XML 引用的井不在工区中: {wid}（请先导入井数据）"
            )
    leaf_ids = [
        el.get("id") or ""
        for el in root.findall("./DisplaySet/Leaf")
        if el.get("id")
    ]
    bindings: list[PlotDataBinding] = []
    for el in root.findall("./DataBindings/Binding"):
        raw = {
            "binding_id": el.get("binding_id"),
            "plot_id": el.get("plot_id"),
            "well_id": el.get("well_id"),
            "leaf_id": el.get("leaf_id"),
            "mnemonic": el.get("mnemonic"),
            "source_id": el.get("source_id"),
        }
        b = PlotDataBinding.from_json(raw)
        if b is not None:
            bindings.append(b)
    if not leaf_ids and bindings:
        leaf_ids = [b.leaf_id for b in bindings]

    pid = plot_id or root.get("id") or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name,
        type="single_well",
        well_ids=well_ids,
        template_id=template_id,
        path=f"plots/{pid}.json",
        display_set=list(leaf_ids),
        data_bindings=bindings,
    )
    if not doc.data_bindings and leaf_ids:
        sync_data_bindings(doc, well_id=well_ids[0], leaf_ids=leaf_ids)
    else:
        for b in doc.data_bindings:
            if not b.plot_id:
                b.plot_id = doc.id  # type: ignore[misc]
        # frozen dataclass — rebuild if needed
        doc.data_bindings = [
            PlotDataBinding(
                binding_id=b.binding_id,
                plot_id=doc.id,
                well_id=b.well_id,
                leaf_id=b.leaf_id,
                mnemonic=b.mnemonic,
                source_id=b.source_id,
            )
            for b in doc.data_bindings
        ]
        doc.display_set = [b.leaf_id for b in doc.data_bindings]

    from well_log_workstation.plot_document import save_plot_document

    save_plot_document(workspace, doc)
    return doc


def export_plot_excel(doc: PlotDocument, path: Path | str) -> Path:
    """Write plot definition as .xlsx (sheet Plot + sheet Bindings)."""
    out = Path(path)
    # Sheet 1: plot header key/value
    plot_rows = [
        ["key", "value"],
        ["id", doc.id],
        ["name", doc.name],
        ["type", doc.type],
        ["template_id", str(doc.template_id or "")],
        ["well_ids", ",".join(doc.well_ids)],
        ["display_set", ",".join(doc.display_set)],
    ]
    bind_rows = [
        [
            "binding_id",
            "plot_id",
            "well_id",
            "leaf_id",
            "mnemonic",
            "source_id",
        ]
    ]
    for b in doc.data_bindings:
        bind_rows.append(
            [
                b.binding_id,
                b.plot_id or doc.id,
                b.well_id,
                b.leaf_id,
                b.mnemonic,
                b.source_id,
            ]
        )
    _write_xlsx(
        out,
        sheets={
            "Plot": plot_rows,
            "Bindings": bind_rows,
        },
    )
    return out


def import_plot_excel(
    workspace: Workspace,
    path: Path | str,
    *,
    plot_id: str | None = None,
) -> PlotDocument:
    """Import single-well plot definition from .xlsx or .csv (Plot sheet)."""
    p = Path(path)
    if p.suffix.lower() in (".csv", ".txt"):
        return _import_plot_csv(workspace, p, plot_id=plot_id)
    sheets = _read_xlsx_sheets(p)
    plot_sheet = sheets.get("Plot") or sheets.get("plot")
    bind_sheet = sheets.get("Bindings") or sheets.get("bindings")
    if not plot_sheet:
        raise WorkspaceError("Excel 缺少 Plot 工作表")
    kv: dict[str, str] = {}
    for row in plot_sheet[1:]:
        if len(row) >= 2 and row[0]:
            kv[str(row[0]).strip()] = str(row[1]).strip()
    name = kv.get("name") or "导入井图"
    template_id = kv.get("template_id") or "std-gr-rt-den"
    well_ids = [w for w in (kv.get("well_ids") or "").split(",") if w.strip()]
    if not well_ids:
        raise WorkspaceError("Excel Plot 表缺少 well_ids")
    for wid in well_ids:
        if not any(w.id == wid for w in workspace.wells):
            raise WorkspaceError(f"Excel 引用的井不在工区中: {wid}")
    leaf_ids = [x for x in (kv.get("display_set") or "").split(",") if x.strip()]
    bindings: list[PlotDataBinding] = []
    if bind_sheet and len(bind_sheet) > 1:
        headers = [str(h).strip() for h in bind_sheet[0]]
        idx = {h: i for i, h in enumerate(headers)}
        for row in bind_sheet[1:]:
            def col(name: str) -> str:
                i = idx.get(name)
                if i is None or i >= len(row):
                    return ""
                return str(row[i]).strip()

            raw = {
                "binding_id": col("binding_id"),
                "plot_id": col("plot_id"),
                "well_id": col("well_id") or well_ids[0],
                "leaf_id": col("leaf_id"),
                "mnemonic": col("mnemonic"),
                "source_id": col("source_id"),
            }
            b = PlotDataBinding.from_json(raw)
            if b is not None:
                bindings.append(b)
    if not leaf_ids and bindings:
        leaf_ids = [b.leaf_id for b in bindings]
    pid = plot_id or kv.get("id") or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=name,
        type="single_well",
        well_ids=well_ids,
        template_id=template_id,
        path=f"plots/{pid}.json",
        display_set=list(leaf_ids),
        data_bindings=bindings,
    )
    if not doc.data_bindings and leaf_ids:
        sync_data_bindings(doc, well_id=well_ids[0], leaf_ids=leaf_ids)
    else:
        doc.data_bindings = [
            PlotDataBinding(
                binding_id=b.binding_id,
                plot_id=doc.id,
                well_id=b.well_id,
                leaf_id=b.leaf_id,
                mnemonic=b.mnemonic,
                source_id=b.source_id,
            )
            for b in doc.data_bindings
        ]
        doc.display_set = [b.leaf_id for b in doc.data_bindings] or list(leaf_ids)

    from well_log_workstation.plot_document import save_plot_document

    save_plot_document(workspace, doc)
    return doc


def _import_plot_csv(
    workspace: Workspace, path: Path, *, plot_id: str | None
) -> PlotDocument:
    """Minimal CSV: header row key,value pairs or bindings table with leaf_id column."""
    text = path.read_text(encoding="utf-8")
    rows = list(csv.reader(io.StringIO(text)))
    if not rows:
        raise WorkspaceError("空 CSV")
    # Detect bindings-style
    header = [c.strip() for c in rows[0]]
    if "leaf_id" in header:
        idx = {h: i for i, h in enumerate(header)}
        well_id = ""
        leaf_ids: list[str] = []
        bindings: list[PlotDataBinding] = []
        for row in rows[1:]:
            def col(name: str) -> str:
                i = idx.get(name, -1)
                return str(row[i]).strip() if 0 <= i < len(row) else ""

            lid = col("leaf_id")
            if not lid:
                continue
            wid = col("well_id") or well_id
            if not well_id and wid:
                well_id = wid
            leaf_ids.append(lid)
            b = PlotDataBinding.from_json(
                {
                    "binding_id": col("binding_id"),
                    "plot_id": col("plot_id"),
                    "well_id": wid,
                    "leaf_id": lid,
                    "mnemonic": col("mnemonic"),
                    "source_id": col("source_id"),
                }
            )
            if b:
                bindings.append(b)
        if not well_id:
            raise WorkspaceError("CSV 缺少 well_id")
        if not any(w.id == well_id for w in workspace.wells):
            raise WorkspaceError(f"井不在工区: {well_id}")
        pid = plot_id or str(uuid.uuid4())
        doc = PlotDocument(
            id=pid,
            name=path.stem,
            type="single_well",
            well_ids=[well_id],
            template_id="std-gr-rt-den",
            path=f"plots/{pid}.json",
            display_set=leaf_ids,
            data_bindings=bindings
            or [
                PlotDataBinding(
                    binding_id=str(uuid.uuid4()),
                    plot_id=pid,
                    well_id=well_id,
                    leaf_id=lid,
                )
                for lid in leaf_ids
            ],
        )
        from well_log_workstation.plot_document import save_plot_document

        save_plot_document(workspace, doc)
        return doc
    # key,value
    kv = {str(r[0]).strip(): str(r[1]).strip() for r in rows[1:] if len(r) >= 2}
    well_ids = [w for w in (kv.get("well_ids") or "").split(",") if w]
    if not well_ids:
        raise WorkspaceError("CSV 缺少 well_ids")
    leaf_ids = [x for x in (kv.get("display_set") or "").split(",") if x]
    pid = plot_id or kv.get("id") or str(uuid.uuid4())
    doc = PlotDocument(
        id=pid,
        name=kv.get("name") or path.stem,
        type="single_well",
        well_ids=well_ids,
        template_id=kv.get("template_id") or "std-gr-rt-den",
        path=f"plots/{pid}.json",
    )
    sync_data_bindings(doc, well_id=well_ids[0], leaf_ids=leaf_ids)
    from well_log_workstation.plot_document import save_plot_document

    save_plot_document(workspace, doc)
    return doc


def _xml_escape(s: str) -> str:
    return (
        s.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
        .replace('"', "&quot;")
    )


def _sheet_to_xml(name: str, rows: list[list[Any]]) -> str:
    # Minimal SpreadsheetML worksheet
    lines = [
        '<?xml version="1.0" encoding="UTF-8" standalone="yes"?>',
        '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main">',
        "<sheetData>",
    ]
    for r_i, row in enumerate(rows, start=1):
        lines.append(f'<row r="{r_i}">')
        for c_i, cell in enumerate(row):
            col = _col_name(c_i + 1)
            ref = f"{col}{r_i}"
            val = "" if cell is None else str(cell)
            lines.append(
                f'<c r="{ref}" t="inlineStr"><is><t>{_xml_escape(val)}</t></is></c>'
            )
        lines.append("</row>")
    lines.append("</sheetData></worksheet>")
    return "\n".join(lines)


def _col_name(n: int) -> str:
    s = ""
    while n:
        n, r = divmod(n - 1, 26)
        s = chr(65 + r) + s
    return s


def _write_xlsx(path: Path, sheets: dict[str, list[list[Any]]]) -> None:
    content_types = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">
  <Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>
  <Default Extension="xml" ContentType="application/xml"/>
  <Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>
"""
    for i in range(1, len(sheets) + 1):
        content_types += (
            f'  <Override PartName="/xl/worksheets/sheet{i}.xml" '
            'ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>\n'
        )
    content_types += "</Types>"

    workbook = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"
 xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">
  <sheets>
"""
    rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
"""
    names = list(sheets.keys())
    for i, name in enumerate(names, start=1):
        workbook += f'    <sheet name="{_xml_escape(name)}" sheetId="{i}" r:id="rId{i}"/>\n'
        rels += (
            f'  <Relationship Id="rId{i}" '
            'Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" '
            f'Target="worksheets/sheet{i}.xml"/>\n'
        )
    workbook += "  </sheets>\n</workbook>"
    rels += "</Relationships>"

    root_rels = """<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">
  <Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>
</Relationships>
"""
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w", compression=zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("[Content_Types].xml", content_types)
        zf.writestr("_rels/.rels", root_rels)
        zf.writestr("xl/workbook.xml", workbook)
        zf.writestr("xl/_rels/workbook.xml.rels", rels)
        for i, name in enumerate(names, start=1):
            zf.writestr(
                f"xl/worksheets/sheet{i}.xml",
                _sheet_to_xml(name, sheets[name]),
            )


def _read_xlsx_sheets(path: Path) -> dict[str, list[list[str]]]:
    """Very small reader for our own export format (inlineStr cells)."""
    ns = {
        "m": "http://schemas.openxmlformats.org/spreadsheetml/2006/main",
        "r": "http://schemas.openxmlformats.org/officeDocument/2006/relationships",
        "pr": "http://schemas.openxmlformats.org/package/2006/relationships",
    }
    with zipfile.ZipFile(path, "r") as zf:
        wb = ET.fromstring(zf.read("xl/workbook.xml"))
        rels_root = ET.fromstring(zf.read("xl/_rels/workbook.xml.rels"))
        rid_to_target = {
            rel.get("Id"): rel.get("Target")
            for rel in rels_root.findall("pr:Relationship", ns)
        }
        out: dict[str, list[list[str]]] = {}
        for sheet in wb.findall("m:sheets/m:sheet", ns):
            name = sheet.get("name") or "Sheet"
            rid = sheet.get(
                "{http://schemas.openxmlformats.org/officeDocument/2006/relationships}id"
            )
            target = rid_to_target.get(rid or "", "")
            if not target:
                continue
            if not target.startswith("xl/"):
                target = "xl/" + target.lstrip("/")
            if target.startswith("/"):
                target = target[1:]
            ws = ET.fromstring(zf.read(target))
            rows: list[list[str]] = []
            for row in ws.findall("m:sheetData/m:row", ns):
                cells: list[str] = []
                for c in row.findall("m:c", ns):
                    t = c.find("m:is/m:t", ns)
                    if t is not None and t.text is not None:
                        cells.append(t.text)
                    else:
                        v = c.find("m:v", ns)
                        cells.append(v.text if v is not None and v.text else "")
                rows.append(cells)
            out[name] = rows
        return out
