#!/usr/bin/env python3
"""Generate a deterministic KiCad 10 schematic set from extracted SC-02 JSON.

The input is intentionally smaller than a KiCad data model.  It records source
page geometry, component and pin anchors, and net membership.  This generator
keeps those facts and does not invent routed wires: every connected pin gets a
global label directly on its connection point.  Representative visual-label
data remains in the source JSON and SVG views and is not made electrical here.

Format references:
  https://dev-docs.kicad.org/en/file-formats/sexpr-schematic/
  https://dev-docs.kicad.org/en/file-formats/sexpr-symbol-lib/
  https://dev-docs.kicad.org/en/file-formats/sexpr-intro/
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import re
import sys
import uuid
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterable, Mapping, Sequence


PROJECT_NAME = "sc02_prototype"
LIB_NICKNAME = "SC02_Generic"
LIB_FILENAME = "sc02_generic.kicad_sym"
FORMAT_VERSION = 20260306
SYMBOL_LIB_FORMAT_VERSION = 20251024
GENERATOR = "sc02_kicad_generate"
GENERATOR_VERSION = "1.0"
PT_TO_MM = 25.4 / 72.0
PAGE_MARGIN_MM = 10.0
UUID_NAMESPACE = uuid.UUID("df285411-0d7c-57e7-a3eb-69c4711f2d65")
ARCHIVE_SCHEMA = "appletini.sc02_schematic.v1"
ARCHIVE_SOURCE_SHA256 = "d0e05ea01fc5e823571e140fce5ce9f12a48f1993484d683f075151382adba35"
ARCHIVE_SOURCE_BYTES = 5_080_426
ARCHIVE_MEMBERSHIP_SHA256 = "b13d422a2b0338a3080a5e16b092374ad5afddbb8699cf532b3d418d7cdd208f"
EXPECTED_ARCHIVE_SUMMARY = {
    "sheet_count": 7,
    "component_unit_count": 785,
    "physical_component_ref_count": 484,
    "explicit_pin_entry_count": 3213,
    "unique_explicit_pin_id_count": 3213,
    "net_pin_membership_count": 3519,
    "unique_connected_pin_id_count": 3421,
    "unique_implicit_pin_id_count": 208,
    "implicit_pin_index_count": 208,
    "sheet_net_entry_count": 1257,
    "distinct_net_name_count": 1088,
    "labeled_sheet_net_count": 383,
    "distinct_net_label_name_count": 234,
    "net_label_placement_count": 718,
    "styled_text_span_count": 15330,
    "toc_entry_count": 11154,
    "membership_tuple_sha256": ARCHIVE_MEMBERSHIP_SHA256,
}
EXPECTED_ARCHIVE_SHEETS = [
    ("analog_1.SchDoc", 1),
    ("analog_2.SchDoc", 2),
    ("digital_1.SchDoc", 3),
    ("digital_2.SchDoc", 4),
    ("digital_3.SchDoc", 5),
    ("digital_4.SchDoc", 6),
    ("digital_5.SchDoc", 7),
]


class InputError(ValueError):
    """Raised when normalized input is incomplete or ambiguous."""


@dataclass
class Pin:
    source_id: str
    number: str
    anchor_pt: tuple[float, float]
    ordinal: int
    implicit: bool = False
    component: "Component | None" = field(default=None, repr=False)
    net_name: str | None = None

    @property
    def key(self) -> str:
        assert self.component is not None
        return f"{self.component.key}/pin/{self.ordinal}/{self.source_id}/{self.number}"


@dataclass
class Component:
    reference: str
    anchor_pt: tuple[float, float]
    pins: list[Pin]
    ordinal: int
    physical_reference: str = ""
    unit_suffix: str = ""
    sheet: "Sheet | None" = field(default=None, repr=False)
    symbol_name: str = ""

    @property
    def key(self) -> str:
        assert self.sheet is not None
        return f"{self.sheet.key}/component/{self.ordinal}/{self.reference}"


@dataclass
class Net:
    name: str
    pin_specs: list[Any]


@dataclass
class Sheet:
    name: str
    page_number: str
    page_size_pt: tuple[float, float]
    components: list[Component]
    nets: list[Net]
    ordinal: int = 0
    leaf_filename: str = ""
    sheet_uuid: str = ""

    @property
    def key(self) -> str:
        return f"sheet/{self.ordinal}/{self.page_number}/{self.name}"


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":"))


def natural_key(value: Any) -> tuple[Any, ...]:
    parts = re.split(r"(\d+)", str(value))
    return tuple(int(part) if part.isdigit() else part.casefold() for part in parts)


def sexpr_string(value: Any) -> str:
    text = str(value)
    text = text.replace("\\", "\\\\").replace('"', '\\"')
    text = text.replace("\r", "\\r").replace("\n", "\\n").replace("\t", "\\t")
    return f'"{text}"'


def fmt_number(value: float) -> str:
    if not math.isfinite(value):
        raise InputError(f"non-finite coordinate: {value!r}")
    if abs(value) < 0.00005:
        value = 0.0
    result = f"{value:.4f}".rstrip("0").rstrip(".")
    return result or "0"


def stable_uuid(*parts: Any) -> str:
    key = "\x1f".join(str(part) for part in parts)
    return str(uuid.uuid5(UUID_NAMESPACE, key))


def slug(value: str) -> str:
    result = re.sub(r"[^a-z0-9]+", "_", value.casefold()).strip("_")
    return result[:48] or "sheet"


def require_mapping(value: Any, where: str) -> Mapping[str, Any]:
    if not isinstance(value, Mapping):
        raise InputError(f"{where} must be an object")
    return value


def require_list(value: Any, where: str) -> list[Any]:
    if not isinstance(value, list):
        raise InputError(f"{where} must be an array")
    return value


def require_text(value: Any, where: str) -> str:
    if value is None:
        raise InputError(f"{where} is required")
    result = str(value).strip()
    if not result:
        raise InputError(f"{where} must not be empty")
    return result


def archive_membership_digest(root: Mapping[str, Any]) -> str:
    """Hash the memberships in canonical archive form, without trusting its summary."""

    tuples: list[list[str]] = []
    for sheet_index, raw_sheet_value in enumerate(require_list(root.get("sheets"), "input.sheets")):
        sheet_where = f"input.sheets[{sheet_index}]"
        raw_sheet = require_mapping(raw_sheet_value, sheet_where)
        sheet_name = require_text(raw_sheet.get("name"), f"{sheet_where}.name")
        owners: dict[str, str] = {}
        for component_index, raw_component_value in enumerate(
            require_list(raw_sheet.get("components"), f"{sheet_where}.components")
        ):
            component_where = f"{sheet_where}.components[{component_index}]"
            raw_component = require_mapping(raw_component_value, component_where)
            reference = require_text(
                raw_component.get("ref", raw_component.get("reference")),
                f"{component_where}.ref",
            )
            for pin_index, raw_pin_value in enumerate(
                require_list(raw_component.get("pins"), f"{component_where}.pins")
            ):
                pin_where = f"{component_where}.pins[{pin_index}]"
                raw_pin = require_mapping(raw_pin_value, pin_where)
                pin_id = require_text(raw_pin.get("id"), f"{pin_where}.id")
                if pin_id in owners:
                    raise InputError(f"{pin_where}: duplicate pin ID {pin_id!r}")
                owners[pin_id] = reference
        for pin_index, raw_pin_value in enumerate(
            require_list(raw_sheet.get("implicit_pins"), f"{sheet_where}.implicit_pins")
        ):
            pin_where = f"{sheet_where}.implicit_pins[{pin_index}]"
            raw_pin = require_mapping(raw_pin_value, pin_where)
            pin_id = require_text(raw_pin.get("id"), f"{pin_where}.id")
            owner = require_text(raw_pin.get("physical_ref"), f"{pin_where}.physical_ref")
            if pin_id in owners:
                raise InputError(f"{pin_where}: duplicate pin ID {pin_id!r}")
            owners[pin_id] = owner
        connected_ids: list[str] = []
        for net_index, raw_net_value in enumerate(
            require_list(raw_sheet.get("nets"), f"{sheet_where}.nets")
        ):
            net_where = f"{sheet_where}.nets[{net_index}]"
            raw_net = require_mapping(raw_net_value, net_where)
            net_name = require_text(raw_net.get("name"), f"{net_where}.name")
            for pin_index, raw_pin_id in enumerate(
                require_list(raw_net.get("pin_ids"), f"{net_where}.pin_ids")
            ):
                pin_id = require_text(raw_pin_id, f"{net_where}.pin_ids[{pin_index}]")
                owner = owners.get(pin_id)
                if owner is None:
                    raise InputError(f"{net_where}: unknown pin ID {pin_id!r}")
                connected_ids.append(pin_id)
                tuples.append([sheet_name, owner, pin_id, net_name])
        if Counter(connected_ids) != Counter(owners.keys()):
            missing = sorted(set(owners) - set(connected_ids), key=natural_key)
            repeated = sorted(
                (pin_id for pin_id, count in Counter(connected_ids).items() if count != 1),
                key=natural_key,
            )
            detail = f"missing={missing[:1]!r}, repeated={repeated[:1]!r}"
            raise InputError(f"{sheet_where}: every pin must occur on exactly one net ({detail})")
    payload = json.dumps(
        sorted(tuples),
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def validate_archive_input(root: Mapping[str, Any]) -> None:
    """Require the exact checked SC-02 archive before making the saved KiCad view."""

    if root.get("schema") != ARCHIVE_SCHEMA:
        raise InputError(
            f"input.schema must be {ARCHIVE_SCHEMA!r}; use --allow-normalized-input only for tests"
        )
    source = require_mapping(root.get("source"), "input.source")
    if source.get("sha256") != ARCHIVE_SOURCE_SHA256:
        raise InputError("input.source SHA-256 does not match the archived SC-02 PDF")
    if source.get("byte_length") != ARCHIVE_SOURCE_BYTES:
        raise InputError("input.source byte length does not match the archived SC-02 PDF")
    if root.get("summary") != EXPECTED_ARCHIVE_SUMMARY:
        raise InputError("input.summary does not match the checked SC-02 extraction")

    validation = require_mapping(root.get("validation"), "input.validation")
    checks = require_list(validation.get("checks"), "input.validation.checks")
    if validation.get("status") != "passed" or len(checks) != 34:
        raise InputError("input validation did not pass the expected 34 extractor checks")
    for check_index, check_value in enumerate(checks):
        check = require_mapping(check_value, f"input.validation.checks[{check_index}]")
        if check.get("passed") is not True:
            raise InputError(f"input validation check {check.get('name', check_index)!r} did not pass")

    raw_sheets = require_list(root.get("sheets"), "input.sheets")
    actual_sheets = []
    for index, sheet_value in enumerate(raw_sheets):
        sheet = require_mapping(sheet_value, f"input.sheets[{index}]")
        actual_sheets.append(
            (
                require_text(sheet.get("name"), f"input.sheets[{index}].name"),
                sheet.get("page_number"),
            )
        )
    if actual_sheets != EXPECTED_ARCHIVE_SHEETS:
        raise InputError("input sheet names, order, or page numbers changed")

    components = [
        require_mapping(component, f"input.sheets[{sheet_index}].components[{component_index}]")
        for sheet_index, sheet_value in enumerate(raw_sheets)
        for component_index, component in enumerate(
            require_list(
                require_mapping(sheet_value, f"input.sheets[{sheet_index}]").get("components"),
                f"input.sheets[{sheet_index}].components",
            )
        )
    ]
    explicit_pins = [
        require_mapping(pin, f"component[{component_index}].pins[{pin_index}]")
        for component_index, component in enumerate(components)
        for pin_index, pin in enumerate(
            require_list(component.get("pins"), f"component[{component_index}].pins")
        )
    ]
    implicit_pins = [
        require_mapping(pin, f"input.sheets[{sheet_index}].implicit_pins[{pin_index}]")
        for sheet_index, sheet_value in enumerate(raw_sheets)
        for pin_index, pin in enumerate(
            require_list(
                require_mapping(sheet_value, f"input.sheets[{sheet_index}]").get("implicit_pins"),
                f"input.sheets[{sheet_index}].implicit_pins",
            )
        )
    ]
    nets = [
        require_mapping(net, f"input.sheets[{sheet_index}].nets[{net_index}]")
        for sheet_index, sheet_value in enumerate(raw_sheets)
        for net_index, net in enumerate(
            require_list(
                require_mapping(sheet_value, f"input.sheets[{sheet_index}]").get("nets"),
                f"input.sheets[{sheet_index}].nets",
            )
        )
    ]
    membership_count = sum(
        len(require_list(net.get("pin_ids"), f"net[{net_index}].pin_ids"))
        for net_index, net in enumerate(nets)
    )
    physical_references = {
        require_text(
            component.get("physical_ref", component.get("ref", component.get("reference"))),
            f"component[{component_index}].physical_ref",
        )
        for component_index, component in enumerate(components)
    }
    actual_counts = {
        "components": len(components),
        "physical_references": len(physical_references),
        "explicit_pins": len(explicit_pins),
        "implicit_pin_memberships": len(implicit_pins),
        "nets": len(nets),
        "memberships": membership_count,
        "distinct_net_names": len(
            {
                require_text(net.get("name"), f"net[{net_index}].name")
                for net_index, net in enumerate(nets)
            }
        ),
    }
    expected_counts = {
        "components": 785,
        "physical_references": 484,
        "explicit_pins": 3213,
        "implicit_pin_memberships": 306,
        "nets": 1257,
        "memberships": 3519,
        "distinct_net_names": 1088,
    }
    if actual_counts != expected_counts:
        raise InputError(f"actual archive structure counts changed: {actual_counts!r}")

    stored_digest = require_mapping(root.get("membership_digest"), "input.membership_digest")
    if stored_digest.get("sha256") != ARCHIVE_MEMBERSHIP_SHA256:
        raise InputError("stored membership digest does not match the checked SC-02 extraction")
    actual_digest = archive_membership_digest(root)
    if actual_digest != ARCHIVE_MEMBERSHIP_SHA256:
        raise InputError("actual membership tuples do not match the checked SC-02 extraction")


def parse_point(value: Any, where: str) -> tuple[float, float]:
    if isinstance(value, Mapping):
        if "x" not in value or "y" not in value:
            raise InputError(f"{where} must contain x and y")
        raw_x, raw_y = value["x"], value["y"]
    elif isinstance(value, Sequence) and not isinstance(value, (str, bytes)) and len(value) == 2:
        raw_x, raw_y = value
    else:
        raise InputError(f"{where} must be [x, y] or {{x, y}}")
    try:
        point = (float(raw_x), float(raw_y))
    except (TypeError, ValueError) as exc:
        raise InputError(f"{where} contains a non-number") from exc
    if not all(math.isfinite(axis) for axis in point):
        raise InputError(f"{where} contains a non-finite number")
    return point


def parse_page_size(value: Any, where: str) -> tuple[float, float]:
    if isinstance(value, Mapping):
        raw = [value.get("width", value.get("w")), value.get("height", value.get("h"))]
    else:
        raw = value
    width, height = parse_point(raw, where)
    if width <= 0 or height <= 0:
        raise InputError(f"{where} dimensions must be positive")
    return width, height


def parse_record_anchor(record: Mapping[str, Any], where: str) -> tuple[float, float]:
    if record.get("anchor_pt") is not None:
        return parse_point(record["anchor_pt"], f"{where}.anchor_pt")
    anchors = require_list(record.get("anchors"), f"{where}.anchors")
    if not anchors:
        raise InputError(f"{where}.anchors must not be empty")
    anchor = require_mapping(anchors[0], f"{where}.anchors[0]")
    if anchor.get("center_points") is None:
        raise InputError(f"{where}.anchors[0].center_points is required")
    return parse_point(anchor["center_points"], f"{where}.anchors[0].center_points")


def parse_input(raw: Any) -> tuple[Any, list[Sheet]]:
    root = require_mapping(raw, "input")
    if "source" not in root:
        raise InputError("input.source is required")
    source = root["source"]
    raw_sheets = require_list(root.get("sheets"), "input.sheets")
    if not raw_sheets:
        raise InputError("input.sheets must not be empty")

    sheets: list[Sheet] = []
    for source_sheet_index, raw_sheet_value in enumerate(raw_sheets):
        where = f"input.sheets[{source_sheet_index}]"
        raw_sheet = require_mapping(raw_sheet_value, where)
        name = require_text(raw_sheet.get("name"), f"{where}.name")
        page_number = require_text(raw_sheet.get("page_number"), f"{where}.page_number")
        raw_page_size = raw_sheet.get("page_size_pt", raw_sheet.get("page_size_points"))
        page_size = parse_page_size(raw_page_size, f"{where}.page_size_pt")

        components: list[Component] = []
        for source_component_index, raw_component_value in enumerate(
            require_list(raw_sheet.get("components", []), f"{where}.components")
        ):
            component_where = f"{where}.components[{source_component_index}]"
            raw_component = require_mapping(raw_component_value, component_where)
            reference = require_text(
                raw_component.get("reference", raw_component.get("ref")),
                f"{component_where}.reference",
            )
            physical_reference = require_text(
                raw_component.get("physical_ref", reference),
                f"{component_where}.physical_ref",
            )
            unit_suffix = str(raw_component.get("unit_suffix") or "")
            anchor = parse_record_anchor(raw_component, component_where)
            pins: list[Pin] = []
            for source_pin_index, raw_pin_value in enumerate(
                require_list(raw_component.get("pins", []), f"{component_where}.pins")
            ):
                pin_where = f"{component_where}.pins[{source_pin_index}]"
                raw_pin = require_mapping(raw_pin_value, pin_where)
                source_id = require_text(raw_pin.get("id"), f"{pin_where}.id")
                number = require_text(raw_pin.get("number"), f"{pin_where}.number")
                pin_anchor = parse_record_anchor(raw_pin, pin_where)
                pins.append(Pin(source_id, number, pin_anchor, source_pin_index))
            pins.sort(key=lambda pin: (natural_key(pin.number), natural_key(pin.source_id), pin.anchor_pt))
            for pin_ordinal, pin in enumerate(pins):
                pin.ordinal = pin_ordinal
            components.append(
                Component(
                    reference,
                    anchor,
                    pins,
                    source_component_index,
                    physical_reference,
                    unit_suffix,
                )
            )

        # The PDF archive records hidden power and control pins separately.  Put
        # each on the closest unit of its physical part.  Its connection point
        # stays at the exact source anchor because symbol pin coordinates are
        # stored relative to the chosen component anchor.
        for source_pin_index, raw_pin_value in enumerate(
            require_list(raw_sheet.get("implicit_pins", []), f"{where}.implicit_pins")
        ):
            pin_where = f"{where}.implicit_pins[{source_pin_index}]"
            raw_pin = require_mapping(raw_pin_value, pin_where)
            source_id = require_text(raw_pin.get("id"), f"{pin_where}.id")
            number = require_text(raw_pin.get("number"), f"{pin_where}.number")
            physical_reference = require_text(
                raw_pin.get("physical_ref"), f"{pin_where}.physical_ref"
            )
            pin_anchor = parse_record_anchor(raw_pin, pin_where)
            candidates = [
                component
                for component in components
                if component.physical_reference == physical_reference
            ]
            if not candidates:
                raise InputError(
                    f"{pin_where}: no component has physical_ref {physical_reference!r}"
                )

            def implicit_owner_key(component: Component) -> tuple[Any, ...]:
                anchors = [pin.anchor_pt for pin in component.pins] or [component.anchor_pt]
                distance = min(math.dist(pin_anchor, anchor) for anchor in anchors)
                return distance, natural_key(component.reference), component.ordinal

            owner = min(candidates, key=implicit_owner_key)
            owner.pins.append(
                Pin(source_id, number, pin_anchor, len(owner.pins), implicit=True)
            )

        for component in components:
            component.pins.sort(
                key=lambda pin: (
                    natural_key(pin.number),
                    natural_key(pin.source_id),
                    pin.anchor_pt,
                    pin.implicit,
                )
            )
            for pin_ordinal, pin in enumerate(component.pins):
                pin.ordinal = pin_ordinal

        components.sort(
            key=lambda component: (
                natural_key(component.reference),
                component.anchor_pt,
                canonical_json(
                    [(pin.source_id, pin.number, pin.anchor_pt) for pin in component.pins]
                ),
            )
        )

        nets: list[Net] = []
        for source_net_index, raw_net_value in enumerate(
            require_list(raw_sheet.get("nets", []), f"{where}.nets")
        ):
            net_where = f"{where}.nets[{source_net_index}]"
            raw_net = require_mapping(raw_net_value, net_where)
            net_name = require_text(raw_net.get("name"), f"{net_where}.name")
            pin_specs = require_list(
                raw_net.get("pins", raw_net.get("pin_ids", [])),
                f"{net_where}.pins",
            )
            # Source label occurrences and representative anchors belong to
            # the archive/SVG view.  They are deliberately not electrical
            # objects in KiCad; pin-attached labels below define each net.
            nets.append(Net(net_name, pin_specs))
        nets.sort(key=lambda net: natural_key(net.name))
        sheets.append(Sheet(name, page_number, page_size, components, nets))

    sheets.sort(key=lambda sheet: (natural_key(sheet.page_number), natural_key(sheet.name)))
    for sheet_ordinal, sheet in enumerate(sheets):
        sheet.ordinal = sheet_ordinal
        sheet.leaf_filename = f"sc02_sheet_{sheet_ordinal + 1:02d}_{slug(sheet.name)}.kicad_sch"
        sheet.sheet_uuid = stable_uuid(source_key(source), sheet.key, "hierarchical-sheet")
        for component_ordinal, component in enumerate(sheet.components):
            component.ordinal = component_ordinal
            component.sheet = sheet
            for pin in component.pins:
                pin.component = component
            component.symbol_name = make_symbol_name(sheet, component)
        bind_nets(sheet)
    return source, sheets


def source_key(source: Any) -> str:
    return source if isinstance(source, str) else canonical_json(source)


def make_symbol_name(sheet: Sheet, component: Component) -> str:
    safe_ref = re.sub(r"[^A-Za-z0-9_]+", "_", component.reference).strip("_") or "COMP"
    digest = stable_uuid(sheet.key, component.ordinal, component.reference).replace("-", "")[:8]
    return f"S{sheet.ordinal + 1:02d}_{safe_ref}_{digest}"


def pin_aliases(pin: Pin) -> set[str]:
    assert pin.component is not None
    ref = pin.component.reference
    return {
        pin.source_id,
        f"{ref}.{pin.number}",
        f"{ref}:{pin.number}",
        f"{ref}/{pin.number}",
        f"{ref}#{pin.number}",
        f"{ref}.{pin.source_id}",
        f"{ref}:{pin.source_id}",
        f"{ref}/{pin.source_id}",
    }


def resolve_pin_spec(
    spec: Any,
    aliases: Mapping[str, list[Pin]],
    sheet: Sheet,
    where: str,
) -> Pin:
    candidates: list[Pin]
    display = canonical_json(spec) if not isinstance(spec, str) else spec
    if isinstance(spec, str):
        candidates = aliases.get(spec, [])
    elif isinstance(spec, Sequence) and not isinstance(spec, (str, bytes)) and len(spec) == 2:
        ref, pin_selector = str(spec[0]), str(spec[1])
        candidates = [
            pin
            for component in sheet.components
            if component.reference == ref
            for pin in component.pins
            if pin.number == pin_selector or pin.source_id == pin_selector
        ]
    elif isinstance(spec, Mapping):
        ref_value = spec.get("reference", spec.get("ref", spec.get("component")))
        id_value = spec.get("id", spec.get("pin_id"))
        number_value = spec.get("number")
        generic_pin = spec.get("pin")
        if number_value is None and id_value is None and generic_pin is not None:
            number_value = generic_pin
        if ref_value is None:
            token = id_value if id_value is not None else number_value
            candidates = aliases.get(str(token), []) if token is not None else []
        else:
            ref = str(ref_value)
            candidates = [pin for component in sheet.components if component.reference == ref for pin in component.pins]
            if id_value is not None:
                candidates = [pin for pin in candidates if pin.source_id == str(id_value)]
            if number_value is not None:
                candidates = [pin for pin in candidates if pin.number == str(number_value)]
    else:
        candidates = []

    unique = {pin.key: pin for pin in candidates}
    if not unique:
        raise InputError(f"{where}: pin reference {display!r} does not resolve on sheet {sheet.name!r}")
    if len(unique) != 1:
        matches = ", ".join(sorted(pin.key for pin in unique.values()))
        raise InputError(f"{where}: pin reference {display!r} is ambiguous: {matches}")
    return next(iter(unique.values()))


def bind_nets(sheet: Sheet) -> None:
    aliases: dict[str, list[Pin]] = defaultdict(list)
    for component in sheet.components:
        for pin in component.pins:
            for alias in pin_aliases(pin):
                aliases[alias].append(pin)

    seen_net_names: set[str] = set()
    for net_index, net in enumerate(sheet.nets):
        if net.name in seen_net_names:
            raise InputError(f"sheet {sheet.name!r} has duplicate net {net.name!r}")
        seen_net_names.add(net.name)
        for spec_index, spec in enumerate(net.pin_specs):
            pin = resolve_pin_spec(spec, aliases, sheet, f"{sheet.key}.nets[{net_index}].pins[{spec_index}]")
            if pin.net_name is not None and pin.net_name != net.name:
                raise InputError(
                    f"pin {pin.key} belongs to both net {pin.net_name!r} and {net.name!r}"
                )
            pin.net_name = net.name


def mm_from_pt(point: tuple[float, float], *, margin: bool = True) -> tuple[float, float]:
    offset = PAGE_MARGIN_MM if margin else 0.0
    return point[0] * PT_TO_MM + offset, point[1] * PT_TO_MM + offset


def effects(*, justify: str | None = None, hidden: bool = False, size: float = 1.27) -> str:
    parts = [f"(effects (font (size {fmt_number(size)} {fmt_number(size)}))"]
    if justify:
        parts.append(f" (justify {justify})")
    if hidden:
        parts.append(" (hide yes)")
    parts.append(")")
    return "".join(parts)


def render_property(
    name: str,
    value: Any,
    x: float,
    y: float,
    *,
    hidden: bool = False,
    justify: str | None = None,
    indent: str = "    ",
) -> str:
    hide_line = f"\n{indent}  (hide yes)" if hidden else ""
    return (
        f"{indent}(property {sexpr_string(name)} {sexpr_string(value)}\n"
        f"{indent}  (at {fmt_number(x)} {fmt_number(y)} 0)\n"
        f"{indent}  (show_name no)\n"
        f"{indent}  (do_not_autoplace no)"
        f"{hide_line}\n"
        f"{indent}  {effects(justify=justify)}\n"
        f"{indent})"
    )


def body_geometry(component: Component) -> tuple[float, float]:
    relative = [
        ((pin.anchor_pt[0] - component.anchor_pt[0]) * PT_TO_MM,
         (pin.anchor_pt[1] - component.anchor_pt[1]) * PT_TO_MM)
        for pin in component.pins
    ]
    horizontal = [(abs(dx), abs(dy)) for dx, dy in relative if abs(dx) >= abs(dy) and abs(dx) > 0.01]
    vertical = [(abs(dx), abs(dy)) for dx, dy in relative if abs(dy) > abs(dx) and abs(dy) > 0.01]
    half_w = max([3.81] + [dx + 1.27 for dx, _ in vertical])
    half_h = max([2.54] + [dy + 1.27 for _, dy in horizontal])
    if horizontal:
        half_w = min(half_w, max(1.27, min(dx for dx, _ in horizontal) - 0.508))
    if vertical:
        half_h = min(half_h, max(1.27, min(dy for _, dy in vertical) - 0.508))
    return min(half_w, 25.4), min(half_h, 25.4)


def pin_geometry(component: Component, pin: Pin, body: tuple[float, float]) -> tuple[float, float, int, float]:
    dx = (pin.anchor_pt[0] - component.anchor_pt[0]) * PT_TO_MM
    dy = (pin.anchor_pt[1] - component.anchor_pt[1]) * PT_TO_MM
    half_w, half_h = body
    if abs(dx) >= abs(dy):
        rotation = 0 if dx <= 0 else 180
        length = max(0.508, abs(dx) - half_w)
    else:
        # KiCad mirrors symbol-local Y when it places a symbol on a sheet.
        # render_lib_symbol() negates dy, so these rotations make each vertical
        # pin line point from its connection point back toward the body.
        rotation = 270 if dy <= 0 else 90
        length = max(0.508, abs(dy) - half_h)
    return dx, dy, rotation, min(length, 50.8)


def reference_prefix(reference: str) -> str:
    match = re.match(r"[A-Za-z]+", reference)
    return match.group(0) if match else "U"


def render_lib_symbol(component: Component, *, embedded: bool) -> str:
    base_name = component.symbol_name
    symbol_name = f"{LIB_NICKNAME}:{base_name}" if embedded else base_name
    body_name = f"{base_name}_0_1"
    pins_name = f"{base_name}_1_1"
    half_w, half_h = body_geometry(component)
    duplicate_numbers = any(count > 1 for count in Counter(pin.number for pin in component.pins).values())
    lines = [
        f"    (symbol {sexpr_string(symbol_name)}",
        "      (pin_names (offset 0.508))",
        "      (exclude_from_sim no)",
        "      (in_bom yes)",
        "      (on_board yes)",
        "      (in_pos_files no)",
        f"      (duplicate_pin_numbers_are_jumpers {'yes' if duplicate_numbers else 'no'})",
        f"      (property \"Reference\" {sexpr_string(reference_prefix(component.reference))}",
        f"        (at 0 {fmt_number(-half_h - 1.27)} 0)",
        "        (show_name no)",
        "        (do_not_autoplace no)",
        f"        {effects()}",
        "      )",
        f"      (property \"Value\" {sexpr_string(component.reference)}",
        f"        (at 0 {fmt_number(half_h + 1.27)} 0)",
        "        (show_name no)",
        "        (do_not_autoplace no)",
        f"        {effects()}",
        "      )",
        "      (property \"Footprint\" \"\"",
        "        (at 0 0 0)",
        "        (show_name no)",
        "        (do_not_autoplace no)",
        "        (hide yes)",
        f"        {effects()}",
        "      )",
        "      (property \"Datasheet\" \"\"",
        "        (at 0 0 0)",
        "        (show_name no)",
        "        (do_not_autoplace no)",
        "        (hide yes)",
        f"        {effects()}",
        "      )",
        "      (property \"Description\" \"Generic source-geometry component\"",
        "        (at 0 0 0)",
        "        (show_name no)",
        "        (do_not_autoplace no)",
        "        (hide yes)",
        f"        {effects()}",
        "      )",
        f"      (symbol {sexpr_string(body_name)}",
        "        (rectangle",
        f"          (start {fmt_number(-half_w)} {fmt_number(half_h)})",
        f"          (end {fmt_number(half_w)} {fmt_number(-half_h)})",
        "          (stroke (width 0.254) (type default))",
        "          (fill (type background))",
        "        )",
        "      )",
        f"      (symbol {sexpr_string(pins_name)}",
    ]
    for pin in component.pins:
        dx, dy, rotation, length = pin_geometry(component, pin, (half_w, half_h))
        component_x, component_y = mm_from_pt(component.anchor_pt)
        pin_x, pin_y = mm_from_pt(pin.anchor_pt)
        # KiCad stores both positions to four decimal places, then mirrors the
        # symbol-local Y.  Derive the local point from those stored sheet
        # coordinates so the placed pin and its page label are bit-exact.
        local_x = float(fmt_number(pin_x)) - float(fmt_number(component_x))
        local_y = float(fmt_number(component_y)) - float(fmt_number(pin_y))
        lines.extend(
            [
                "        (pin passive line",
                f"          (at {fmt_number(local_x)} {fmt_number(local_y)} {rotation})",
                f"          (length {fmt_number(length)})",
                f"          (name {sexpr_string(pin.source_id)} {effects()})",
                f"          (number {sexpr_string(pin.number)} {effects()})",
                "        )",
            ]
        )
    lines.extend(["      )", "      (embedded_fonts no)", "    )"])
    return "\n".join(lines)


def pin_label_orientation(component: Component, pin: Pin) -> tuple[int, str]:
    dx = pin.anchor_pt[0] - component.anchor_pt[0]
    dy = pin.anchor_pt[1] - component.anchor_pt[1]
    if abs(dx) >= abs(dy):
        if dx <= 0:
            return 180, "right"
        return 0, "left"
    if dy <= 0:
        return 270, "right"
    return 90, "left"


def render_global_label(
    net_name: str,
    x: float,
    y: float,
    angle: int,
    justify: str,
    object_key: str,
) -> str:
    property_angle = 0 if angle in (0, 180) else angle
    return "\n".join(
        [
            f"  (global_label {sexpr_string(net_name)}",
            "    (shape passive)",
            f"    (at {fmt_number(x)} {fmt_number(y)} {angle})",
            "    (fields_autoplaced yes)",
            f"    {effects(justify=justify)}",
            f"    (uuid {sexpr_string(stable_uuid(object_key, 'global-label'))})",
            "    (property \"Intersheetrefs\" \"${INTERSHEET_REFS}\"",
            f"      (at {fmt_number(x)} {fmt_number(y)} {property_angle})",
            f"      {effects(justify=justify, hidden=True)}",
            "    )",
            "  )",
        ]
    )


def render_symbol_instance(
    source: Any,
    root_uuid: str,
    sheet: Sheet,
    component: Component,
) -> str:
    x, y = mm_from_pt(component.anchor_pt)
    half_w, half_h = body_geometry(component)
    source_text = source_key(source)
    pin_map = [
        {
            "id": pin.source_id,
            "implicit": pin.implicit,
            "net": pin.net_name or "",
            "number": pin.number,
        }
        for pin in component.pins
    ]
    properties: list[tuple[str, Any, bool, float, float]] = [
        ("Reference", component.reference, False, x, y - half_h - 1.27),
        ("Value", component.reference, False, x, y + half_h + 1.27),
        ("Footprint", "", True, x, y),
        ("Datasheet", "", True, x, y),
        ("Description", "Generic source-geometry component", True, x, y),
        ("SC02.Source", source_text, True, x, y),
        ("SC02.SourceSheet", sheet.name, True, x, y),
        ("SC02.SourcePage", sheet.page_number, True, x, y),
        ("SC02.SourceReference", component.reference, True, x, y),
        ("SC02.SourcePhysicalReference", component.physical_reference, True, x, y),
        ("SC02.SourceUnitSuffix", component.unit_suffix, True, x, y),
        (
            "SC02.VisualLabelPlacement",
            "pin-attached global labels define connectivity; representative visual labels remain in JSON/SVG only",
            True,
            x,
            y,
        ),
        ("SC02.PinMap", canonical_json(pin_map), True, x, y),
    ]
    for index, pin in enumerate(component.pins, start=1):
        prefix = f"SC02.Pin.{index:03d}"
        properties.extend(
            [
                (f"{prefix}.Id", pin.source_id, True, x, y),
                (f"{prefix}.Number", pin.number, True, x, y),
                (f"{prefix}.Net", pin.net_name or "", True, x, y),
                (f"{prefix}.Implicit", "yes" if pin.implicit else "no", True, x, y),
            ]
        )

    lines = [
        "  (symbol",
        f"    (lib_id {sexpr_string(f'{LIB_NICKNAME}:{component.symbol_name}')})",
        f"    (at {fmt_number(x)} {fmt_number(y)} 0)",
        "    (unit 1)",
        "    (body_style 1)",
        "    (exclude_from_sim no)",
        "    (in_bom yes)",
        "    (on_board yes)",
        "    (in_pos_files no)",
        "    (dnp no)",
        f"    (uuid {sexpr_string(stable_uuid(source_text, component.key, 'symbol-instance'))})",
    ]
    lines.extend(
        render_property(name, value, px, py, hidden=hidden)
        for name, value, hidden, px, py in properties
    )
    for pin in component.pins:
        lines.append(
            f"    (pin {sexpr_string(pin.number)} (uuid {sexpr_string(stable_uuid(source_text, pin.key, 'pin-instance'))}))"
        )
    lines.extend(
        [
            "    (instances",
            f"      (project {sexpr_string(PROJECT_NAME)}",
            f"        (path {sexpr_string(f'/{root_uuid}/{sheet.sheet_uuid}')}",
            f"          (reference {sexpr_string(component.reference)})",
            "          (unit 1)",
            "        )",
            "      )",
            "    )",
            "  )",
        ]
    )
    return "\n".join(lines)


def render_header(file_uuid: str, paper: str) -> list[str]:
    return [
        "(kicad_sch",
        f"  (version {FORMAT_VERSION})",
        f"  (generator {sexpr_string(GENERATOR)})",
        f"  (generator_version {sexpr_string(GENERATOR_VERSION)})",
        f"  (uuid {sexpr_string(file_uuid)})",
        f"  {paper}",
    ]


def render_leaf(source: Any, root_uuid: str, sheet: Sheet) -> str:
    source_text = source_key(source)
    width_mm = sheet.page_size_pt[0] * PT_TO_MM + 2 * PAGE_MARGIN_MM
    height_mm = sheet.page_size_pt[1] * PT_TO_MM + 2 * PAGE_MARGIN_MM
    file_uuid = stable_uuid(source_text, sheet.key, "leaf-file")
    lines = render_header(
        file_uuid,
        f'(paper "User" {fmt_number(width_mm)} {fmt_number(height_mm)})',
    )
    lines.extend(
        [
            "  (title_block",
            f"    (title {sexpr_string(sheet.name)})",
            "    (rev \"generated\")",
            "    (company \"Votrax / Federal Screw Works prototype reconstruction\")",
            f"    (comment 1 {sexpr_string(f'Source page {sheet.page_number}')})",
            f"    (comment 2 {sexpr_string(source_text)})",
            "    (comment 3 \"Pin-attached global labels define nets; visual label data remains in JSON/SVG only.\")",
            "  )",
            "  (lib_symbols",
        ]
    )
    for component in sheet.components:
        lines.append(render_lib_symbol(component, embedded=True))
    lines.append("  )")

    for component in sheet.components:
        lines.append(render_symbol_instance(source, root_uuid, sheet, component))
        for pin in component.pins:
            pin_x, pin_y = mm_from_pt(pin.anchor_pt)
            if pin.net_name is None:
                lines.extend(
                    [
                        "  (no_connect",
                        f"    (at {fmt_number(pin_x)} {fmt_number(pin_y)})",
                        f"    (uuid {sexpr_string(stable_uuid(source_text, pin.key, 'no-connect'))})",
                        "  )",
                    ]
                )
                continue
            angle, justify = pin_label_orientation(component, pin)
            object_key = f"{source_text}/{pin.key}/{pin.net_name}"
            lines.append(
                render_global_label(
                    pin.net_name,
                    pin_x,
                    pin_y,
                    angle,
                    justify,
                    object_key,
                )
            )

    lines.extend(["  (embedded_fonts no)", ")", ""])
    return "\n".join(lines)


def render_root(source: Any, root_uuid: str, sheets: list[Sheet]) -> str:
    source_text = source_key(source)
    lines = render_header(root_uuid, '(paper "A4")')
    lines.extend(
        [
            "  (title_block",
            "    (title \"SSI-263 / SC-02 prototype schematic index\")",
            "    (rev \"generated\")",
            "    (company \"Votrax / Federal Screw Works prototype reconstruction\")",
            f"    (comment 1 {sexpr_string(source_text)})",
            "    (comment 2 \"Pin-attached global labels define nets; visual label data remains in JSON/SVG only.\")",
            "  )",
            "  (lib_symbols)",
        ]
    )

    for sheet_index, sheet in enumerate(sheets):
        column = sheet_index % 2
        row = sheet_index // 2
        x = 25.4 + column * 101.6
        y = 30.48 + row * 45.72
        width, height = 76.2, 25.4
        hierarchy_page = str(sheet_index + 2)
        lines.extend(
            [
                "  (sheet",
                f"    (at {fmt_number(x)} {fmt_number(y)})",
                f"    (size {fmt_number(width)} {fmt_number(height)})",
                "    (exclude_from_sim no)",
                "    (in_bom yes)",
                "    (on_board yes)",
                "    (dnp no)",
                "    (fields_autoplaced yes)",
                "    (stroke (width 0.1524) (type solid))",
                "    (fill (color 0 0 0 0))",
                f"    (uuid {sexpr_string(sheet.sheet_uuid)})",
                render_property("Sheetname", sheet.name, x + 1.27, y + 1.27, justify="left", indent="    "),
                render_property("Sheetfile", sheet.leaf_filename, x + 1.27, y + 3.81, justify="left", indent="    "),
                render_property("SC02.SourceSheet", sheet.name, x, y, hidden=True, indent="    "),
                render_property("SC02.SourcePage", sheet.page_number, x, y, hidden=True, indent="    "),
                render_property(
                    "SC02.VisualLabelPlacement",
                    "pin-attached global labels define connectivity; representative visual labels remain in JSON/SVG only",
                    x,
                    y,
                    hidden=True,
                    indent="    ",
                ),
                f"    (instances",
                f"      (project {sexpr_string(PROJECT_NAME)}",
                f"        (path {sexpr_string(f'/{root_uuid}')}",
                f"          (page {sexpr_string(hierarchy_page)})",
                "        )",
                "      )",
                "    )",
                "  )",
            ]
        )
    lines.extend(
        [
            "  (sheet_instances",
            "    (path \"/\" (page \"1\"))",
            "  )",
            "  (embedded_fonts no)",
            ")",
            "",
        ]
    )
    return "\n".join(lines)


def render_library(source: Any, sheets: list[Sheet]) -> str:
    lines = [
        "(kicad_symbol_lib",
        f"  (version {SYMBOL_LIB_FORMAT_VERSION})",
        f"  (generator {sexpr_string(GENERATOR)})",
        f"  (generator_version {sexpr_string(GENERATOR_VERSION)})",
    ]
    for sheet in sheets:
        for component in sheet.components:
            definition = render_lib_symbol(component, embedded=False)
            lines.append(definition[2:] if definition.startswith("  ") else definition)
    lines.extend([")", ""])
    return "\n".join(lines)


def render_sym_lib_table() -> str:
    return "\n".join(
        [
            "(sym_lib_table",
            "  (version 7)",
            f"  (lib (name {sexpr_string(LIB_NICKNAME)})(type \"KiCad\")(uri \"${{KIPRJMOD}}/{LIB_FILENAME}\")(options \"\")(descr \"SC-02 source-geometry generic symbols\"))",
            ")",
            "",
        ]
    )


def render_project(source: Any, root_uuid: str, sheets: list[Sheet]) -> str:
    project = {
        "board": {},
        "boards": [],
        "component_class_settings": {
            "assignments": [],
            "meta": {"version": 0},
            "sheet_component_classes": {"enabled": False},
        },
        "cvpcb": {},
        "erc": {},
        "libraries": {},
        "meta": {"filename": f"{PROJECT_NAME}.kicad_pro", "version": 3},
        "net_settings": {
            "classes": [
                {
                    "bus_width": 12,
                    "clearance": 0.2,
                    "diff_pair_gap": 0.25,
                    "diff_pair_via_gap": 0.25,
                    "diff_pair_width": 0.2,
                    "line_style": 0,
                    "microvia_diameter": 0.3,
                    "microvia_drill": 0.1,
                    "name": "Default",
                    "pcb_color": "rgba(0, 0, 0, 0.000)",
                    "priority": 2147483647,
                    "schematic_color": "rgba(0, 0, 0, 0.000)",
                    "track_width": 0.25,
                    "via_diameter": 0.8,
                    "via_drill": 0.4,
                    "wire_width": 6,
                }
            ],
            "meta": {"version": 5},
            "net_colors": None,
            "netclass_assignments": {},
            "netclass_patterns": [],
        },
        "pcbnew": {},
        "schematic": {
            "annotate_start_num": 0,
            "drawing": {},
            "legacy_lib_dir": "",
            "legacy_lib_list": [],
            "meta": {"version": 1},
            "net_format_name": "",
            "ngspice": {},
            "page_layout_descr_file": "",
            "plot_directory": "",
            "spice_adjust_passive_values": False,
            "spice_current_sheet_as_root": False,
            "spice_external_command": "spice \"%I\"",
            "spice_model_current_sheet_as_root": True,
            "spice_save_all_currents": False,
            "spice_save_all_dissipations": False,
            "spice_save_all_voltages": False,
            "subpart_first_id": 65,
            "subpart_id_separator": 0,
            "top_level_sheets": [
                {
                    "filename": f"{PROJECT_NAME}.kicad_sch",
                    "name": "Root",
                    "uuid": root_uuid,
                }
            ],
            "variants": [],
        },
        "sheets": [
            [root_uuid, "Root"],
            *[[sheet.sheet_uuid, sheet.name] for sheet in sheets],
        ],
        "text_variables": {
            "SC02_GENERATOR": f"{GENERATOR} {GENERATOR_VERSION}",
            "SC02_SOURCE": source_key(source),
            "SC02_VISUAL_LABEL_PLACEMENT": "pin-attached global labels define connectivity; representative visual labels remain in JSON/SVG only",
        },
        "tuning_profiles": {
            "meta": {"version": 0},
            "tuning_profiles_impedance_geometric": [],
        },
    }
    return json.dumps(project, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def validate_sexpr(text: str, expected_root: str, filename: str) -> None:
    match = re.match(r"\s*\(\s*([^\s()]+)", text)
    if match is None or match.group(1) != expected_root:
        actual = match.group(1) if match else "<none>"
        raise RuntimeError(f"{filename}: expected root {expected_root!r}, found {actual!r}")
    depth = 0
    in_string = False
    escaped = False
    for offset, character in enumerate(text):
        if in_string:
            if escaped:
                escaped = False
            elif character == "\\":
                escaped = True
            elif character == '"':
                in_string = False
            continue
        if character == '"':
            in_string = True
        elif character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
            if depth < 0:
                raise RuntimeError(f"{filename}: unmatched ')' at byte {offset}")
        elif ord(character) < 32 and character not in "\r\n\t":
            raise RuntimeError(f"{filename}: control character at byte {offset}")
    if in_string:
        raise RuntimeError(f"{filename}: unterminated quoted string")
    if depth != 0:
        raise RuntimeError(f"{filename}: unbalanced parentheses, final depth {depth}")
    object_uuids = re.findall(r'\(uuid\s+"([0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12})"\)', text)
    duplicates = sorted(value for value, count in Counter(object_uuids).items() if count > 1)
    if duplicates:
        raise RuntimeError(f"{filename}: duplicate object UUID(s): {', '.join(duplicates)}")


def atomic_write(path: Path, text: str) -> None:
    temporary = path.with_name(f".{path.name}.tmp")
    temporary.write_text(text, encoding="utf-8", newline="\n")
    os.replace(temporary, path)


def generate(
    input_path: Path,
    output_dir: Path,
    *,
    allow_normalized_input: bool = False,
) -> list[Path]:
    try:
        raw = json.loads(input_path.read_text(encoding="utf-8"))
    except OSError as exc:
        raise InputError(f"cannot read {input_path}: {exc}") from exc
    except json.JSONDecodeError as exc:
        raise InputError(f"invalid JSON in {input_path}: {exc}") from exc

    root = require_mapping(raw, "input")
    if not allow_normalized_input:
        validate_archive_input(root)
    source, sheets = parse_input(root)
    if not allow_normalized_input:
        all_components = [component for sheet in sheets for component in sheet.components]
        all_pins = [pin for component in all_components for pin in component.pins]
        all_nets = [net for sheet in sheets for net in sheet.nets]
        if len(all_components) != 785 or len(all_pins) != 3519 or len(all_nets) != 1257:
            raise InputError("parsed KiCad input counts do not match the checked archive")
        unbound = [pin.key for pin in all_pins if pin.net_name is None]
        if unbound:
            raise InputError(
                f"checked archive contains {len(unbound)} pin(s) without a net; first is {unbound[0]}"
            )
        if len({net.name for net in all_nets}) != 1088:
            raise InputError("parsed distinct net-name count does not match the checked archive")
    root_uuid = stable_uuid(source_key(source), PROJECT_NAME, "root-file")
    outputs: dict[str, tuple[str, str | None]] = {
        f"{PROJECT_NAME}.kicad_sch": (render_root(source, root_uuid, sheets), "kicad_sch"),
        LIB_FILENAME: (render_library(source, sheets), "kicad_symbol_lib"),
        "sym-lib-table": (render_sym_lib_table(), "sym_lib_table"),
        f"{PROJECT_NAME}.kicad_pro": (render_project(source, root_uuid, sheets), None),
    }
    for sheet in sheets:
        outputs[sheet.leaf_filename] = (render_leaf(source, root_uuid, sheet), "kicad_sch")

    for filename, (content, root_token) in outputs.items():
        if root_token is not None:
            validate_sexpr(content, root_token, filename)
        else:
            json.loads(content)

    output_dir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    for filename in sorted(outputs, key=natural_key):
        path = output_dir / filename
        atomic_write(path, outputs[filename][0])
        written.append(path)
    return written


def default_output_dir() -> Path:
    return Path(__file__).resolve().parents[1] / "schematics" / "sc02_prototype" / "kicad"


def build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("json", type=Path, help="canonical extracted SC-02 JSON input")
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=default_output_dir(),
        help="output directory (default: schematics/sc02_prototype/kicad)",
    )
    parser.add_argument(
        "--allow-normalized-input",
        action="store_true",
        help="accept a small non-archive input for generator tests only",
    )
    return parser


def main(argv: Iterable[str] | None = None) -> int:
    args = build_argument_parser().parse_args(argv)
    try:
        written = generate(
            args.json.resolve(),
            args.output_dir.resolve(),
            allow_normalized_input=args.allow_normalized_input,
        )
    except (InputError, OSError, RuntimeError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2
    for path in written:
        print(path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
