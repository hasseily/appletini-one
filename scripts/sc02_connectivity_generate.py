#!/usr/bin/env python3
"""Generate deterministic SC-02 connectivity CSV and Graphviz DOT files.

Input must use the ``appletini.sc02_schematic.v1`` schema emitted by
``sc02_schematic_extract.py``.  This script uses only the Python standard
library and does not invoke Graphviz.
"""

from __future__ import annotations

import argparse
import csv
import io
import json
import os
from pathlib import Path
import re
import sys
import unicodedata
from typing import Any


SCHEMA_NAME = "appletini.sc02_schematic.v1"
CSV_FILENAME = "sc02_pin_net_memberships.csv"
INDEX_DOT_FILENAME = "index.dot"
EXPECTED_SUMMARY = {
    "sheet_count": 7,
    "component_unit_count": 785,
    "physical_component_ref_count": 484,
    "explicit_pin_entry_count": 3213,
    "unique_explicit_pin_id_count": 3213,
    "net_pin_membership_count": 3519,
    "unique_connected_pin_id_count": 3421,
    "unique_implicit_pin_id_count": 208,
    "sheet_net_entry_count": 1257,
    "distinct_net_name_count": 1088,
    "labeled_sheet_net_count": 383,
    "distinct_net_label_name_count": 234,
    "net_label_placement_count": 718,
    "toc_entry_count": 11154,
}
CSV_FIELDS = (
    "sheet_name",
    "page_number",
    "component_ref",
    "physical_ref",
    "unit_suffix",
    "pin_id",
    "pin_number",
    "net_name",
    "implicit",
    "component_anchor_count",
    "component_anchor_x0_points",
    "component_anchor_y0_points",
    "component_anchor_x1_points",
    "component_anchor_y1_points",
    "component_anchor_center_x_points",
    "component_anchor_center_y_points",
    "pin_anchor_count",
    "pin_anchor_x0_points",
    "pin_anchor_y0_points",
    "pin_anchor_x1_points",
    "pin_anchor_y1_points",
    "pin_anchor_center_x_points",
    "pin_anchor_center_y_points",
)


class GenerationError(RuntimeError):
    """Raised when an input model is invalid or internally inconsistent."""


def _require_mapping(value: Any, context: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise GenerationError(f"{context} must be a JSON object")
    return value


def _require_list(value: Any, context: str) -> list[Any]:
    if not isinstance(value, list):
        raise GenerationError(f"{context} must be a JSON array")
    return value


def _natural_key(value: str) -> tuple[tuple[int, Any], ...]:
    parts = re.split(r"(\d+)", value)
    return tuple(
        (0, int(part)) if part.isdigit() else (1, part.casefold(), part)
        for part in parts
    )


def _safe_slug(value: str) -> str:
    normalized = unicodedata.normalize("NFKD", value)
    ascii_value = normalized.encode("ascii", "ignore").decode("ascii")
    slug = re.sub(r"[^a-z0-9]+", "-", ascii_value.casefold()).strip("-")
    return slug or "sheet"


def _dot_quote(value: Any) -> str:
    text = str(value)
    text = (
        text.replace("\\", "\\\\")
        .replace('"', '\\"')
        .replace("\r", "\\r")
        .replace("\n", "\\n")
    )
    return f'"{text}"'


def _atomic_write_text(path: Path, payload: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = path.with_name(f".{path.name}.tmp")
    try:
        temporary_path.write_text(payload, encoding="utf-8", newline="\n")
        os.replace(temporary_path, path)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()


def _load_model(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            model = json.load(stream)
    except (OSError, json.JSONDecodeError) as exc:
        raise GenerationError(f"Could not read input JSON: {exc}") from exc
    return _require_mapping(model, "input root")


def _first_anchor_fields(
    anchors_value: Any,
    prefix: str,
) -> dict[str, Any]:
    anchors = _require_list(anchors_value, f"{prefix} anchors")
    result: dict[str, Any] = {f"{prefix}_anchor_count": len(anchors)}
    field_names = (
        "x0_points",
        "y0_points",
        "x1_points",
        "y1_points",
        "center_x_points",
        "center_y_points",
    )
    for field_name in field_names:
        result[f"{prefix}_anchor_{field_name}"] = ""
    if not anchors:
        return result

    anchor = _require_mapping(anchors[0], f"first {prefix} anchor")
    bbox = _require_list(anchor.get("bbox_points"), f"{prefix} anchor bbox")
    center = _require_list(
        anchor.get("center_points"), f"{prefix} anchor center"
    )
    if len(bbox) != 4 or len(center) != 2:
        raise GenerationError(f"Malformed {prefix} anchor coordinates")
    values = (*bbox, *center)
    for field_name, value in zip(field_names, values):
        result[f"{prefix}_anchor_{field_name}"] = value
    return result


def _build_membership_rows(model: dict[str, Any]) -> list[dict[str, Any]]:
    sheets = _require_list(model.get("sheets"), "sheets")
    rows: list[dict[str, Any]] = []
    global_pin_to_nets: dict[str, set[str]] = {}

    for sheet_value in sorted(
        sheets,
        key=lambda value: int(_require_mapping(value, "sheet").get("page_number", 0)),
    ):
        sheet = _require_mapping(sheet_value, "sheet")
        sheet_name = str(sheet.get("name", ""))
        page_number = sheet.get("page_number")
        if not sheet_name or not isinstance(page_number, int):
            raise GenerationError("Every sheet needs a name and integer page_number")

        explicit_pins: dict[str, dict[str, Any]] = {}
        for component_value in _require_list(
            sheet.get("components"), f"{sheet_name} components"
        ):
            component = _require_mapping(component_value, "component")
            component_ref = str(component.get("ref", ""))
            physical_ref = str(component.get("physical_ref", ""))
            if not component_ref or not physical_ref:
                raise GenerationError(
                    f"Component in {sheet_name!r} lacks ref or physical_ref"
                )
            for pin_value in _require_list(
                component.get("pins"), f"{component_ref} pins"
            ):
                pin = _require_mapping(pin_value, "component pin")
                pin_id = str(pin.get("id", ""))
                if not pin_id:
                    raise GenerationError(f"Empty pin id on {component_ref!r}")
                if pin_id in explicit_pins:
                    raise GenerationError(
                        f"Pin {pin_id!r} belongs to two component units on {sheet_name!r}"
                    )
                explicit_pins[pin_id] = {
                    "component_ref": component_ref,
                    "physical_ref": physical_ref,
                    "unit_suffix": component.get("unit_suffix") or "",
                    "pin_number": str(pin.get("number", "")),
                    "component_anchors": component.get("anchors", []),
                    "pin_anchors": pin.get("anchors", []),
                    "implicit": False,
                }

        implicit_pins: dict[str, dict[str, Any]] = {}
        for pin_value in _require_list(
            sheet.get("implicit_pins"), f"{sheet_name} implicit_pins"
        ):
            pin = _require_mapping(pin_value, "implicit pin")
            pin_id = str(pin.get("id", ""))
            if not pin_id:
                raise GenerationError(f"Empty implicit pin id on {sheet_name!r}")
            if pin_id in explicit_pins or pin_id in implicit_pins:
                raise GenerationError(
                    f"Duplicate explicit/implicit pin {pin_id!r} on {sheet_name!r}"
                )
            implicit_pins[pin_id] = {
                "component_ref": "",
                "physical_ref": str(pin.get("physical_ref", "")),
                "unit_suffix": "",
                "pin_number": str(pin.get("number", "")),
                "component_anchors": [],
                "pin_anchors": pin.get("anchors", []),
                "implicit": True,
            }

        sheet_pin_to_net: dict[str, str] = {}
        for net_value in _require_list(sheet.get("nets"), f"{sheet_name} nets"):
            net = _require_mapping(net_value, "net")
            net_name = str(net.get("name", ""))
            if not net_name:
                raise GenerationError(f"Empty net name on {sheet_name!r}")
            for pin_id_value in _require_list(
                net.get("pin_ids"), f"{sheet_name}:{net_name} pin_ids"
            ):
                pin_id = str(pin_id_value)
                if pin_id in sheet_pin_to_net:
                    raise GenerationError(
                        f"Pin {pin_id!r} occurs in two sheet-net entries on {sheet_name!r}"
                    )
                pin_data = explicit_pins.get(pin_id) or implicit_pins.get(pin_id)
                if pin_data is None:
                    raise GenerationError(
                        f"Net {net_name!r} refers to unknown pin {pin_id!r} "
                        f"on {sheet_name!r}"
                    )
                sheet_pin_to_net[pin_id] = net_name
                global_pin_to_nets.setdefault(pin_id, set()).add(net_name)

                row = {
                    "sheet_name": sheet_name,
                    "page_number": page_number,
                    "component_ref": pin_data["component_ref"],
                    "physical_ref": pin_data["physical_ref"],
                    "unit_suffix": pin_data["unit_suffix"],
                    "pin_id": pin_id,
                    "pin_number": pin_data["pin_number"],
                    "net_name": net_name,
                    "implicit": "true" if pin_data["implicit"] else "false",
                }
                row.update(
                    _first_anchor_fields(
                        pin_data["component_anchors"], "component"
                    )
                )
                row.update(_first_anchor_fields(pin_data["pin_anchors"], "pin"))
                rows.append(row)

    conflicts = {
        pin_id: sorted(net_names)
        for pin_id, net_names in global_pin_to_nets.items()
        if len(net_names) > 1
    }
    if conflicts:
        first_pin = sorted(conflicts, key=_natural_key)[0]
        raise GenerationError(
            f"Pin {first_pin!r} belongs to multiple net names: "
            f"{conflicts[first_pin]!r}"
        )

    rows.sort(
        key=lambda row: (
            row["page_number"],
            _natural_key(row["component_ref"] or row["physical_ref"]),
            _natural_key(row["pin_number"]),
            _natural_key(row["pin_id"]),
            row["net_name"],
        )
    )
    return rows


def _recomputed_summary(model: dict[str, Any], rows: list[dict[str, Any]]) -> dict[str, int]:
    sheets = _require_list(model.get("sheets"), "sheets")
    components = [
        _require_mapping(component, "component")
        for sheet_value in sheets
        for component in _require_list(
            _require_mapping(sheet_value, "sheet").get("components"), "components"
        )
    ]
    explicit_pins = [
        _require_mapping(pin, "pin")
        for component in components
        for pin in _require_list(component.get("pins"), "component pins")
    ]
    nets = [
        _require_mapping(net, "net")
        for sheet_value in sheets
        for net in _require_list(
            _require_mapping(sheet_value, "sheet").get("nets"), "nets"
        )
    ]
    connected_pin_ids = {row["pin_id"] for row in rows}
    explicit_pin_ids = {str(pin.get("id", "")) for pin in explicit_pins}
    implicit_pin_ids = connected_pin_ids - explicit_pin_ids
    labeled_nets = [net for net in nets if net.get("net_labels")]
    return {
        "sheet_count": len(sheets),
        "component_unit_count": len(components),
        "physical_component_ref_count": len(
            {str(component.get("physical_ref", "")) for component in components}
        ),
        "explicit_pin_entry_count": len(explicit_pins),
        "unique_explicit_pin_id_count": len(explicit_pin_ids),
        "net_pin_membership_count": len(rows),
        "unique_connected_pin_id_count": len(connected_pin_ids),
        "unique_implicit_pin_id_count": len(implicit_pin_ids),
        "sheet_net_entry_count": len(nets),
        "distinct_net_name_count": len({str(net.get("name", "")) for net in nets}),
        "labeled_sheet_net_count": len(labeled_nets),
        "distinct_net_label_name_count": len(
            {str(net.get("name", "")) for net in labeled_nets}
        ),
        "net_label_placement_count": sum(
            len(_require_list(net.get("net_labels"), "net_labels"))
            for net in labeled_nets
        ),
    }


def _validate_model(model: dict[str, Any], rows: list[dict[str, Any]]) -> None:
    if model.get("schema") != SCHEMA_NAME:
        raise GenerationError(
            f"Unsupported schema {model.get('schema')!r}; expected {SCHEMA_NAME!r}"
        )
    validation = _require_mapping(model.get("validation"), "validation")
    if validation.get("status") != "passed":
        raise GenerationError("Extractor validation status is not 'passed'")

    declared_summary = _require_mapping(model.get("summary"), "summary")
    for name, expected in EXPECTED_SUMMARY.items():
        actual = declared_summary.get(name)
        if actual != expected:
            raise GenerationError(
                f"Summary {name!r} is {actual!r}; expected {expected!r}"
            )

    recomputed = _recomputed_summary(model, rows)
    for name, actual in recomputed.items():
        declared = declared_summary.get(name)
        if declared != actual:
            raise GenerationError(
                f"Summary {name!r} is {declared!r}, but model data gives {actual!r}"
            )


def _csv_payload(rows: list[dict[str, Any]]) -> str:
    stream = io.StringIO(newline="")
    writer = csv.DictWriter(
        stream,
        fieldnames=CSV_FIELDS,
        extrasaction="raise",
        lineterminator="\n",
    )
    writer.writeheader()
    writer.writerows(rows)
    return stream.getvalue()


def _component_key(row: dict[str, Any]) -> tuple[str, str]:
    if row["component_ref"]:
        return ("component", row["component_ref"])
    return ("physical", row["physical_ref"])


def _component_label(row: dict[str, Any]) -> str:
    return row["component_ref"] or row["physical_ref"]


def _graph_payload(
    graph_name: str,
    graph_label: str,
    rows: list[dict[str, Any]],
    source_filename: str,
    page_number: int | None,
) -> str:
    component_rows: dict[tuple[str, str], dict[str, Any]] = {}
    for row in rows:
        key = _component_key(row)
        current = component_rows.get(key)
        if current is None or (
            not current["component_ref"] and row["component_ref"]
        ):
            component_rows[key] = row

    component_keys = sorted(
        component_rows,
        key=lambda key: (
            _natural_key(_component_label(component_rows[key])),
            key[0],
            key[1],
        ),
    )
    net_names = sorted({row["net_name"] for row in rows}, key=_natural_key)
    component_ids = {
        key: f"component_{index:04d}"
        for index, key in enumerate(component_keys, start=1)
    }
    net_ids = {
        net_name: f"net_{index:04d}"
        for index, net_name in enumerate(net_names, start=1)
    }

    lines = [f"graph {_dot_quote(graph_name)} {{"]
    graph_attributes = [
        f"label={_dot_quote(graph_label)}",
        'labelloc="t"',
        f"source_file={_dot_quote(source_filename)}",
    ]
    if page_number is not None:
        graph_attributes.append(f"source_page={_dot_quote(page_number)}")
    lines.append(f"  graph [{', '.join(graph_attributes)}];")
    lines.append('  node [fontname="Arial"];')
    lines.append('  edge [fontname="Arial"];')

    for key in component_keys:
        row = component_rows[key]
        attributes = {
            "bipartite_side": "component",
            "fillcolor": "#fff2cc",
            "implicit_only": "true" if not row["component_ref"] else "false",
            "label": _component_label(row),
            "physical_ref": row["physical_ref"],
            "shape": "box",
            "style": "filled",
            "unit_suffix": row["unit_suffix"],
        }
        rendered = ", ".join(
            f"{name}={_dot_quote(value)}" for name, value in sorted(attributes.items())
        )
        lines.append(f"  {_dot_quote(component_ids[key])} [{rendered}];")

    for net_name in net_names:
        attributes = {
            "bipartite_side": "net",
            "fillcolor": "#d9eaf7",
            "label": net_name,
            "shape": "ellipse",
            "style": "filled",
        }
        rendered = ", ".join(
            f"{name}={_dot_quote(value)}" for name, value in sorted(attributes.items())
        )
        lines.append(f"  {_dot_quote(net_ids[net_name])} [{rendered}];")

    for row in rows:
        attributes = {
            "implicit": row["implicit"],
            "label": row["pin_number"],
            "page": row["page_number"],
            "pin_id": row["pin_id"],
            "sheet": row["sheet_name"],
        }
        rendered = ", ".join(
            f"{name}={_dot_quote(value)}" for name, value in sorted(attributes.items())
        )
        lines.append(
            f"  {_dot_quote(component_ids[_component_key(row)])} -- "
            f"{_dot_quote(net_ids[row['net_name']])} [{rendered}];"
        )

    lines.append("}")
    return "\n".join(lines) + "\n"


def _write_outputs(
    model: dict[str, Any],
    rows: list[dict[str, Any]],
    output_dir: Path,
) -> list[Path]:
    output_dir = output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    output_paths = []

    csv_path = output_dir / CSV_FILENAME
    _atomic_write_text(csv_path, _csv_payload(rows))
    output_paths.append(csv_path)

    source = _require_mapping(model.get("source"), "source")
    source_filename_value = str(source.get("filename", ""))
    source_filename = Path(source_filename_value).name
    if not source_filename:
        raise GenerationError("Input model source filename is missing")

    sheets = sorted(
        _require_list(model.get("sheets"), "sheets"),
        key=lambda value: int(_require_mapping(value, "sheet").get("page_number", 0)),
    )
    for sheet_value in sheets:
        sheet = _require_mapping(sheet_value, "sheet")
        page_number = int(sheet["page_number"])
        sheet_name = str(sheet["name"])
        sheet_rows = [row for row in rows if row["page_number"] == page_number]
        dot_name = f"{page_number:02d}-{_safe_slug(sheet_name)}.dot"
        dot_path = output_dir / dot_name
        _atomic_write_text(
            dot_path,
            _graph_payload(
                graph_name=f"sheet_{page_number:02d}",
                graph_label=sheet_name,
                rows=sheet_rows,
                source_filename=source_filename,
                page_number=page_number,
            ),
        )
        output_paths.append(dot_path)

    index_path = output_dir / INDEX_DOT_FILENAME
    _atomic_write_text(
        index_path,
        _graph_payload(
            graph_name="sc02_whole_design",
            graph_label="SC-02 whole design",
            rows=rows,
            source_filename=source_filename,
            page_number=None,
        ),
    )
    output_paths.append(index_path)
    return output_paths


def generate(input_json: Path, output_dir: Path) -> tuple[int, list[Path]]:
    model = _load_model(input_json.resolve())
    if model.get("schema") != SCHEMA_NAME:
        raise GenerationError(
            f"Unsupported schema {model.get('schema')!r}; expected {SCHEMA_NAME!r}"
        )
    rows = _build_membership_rows(model)
    _validate_model(model, rows)
    output_paths = _write_outputs(model, rows, output_dir)
    return len(rows), output_paths


def _build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Generate SC-02 pin/net CSV and component-to-net Graphviz DOT files "
            "from extractor JSON."
        )
    )
    parser.add_argument("input_json", type=Path, help="extractor JSON input")
    parser.add_argument(
        "--output-dir",
        required=True,
        type=Path,
        help="directory for CSV and DOT outputs",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_argument_parser().parse_args(argv)
    try:
        row_count, output_paths = generate(args.input_json, args.output_dir)
    except (GenerationError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    dot_count = sum(path.suffix.casefold() == ".dot" for path in output_paths)
    print(
        f"wrote {row_count} CSV rows and {dot_count} DOT files "
        f"to {args.output_dir}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
