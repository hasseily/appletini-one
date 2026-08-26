#!/usr/bin/env python3
"""Extract the SC-02 Altium smart-PDF schematic into deterministic JSON.

The PDF bookmark tree is the source of truth for component and net
membership.  Positioned CO/PI/NL text tags provide drawing anchors and nearby
visible text provides review evidence.  No connectivity is inferred from line
crossings.

Requires the PyMuPDF version pinned in ``requirements-sc02-schematic.txt``.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import sys
from typing import Any, Iterable

try:
    import pymupdf as fitz
except ImportError:  # PyMuPDF releases before 1.24 expose only ``fitz``.
    try:
        import fitz  # type: ignore[no-redef]
    except ImportError as exc:  # pragma: no cover - depends on host setup.
        raise SystemExit(
            "PyMuPDF is required; install it with: "
            "python -m pip install -r scripts/requirements-sc02-schematic.txt"
        ) from exc


SCHEMA_NAME = "appletini.sc02_schematic.v1"
EXTRACTOR_VERSION = "1.1"
EXPECTED_PYMUPDF_VERSION = "1.27.2.3"
EXPECTED_SUMMARY = {
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
    "membership_tuple_sha256": (
        "b13d422a2b0338a3080a5e16b092374ad5afddbb8699cf532b3d418d7cdd208f"
    ),
}
EXPECTED_SHEET_COUNTS = {
    "analog_1.SchDoc": {
        "page_number": 1,
        "component_unit_count": 150,
        "explicit_pin_entry_count": 413,
        "unique_explicit_pin_id_count": 413,
        "implicit_pin_membership_count": 26,
        "net_entry_count": 119,
        "net_pin_membership_count": 439,
        "net_label_placement_count": 51,
        "styled_text_span_count": 1918,
    },
    "analog_2.SchDoc": {
        "page_number": 2,
        "component_unit_count": 192,
        "explicit_pin_entry_count": 518,
        "unique_explicit_pin_id_count": 518,
        "implicit_pin_membership_count": 28,
        "net_entry_count": 144,
        "net_pin_membership_count": 546,
        "net_label_placement_count": 62,
        "styled_text_span_count": 2383,
    },
    "digital_1.SchDoc": {
        "page_number": 3,
        "component_unit_count": 139,
        "explicit_pin_entry_count": 882,
        "unique_explicit_pin_id_count": 882,
        "implicit_pin_membership_count": 66,
        "net_entry_count": 331,
        "net_pin_membership_count": 948,
        "net_label_placement_count": 329,
        "styled_text_span_count": 4328,
    },
    "digital_2.SchDoc": {
        "page_number": 4,
        "component_unit_count": 100,
        "explicit_pin_entry_count": 338,
        "unique_explicit_pin_id_count": 338,
        "implicit_pin_membership_count": 32,
        "net_entry_count": 105,
        "net_pin_membership_count": 370,
        "net_label_placement_count": 62,
        "styled_text_span_count": 1574,
    },
    "digital_3.SchDoc": {
        "page_number": 5,
        "component_unit_count": 59,
        "explicit_pin_entry_count": 250,
        "unique_explicit_pin_id_count": 250,
        "implicit_pin_membership_count": 36,
        "net_entry_count": 116,
        "net_pin_membership_count": 286,
        "net_label_placement_count": 36,
        "styled_text_span_count": 1204,
    },
    "digital_4.SchDoc": {
        "page_number": 6,
        "component_unit_count": 79,
        "explicit_pin_entry_count": 371,
        "unique_explicit_pin_id_count": 371,
        "implicit_pin_membership_count": 62,
        "net_entry_count": 172,
        "net_pin_membership_count": 433,
        "net_label_placement_count": 60,
        "styled_text_span_count": 1778,
    },
    "digital_5.SchDoc": {
        "page_number": 7,
        "component_unit_count": 66,
        "explicit_pin_entry_count": 441,
        "unique_explicit_pin_id_count": 441,
        "implicit_pin_membership_count": 56,
        "net_entry_count": 270,
        "net_pin_membership_count": 497,
        "net_label_placement_count": 118,
        "styled_text_span_count": 2145,
    },
}
EVIDENCE_LIMIT = 6
EVIDENCE_RADIUS_POINTS = 24.0


class ExtractionError(RuntimeError):
    """Raised when the input does not match the expected smart-PDF structure."""


def _require_pymupdf_version() -> None:
    actual = str(getattr(fitz, "VersionBind", getattr(fitz, "__version__", "unknown")))
    if actual != EXPECTED_PYMUPDF_VERSION:
        raise ExtractionError(
            f"PyMuPDF {EXPECTED_PYMUPDF_VERSION} is required for byte-stable output; found {actual}. "
            "Install scripts/requirements-sc02-schematic.txt."
        )


def _round_point(value: float) -> float:
    return round(float(value), 3)


def _bbox_list(bbox: Iterable[float]) -> list[float]:
    return [_round_point(value) for value in bbox]


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _split_pin_id(pin_id: str) -> tuple[str, str]:
    try:
        owner, number = pin_id.rsplit("-", 1)
    except ValueError as exc:
        raise ExtractionError(f"Malformed pin id in PDF TOC: {pin_id!r}") from exc
    if not owner or not number:
        raise ExtractionError(f"Malformed pin id in PDF TOC: {pin_id!r}")
    return owner, number


def _component_tag(component_ref: str) -> str:
    return f"CO{component_ref}"


def _pin_tag(pin_id: str) -> str:
    # Altium replaces the component/pin separator with a literal zero.
    return f"PI{pin_id.replace('-', '0')}"


def _net_label_tag(net_name: str) -> str:
    # In this PDF, Altium keeps letters, digits, ampersands, and spaces.  It
    # replaces slash and underscore characters with zero.  The TOC remains
    # authoritative because this tag encoding is not reversible.
    encoded = "".join(
        char if char.isalnum() or char in {"&", " "} else "0"
        for char in net_name
    )
    return f"NL{encoded}"


def _parse_toc(document: Any) -> list[dict[str, Any]]:
    sheets_by_name: dict[str, dict[str, Any]] = {}
    current_sheet: dict[str, Any] | None = None
    current_section: str | None = None
    current_component: dict[str, Any] | None = None
    current_net: dict[str, Any] | None = None
    current_net_subsection: str | None = None

    for level, title, page_number in document.get_toc(simple=True):
        if level == 1:
            continue

        if level == 2:
            if page_number < 1:
                raise ExtractionError(
                    f"Sheet bookmark {title!r} has no destination page"
                )
            if title in sheets_by_name:
                raise ExtractionError(f"Duplicate sheet bookmark: {title!r}")
            current_sheet = {
                "name": title,
                "page_number": page_number,
                "components": [],
                "nets": [],
            }
            sheets_by_name[title] = current_sheet
            current_section = None
            current_component = None
            current_net = None
            current_net_subsection = None
            continue

        if current_sheet is None:
            raise ExtractionError(f"TOC entry appears before a sheet: {title!r}")

        if level == 3:
            if title not in {"Components", "Nets"}:
                raise ExtractionError(
                    f"Unexpected section {title!r} in sheet {current_sheet['name']!r}"
                )
            current_section = title
            current_component = None
            current_net = None
            current_net_subsection = None
            continue

        if level == 4 and current_section == "Components":
            current_component = {"ref": title, "pin_ids": []}
            current_sheet["components"].append(current_component)
            continue

        if level == 5 and current_section == "Components":
            if current_component is None:
                raise ExtractionError(
                    f"Component pin {title!r} has no component parent"
                )
            current_component["pin_ids"].append(title)
            continue

        if level == 4 and current_section == "Nets":
            current_net = {"name": title, "pin_ids": [], "net_labels": []}
            current_sheet["nets"].append(current_net)
            current_net_subsection = None
            continue

        if level == 5 and current_section == "Nets":
            if title not in {"Pins", "NetLabels"}:
                raise ExtractionError(
                    f"Unexpected net subsection {title!r} in "
                    f"sheet {current_sheet['name']!r}"
                )
            current_net_subsection = title
            continue

        if level == 6 and current_section == "Nets":
            if current_net is None or current_net_subsection is None:
                raise ExtractionError(f"Net member {title!r} has no net parent")
            if current_net_subsection == "Pins":
                current_net["pin_ids"].append(title)
            else:
                current_net["net_labels"].append(title)
            continue

        raise ExtractionError(
            f"Unexpected TOC entry at level {level}: {title!r}"
        )

    sheets = sorted(sheets_by_name.values(), key=lambda item: item["page_number"])
    page_numbers = [sheet["page_number"] for sheet in sheets]
    expected_page_numbers = list(range(1, len(sheets) + 1))
    if page_numbers != expected_page_numbers:
        raise ExtractionError(
            f"Sheet pages are not contiguous: {page_numbers!r}"
        )
    return sheets


def _page_words(page: Any) -> list[dict[str, Any]]:
    words: list[dict[str, Any]] = []
    for word in page.get_text("words", sort=False):
        x0, y0, x1, y1, text, block, line, word_index = word
        words.append(
            {
                "text": text,
                "bbox": _bbox_list((x0, y0, x1, y1)),
                "block": int(block),
                "line": int(line),
                "word": int(word_index),
            }
        )
    return words


def _styled_text_spans(page: Any) -> list[dict[str, Any]]:
    spans = []
    text_dictionary = page.get_text("dict")
    for block_ordinal, block in enumerate(text_dictionary.get("blocks", [])):
        for line_ordinal, line in enumerate(block.get("lines", [])):
            for span_ordinal, span in enumerate(line.get("spans", [])):
                text = span.get("text", "")
                if text == "":
                    continue
                bbox = _bbox_list(span["bbox"])
                origin = _bbox_list(span.get("origin", (bbox[0], bbox[3])))
                spans.append(
                    {
                        "block_ordinal": block_ordinal,
                        "line_ordinal": line_ordinal,
                        "span_ordinal": span_ordinal,
                        "text": text,
                        "bbox_points": bbox,
                        "origin_points": origin,
                        "font": str(span.get("font", "")),
                        "size_points": _round_point(span.get("size", 0.0)),
                        "color": int(span.get("color", 0)),
                        "flags": int(span.get("flags", 0)),
                        "char_flags": int(span.get("char_flags", 0)),
                        "alpha": int(span.get("alpha", 255)),
                        "ascender": _round_point(span.get("ascender", 0.0)),
                        "descender": _round_point(span.get("descender", 0.0)),
                    }
                )
    return spans


def _word_index(words: list[dict[str, Any]]) -> dict[str, list[dict[str, Any]]]:
    index: dict[str, list[dict[str, Any]]] = {}
    for word in words:
        index.setdefault(word["text"], []).append(word)
    return index


def _word_position_index(
    words: list[dict[str, Any]],
) -> dict[tuple[int, int, int], dict[str, Any]]:
    return {
        (word["block"], word["line"], word["word"]): word for word in words
    }


def _word_key(word: dict[str, Any]) -> tuple[int, int, int]:
    return (word["block"], word["line"], word["word"])


def _word_groups_for_text(
    text: str,
    words_by_text: dict[str, list[dict[str, Any]]],
    words_by_position: dict[tuple[int, int, int], dict[str, Any]],
) -> list[list[dict[str, Any]]]:
    parts = text.split(" ")
    if not parts or any(part == "" for part in parts):
        return []

    groups = []
    for first_word in words_by_text.get(parts[0], []):
        joined_words = [first_word]
        for offset, part in enumerate(parts[1:], start=1):
            next_word = words_by_position.get(
                (
                    first_word["block"],
                    first_word["line"],
                    first_word["word"] + offset,
                )
            )
            if next_word is None or next_word["text"] != part:
                break
            joined_words.append(next_word)
        if len(joined_words) == len(parts):
            groups.append(joined_words)
    return groups


def _group_bbox(words: list[dict[str, Any]]) -> list[float]:
    return [
        min(word["bbox"][0] for word in words),
        min(word["bbox"][1] for word in words),
        max(word["bbox"][2] for word in words),
        max(word["bbox"][3] for word in words),
    ]


def _rect_distance(first: list[float], second: list[float]) -> float:
    dx = max(second[0] - first[2], first[0] - second[2], 0.0)
    dy = max(second[1] - first[3], first[1] - second[3], 0.0)
    return math.hypot(dx, dy)


def _nearby_text(
    bbox: list[float],
    visible_words: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    candidates: list[tuple[float, dict[str, Any]]] = []
    for word in visible_words:
        distance = _rect_distance(bbox, word["bbox"])
        if distance <= EVIDENCE_RADIUS_POINTS:
            candidates.append((distance, word))

    candidates.sort(
        key=lambda item: (
            round(item[0], 6),
            item[1]["bbox"][1],
            item[1]["bbox"][0],
            item[1]["text"],
            item[1]["block"],
            item[1]["line"],
            item[1]["word"],
        )
    )
    evidence = []
    for distance, word in candidates[:EVIDENCE_LIMIT]:
        evidence.append(
            {
                "text": word["text"],
                "bbox_points": word["bbox"],
                "distance_points": _round_point(distance),
            }
        )
    return evidence


def _anchors_for_tag(
    tag: str,
    words_by_text: dict[str, list[dict[str, Any]]],
    words_by_position: dict[tuple[int, int, int], dict[str, Any]],
    visible_words: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    grouped: dict[tuple[float, float, float, float], int] = {}
    for words in _word_groups_for_text(tag, words_by_text, words_by_position):
        bbox_key = tuple(_group_bbox(words))
        grouped[bbox_key] = grouped.get(bbox_key, 0) + 1

    anchors = []
    for bbox_key in sorted(grouped, key=lambda item: (item[1], item[0], item[3], item[2])):
        bbox = list(bbox_key)
        anchors.append(
            {
                "tag": tag,
                "bbox_points": bbox,
                "center_points": [
                    _round_point((bbox[0] + bbox[2]) / 2.0),
                    _round_point((bbox[1] + bbox[3]) / 2.0),
                ],
                "pdf_word_occurrences": grouped[bbox_key],
                "nearby_visible_text": _nearby_text(bbox, visible_words),
            }
        )
    return anchors


def _metadata_word_keys(
    tags: set[str],
    words_by_text: dict[str, list[dict[str, Any]]],
    words_by_position: dict[tuple[int, int, int], dict[str, Any]],
) -> set[tuple[int, int, int]]:
    keys: set[tuple[int, int, int]] = set()
    for tag in tags:
        for words in _word_groups_for_text(tag, words_by_text, words_by_position):
            keys.update(_word_key(word) for word in words)
    return keys


def _visible_text_candidates(
    text: str,
    visible_words: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    words_by_text = _word_index(visible_words)
    words_by_position = _word_position_index(visible_words)
    candidates = []
    for words in _word_groups_for_text(text, words_by_text, words_by_position):
        bbox = _group_bbox(words)
        candidates.append(
            {
                "text": text,
                "bbox_points": bbox,
                "center_points": [
                    _round_point((bbox[0] + bbox[2]) / 2.0),
                    _round_point((bbox[1] + bbox[3]) / 2.0),
                ],
                "word_count": len(words),
                "words": [
                    {
                        "text": word["text"],
                        "bbox_points": word["bbox"],
                    }
                    for word in words
                ],
            }
        )

    candidates.sort(
        key=lambda candidate: (
            candidate["bbox_points"][1],
            candidate["bbox_points"][0],
            candidate["bbox_points"][3],
            candidate["bbox_points"][2],
        )
    )
    for ordinal, candidate in enumerate(candidates, start=1):
        candidate["candidate_ordinal"] = ordinal
    return candidates


def _label_occurrence_objects(
    label_names: list[str],
    candidates: list[dict[str, Any]],
    representative_tag_anchors: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    occurrences = [
        {
            "ordinal": ordinal,
            "name": name,
            "position_status": "unresolved",
            "visible_text_candidates": [],
        }
        for ordinal, name in enumerate(label_names, start=1)
    ]
    if len(occurrences) != 1 or not candidates:
        return occurrences

    chosen_candidate: dict[str, Any] | None = None
    position_status = "unresolved"
    if len(candidates) == 1:
        chosen_candidate = candidates[0]
        position_status = "unique_exact_visible_text_candidate"
    elif representative_tag_anchors:
        tag_bbox = representative_tag_anchors[0]["bbox_points"]
        ranked = sorted(
            (
                _rect_distance(tag_bbox, candidate["bbox_points"]),
                candidate["candidate_ordinal"],
                candidate,
            )
            for candidate in candidates
        )
        nearest_distance = ranked[0][0]
        next_distance = ranked[1][0] if len(ranked) > 1 else math.inf
        if nearest_distance <= 2.0 and next_distance - nearest_distance >= 0.5:
            chosen_candidate = ranked[0][2]
            position_status = "representative_tag_nearest_exact_text_candidate"

    if chosen_candidate is not None:
        occurrences[0]["position_status"] = position_status
        occurrences[0]["visible_text_candidates"] = [chosen_candidate]
    return occurrences


def _annotate_sheet(document: Any, sheet: dict[str, Any]) -> None:
    page = document[sheet["page_number"] - 1]
    words = _page_words(page)
    words_by_text = _word_index(words)
    words_by_position = _word_position_index(words)

    component_tags = {
        _component_tag(component["ref"]) for component in sheet["components"]
    }
    connected_pin_ids = {
        pin_id for net in sheet["nets"] for pin_id in net["pin_ids"]
    }
    pin_tags = {_pin_tag(pin_id) for pin_id in connected_pin_ids}
    named_nets = [net for net in sheet["nets"] if net["net_labels"]]
    net_tags = {_net_label_tag(net["name"]) for net in named_nets}
    metadata_tags = component_tags | pin_tags | net_tags
    metadata_keys = _metadata_word_keys(
        metadata_tags, words_by_text, words_by_position
    )
    visible_words = [word for word in words if _word_key(word) not in metadata_keys]

    sheet["page_size_points"] = {
        "width": _round_point(page.rect.width),
        "height": _round_point(page.rect.height),
    }
    sheet["styled_text_spans"] = _styled_text_spans(page)

    explicit_pin_ids: set[str] = set()
    annotated_components = []
    component_refs = [component["ref"] for component in sheet["components"]]
    if len(component_refs) != len(set(component_refs)):
        duplicates = sorted(
            ref for ref in set(component_refs) if component_refs.count(ref) > 1
        )
        raise ExtractionError(
            f"Duplicate component-unit refs in {sheet['name']!r}: {duplicates!r}"
        )
    for component in sheet["components"]:
        pin_owners = {_split_pin_id(pin_id)[0] for pin_id in component["pin_ids"]}
        if len(pin_owners) != 1:
            raise ExtractionError(
                f"Component {component['ref']!r} does not have one physical pin owner: "
                f"{sorted(pin_owners)!r}"
            )
        physical_ref = next(iter(pin_owners))
        if not component["ref"].startswith(physical_ref):
            raise ExtractionError(
                f"Component-unit ref {component['ref']!r} does not begin with "
                f"physical ref {physical_ref!r}"
            )
        unit_suffix = component["ref"][len(physical_ref) :]
        if unit_suffix and re.fullmatch(r"[A-Z]+", unit_suffix) is None:
            raise ExtractionError(
                f"Malformed unit suffix {unit_suffix!r} on {component['ref']!r}"
            )

        pins = []
        for pin_id in component["pin_ids"]:
            if pin_id in explicit_pin_ids:
                raise ExtractionError(
                    f"Duplicate explicit pin id {pin_id!r} in {sheet['name']!r}"
                )
            explicit_pin_ids.add(pin_id)
            pin_owner, number = _split_pin_id(pin_id)
            if pin_owner != physical_ref:
                raise ExtractionError(
                    f"Pin {pin_id!r} owner differs from component physical ref "
                    f"{physical_ref!r}"
                )
            tag = _pin_tag(pin_id)
            pins.append(
                {
                    "id": pin_id,
                    "number": number,
                    "anchor_tag": tag,
                    "anchors": _anchors_for_tag(
                        tag, words_by_text, words_by_position, visible_words
                    ),
                }
            )

        component_tag = _component_tag(component["ref"])
        annotated_components.append(
            {
                "ref": component["ref"],
                "physical_ref": physical_ref,
                "unit_suffix": unit_suffix or None,
                "anchor_tag": component_tag,
                "anchors": _anchors_for_tag(
                    component_tag,
                    words_by_text,
                    words_by_position,
                    visible_words,
                ),
                "pins": pins,
            }
        )
    sheet["components"] = annotated_components

    implicit_pin_ids = sorted(connected_pin_ids - explicit_pin_ids)
    sheet["implicit_pins"] = []
    for pin_id in implicit_pin_ids:
        owner, number = _split_pin_id(pin_id)
        tag = _pin_tag(pin_id)
        sheet["implicit_pins"].append(
            {
                "id": pin_id,
                "physical_ref": owner,
                "number": number,
                "anchor_tag": tag,
                "anchors": _anchors_for_tag(
                    tag, words_by_text, words_by_position, visible_words
                ),
            }
        )

    annotated_nets = []
    net_names = [net["name"] for net in sheet["nets"]]
    if len(net_names) != len(set(net_names)):
        duplicates = sorted(name for name in set(net_names) if net_names.count(name) > 1)
        raise ExtractionError(
            f"Duplicate sheet-local net names in {sheet['name']!r}: {duplicates!r}"
        )
    for net in sheet["nets"]:
        if len(net["pin_ids"]) != len(set(net["pin_ids"])):
            raise ExtractionError(
                f"Net {net['name']!r} in {sheet['name']!r} contains a pin twice"
            )
        if any(label != net["name"] for label in net["net_labels"]):
            raise ExtractionError(
                f"Net-label title differs from net title for {net['name']!r}"
            )
        label_tag = _net_label_tag(net["name"]) if net["net_labels"] else None
        representative_tag_anchors = (
            _anchors_for_tag(
                label_tag,
                words_by_text,
                words_by_position,
                visible_words,
            )
            if label_tag is not None
            else []
        )
        visible_text_candidates = (
            _visible_text_candidates(net["name"], visible_words)
            if net["net_labels"]
            else []
        )
        annotated_nets.append(
            {
                "name": net["name"],
                "pin_ids": net["pin_ids"],
                "net_label_names": net["net_labels"],
                "net_labels": _label_occurrence_objects(
                    net["net_labels"],
                    visible_text_candidates,
                    representative_tag_anchors,
                ),
                "visible_text_candidates": visible_text_candidates,
                "representative_tag": label_tag,
                "representative_tag_anchors": representative_tag_anchors,
            }
        )
    sheet["nets"] = annotated_nets


def _check(
    checks: list[dict[str, Any]],
    name: str,
    actual: Any,
    expected: Any,
) -> None:
    passed = actual == expected
    checks.append(
        {
            "name": name,
            "expected": expected,
            "actual": actual,
            "passed": passed,
        }
    )
    if not passed:
        raise ExtractionError(
            f"Validation failed for {name}: expected {expected!r}, got {actual!r}"
        )


def _membership_tuples(
    sheets: list[dict[str, Any]],
) -> list[tuple[str, str, str, str]]:
    tuples = []
    for sheet in sheets:
        component_unit_by_pin = {
            pin["id"]: component["ref"]
            for component in sheet["components"]
            for pin in component["pins"]
        }
        implicit_owner_by_pin = {
            pin["id"]: pin["physical_ref"] for pin in sheet["implicit_pins"]
        }
        for net in sheet["nets"]:
            for pin_id in net["pin_ids"]:
                source_ref = component_unit_by_pin.get(pin_id)
                if source_ref is None:
                    source_ref = implicit_owner_by_pin.get(pin_id)
                if source_ref is None:
                    raise ExtractionError(
                        f"Net {net['name']!r} in {sheet['name']!r} refers to "
                        f"unknown pin {pin_id!r}"
                    )
                tuples.append(
                    (sheet["name"], source_ref, pin_id, net["name"])
                )
    return sorted(tuples)


def _membership_tuple_sha256(
    tuples: list[tuple[str, str, str, str]],
) -> str:
    payload = json.dumps(
        tuples,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def _build_implicit_pin_index(
    sheets: list[dict[str, Any]],
    implicit_pin_ids: set[str],
    pin_to_net_names: dict[str, set[str]],
) -> list[dict[str, Any]]:
    component_units_by_owner: dict[str, list[dict[str, Any]]] = {}
    for sheet in sheets:
        for component in sheet["components"]:
            component_units_by_owner.setdefault(
                component["physical_ref"], []
            ).append(
                {
                    "sheet": sheet["name"],
                    "page_number": sheet["page_number"],
                    "ref": component["ref"],
                    "unit_suffix": component["unit_suffix"],
                }
            )

    implicit_placements: dict[str, list[dict[str, Any]]] = {}
    implicit_owner_by_pin: dict[str, str] = {}
    for sheet in sheets:
        for pin in sheet["implicit_pins"]:
            pin_id = pin["id"]
            if pin_id not in implicit_pin_ids:
                raise ExtractionError(
                    f"Sheet implicit pin {pin_id!r} is explicit elsewhere"
                )
            owner, number = _split_pin_id(pin_id)
            if pin["physical_ref"] != owner or pin["number"] != number:
                raise ExtractionError(
                    f"Malformed implicit pin owner/number for {pin_id!r}"
                )
            prior_owner = implicit_owner_by_pin.setdefault(pin_id, owner)
            if prior_owner != owner:
                raise ExtractionError(
                    f"Implicit pin {pin_id!r} has two physical owners"
                )
            implicit_placements.setdefault(pin_id, []).append(
                {
                    "sheet": sheet["name"],
                    "page_number": sheet["page_number"],
                }
            )

    memberships_by_pin: dict[str, list[dict[str, Any]]] = {}
    for sheet in sheets:
        sheet_implicit_ids = {pin["id"] for pin in sheet["implicit_pins"]}
        for net in sheet["nets"]:
            for pin_id in net["pin_ids"]:
                if pin_id not in implicit_pin_ids:
                    continue
                if pin_id not in sheet_implicit_ids:
                    raise ExtractionError(
                        f"Implicit pin {pin_id!r} lacks its sheet occurrence in "
                        f"{sheet['name']!r}"
                    )
                memberships_by_pin.setdefault(pin_id, []).append(
                    {
                        "sheet": sheet["name"],
                        "page_number": sheet["page_number"],
                        "net_name": net["name"],
                    }
                )

    implicit_pin_index = []
    for pin_id in sorted(implicit_pin_ids):
        owner, number = _split_pin_id(pin_id)
        component_units = sorted(
            component_units_by_owner.get(owner, []),
            key=lambda item: (
                0 if item["ref"] == owner else 1,
                item["page_number"],
                item["ref"],
            ),
        )
        if not component_units:
            raise ExtractionError(
                f"Implicit pin {pin_id!r} owner {owner!r} has no component unit"
            )
        if pin_id not in implicit_placements or pin_id not in memberships_by_pin:
            raise ExtractionError(
                f"Implicit pin {pin_id!r} has no sheet/net membership"
            )
        net_names = pin_to_net_names.get(pin_id, set())
        if len(net_names) != 1:
            raise ExtractionError(
                f"Implicit pin {pin_id!r} does not have one canonical net name"
            )
        canonical = component_units[0]
        memberships = sorted(
            memberships_by_pin[pin_id],
            key=lambda item: (
                item["page_number"],
                item["sheet"],
                item["net_name"],
            ),
        )
        implicit_pin_index.append(
            {
                "id": pin_id,
                "owner": owner,
                "number": number,
                "net_name": sorted(net_names)[0],
                "canonical_sheet": canonical["sheet"],
                "canonical_page_number": canonical["page_number"],
                "canonical_component_unit": canonical["ref"],
                "component_units": component_units,
                "memberships": memberships,
            }
        )
    return implicit_pin_index


def _sheet_counts(sheet: dict[str, Any]) -> dict[str, int]:
    explicit_pin_ids = [
        pin["id"]
        for component in sheet["components"]
        for pin in component["pins"]
    ]
    return {
        "page_number": sheet["page_number"],
        "component_unit_count": len(sheet["components"]),
        "explicit_pin_entry_count": len(explicit_pin_ids),
        "unique_explicit_pin_id_count": len(set(explicit_pin_ids)),
        "implicit_pin_membership_count": len(sheet["implicit_pins"]),
        "net_entry_count": len(sheet["nets"]),
        "net_pin_membership_count": sum(
            len(net["pin_ids"]) for net in sheet["nets"]
        ),
        "net_label_placement_count": sum(
            len(net["net_labels"]) for net in sheet["nets"]
        ),
        "styled_text_span_count": len(sheet["styled_text_spans"]),
    }


def _validate_and_summarize(
    document: Any,
    sheets: list[dict[str, Any]],
) -> tuple[
    dict[str, Any],
    dict[str, Any],
    list[dict[str, Any]],
    list[dict[str, Any]],
]:
    component_refs = [
        component["ref"] for sheet in sheets for component in sheet["components"]
    ]
    if len(component_refs) != len(set(component_refs)):
        duplicates = sorted(
            ref for ref in set(component_refs) if component_refs.count(ref) > 1
        )
        raise ExtractionError(
            f"Duplicate component-unit refs across sheets: {duplicates!r}"
        )

    explicit_pin_entries = [
        pin["id"]
        for sheet in sheets
        for component in sheet["components"]
        for pin in component["pins"]
    ]
    explicit_pin_ids = set(explicit_pin_entries)
    if len(explicit_pin_entries) != len(explicit_pin_ids):
        duplicates = sorted(
            pin_id
            for pin_id in explicit_pin_ids
            if explicit_pin_entries.count(pin_id) > 1
        )
        raise ExtractionError(
            f"Duplicate explicit pin ids across component units: {duplicates!r}"
        )

    connected_pin_ids = {
        pin_id
        for sheet in sheets
        for net in sheet["nets"]
        for pin_id in net["pin_ids"]
    }
    implicit_pin_ids = connected_pin_ids - explicit_pin_ids
    unconnected_explicit_pin_ids = sorted(explicit_pin_ids - connected_pin_ids)
    physical_refs = {
        component["physical_ref"]
        for sheet in sheets
        for component in sheet["components"]
    }
    labeled_sheet_nets = [
        net for sheet in sheets for net in sheet["nets"] if net["net_labels"]
    ]
    distinct_net_names = {
        net["name"] for sheet in sheets for net in sheet["nets"]
    }
    distinct_label_names = {net["name"] for net in labeled_sheet_nets}

    pin_to_net_names: dict[str, set[str]] = {}
    pin_memberships: dict[str, list[dict[str, Any]]] = {}
    for sheet in sheets:
        for net in sheet["nets"]:
            for pin_id in net["pin_ids"]:
                pin_to_net_names.setdefault(pin_id, set()).add(net["name"])
                pin_memberships.setdefault(pin_id, []).append(
                    {
                        "sheet": sheet["name"],
                        "page_number": sheet["page_number"],
                        "net_name": net["name"],
                    }
                )
    conflicting_pin_nets = {
        pin_id: sorted(net_names)
        for pin_id, net_names in pin_to_net_names.items()
        if len(net_names) > 1
    }

    label_occurrence_errors = []
    for sheet in sheets:
        for net in sheet["nets"]:
            for expected_ordinal, occurrence in enumerate(
                net["net_labels"], start=1
            ):
                if occurrence.get("ordinal") != expected_ordinal:
                    label_occurrence_errors.append(
                        {
                            "sheet": sheet["name"],
                            "net_name": net["name"],
                            "reason": "ordinal",
                        }
                    )
                if occurrence.get("name") != net["name"]:
                    label_occurrence_errors.append(
                        {
                            "sheet": sheet["name"],
                            "net_name": net["name"],
                            "reason": "name",
                        }
                    )

    missing_component_anchors = [
        {
            "sheet": sheet["name"],
            "ref": component["ref"],
            "tag": component["anchor_tag"],
        }
        for sheet in sheets
        for component in sheet["components"]
        if not component["anchors"]
    ]
    missing_pin_anchors = []
    missing_representative_tag_anchors = []
    for sheet in sheets:
        for component in sheet["components"]:
            for pin in component["pins"]:
                if not pin["anchors"]:
                    missing_pin_anchors.append(
                        {
                            "sheet": sheet["name"],
                            "pin_id": pin["id"],
                            "tag": pin["anchor_tag"],
                        }
                    )
        for pin in sheet["implicit_pins"]:
            if not pin["anchors"]:
                missing_pin_anchors.append(
                    {
                        "sheet": sheet["name"],
                        "pin_id": pin["id"],
                        "tag": pin["anchor_tag"],
                    }
                )
        for net in sheet["nets"]:
            if net["net_labels"] and not net["representative_tag_anchors"]:
                missing_representative_tag_anchors.append(
                    {
                        "sheet": sheet["name"],
                        "net_name": net["name"],
                        "tag": net["representative_tag"],
                    }
                )

    membership_tuples = _membership_tuples(sheets)
    membership_digest = _membership_tuple_sha256(membership_tuples)
    implicit_pin_index = _build_implicit_pin_index(
        sheets, implicit_pin_ids, pin_to_net_names
    )
    summary = {
        "sheet_count": len(sheets),
        "component_unit_count": len(component_refs),
        "physical_component_ref_count": len(physical_refs),
        "explicit_pin_entry_count": len(explicit_pin_entries),
        "unique_explicit_pin_id_count": len(explicit_pin_ids),
        "net_pin_membership_count": len(membership_tuples),
        "unique_connected_pin_id_count": len(connected_pin_ids),
        "unique_implicit_pin_id_count": len(implicit_pin_ids),
        "implicit_pin_index_count": len(implicit_pin_index),
        "sheet_net_entry_count": sum(len(sheet["nets"]) for sheet in sheets),
        "distinct_net_name_count": len(distinct_net_names),
        "labeled_sheet_net_count": len(labeled_sheet_nets),
        "distinct_net_label_name_count": len(distinct_label_names),
        "net_label_placement_count": sum(
            len(net["net_labels"]) for net in labeled_sheet_nets
        ),
        "styled_text_span_count": sum(
            len(sheet["styled_text_spans"]) for sheet in sheets
        ),
        "toc_entry_count": len(document.get_toc(simple=True)),
        "membership_tuple_sha256": membership_digest,
    }

    checks: list[dict[str, Any]] = []
    _check(checks, "pdf_page_count", document.page_count, 7)
    _check(
        checks,
        "summary_metric_names",
        sorted(summary),
        sorted(EXPECTED_SUMMARY),
    )
    for name, expected in EXPECTED_SUMMARY.items():
        _check(checks, f"summary:{name}", summary[name], expected)
    actual_sheet_names = [sheet["name"] for sheet in sheets]
    _check(
        checks,
        "sheet_names",
        actual_sheet_names,
        list(EXPECTED_SHEET_COUNTS),
    )
    for sheet in sheets:
        _check(
            checks,
            f"sheet_counts:{sheet['name']}",
            _sheet_counts(sheet),
            EXPECTED_SHEET_COUNTS[sheet["name"]],
        )
    _check(
        checks,
        "explicit_entries_equal_unique_explicit_ids",
        len(explicit_pin_entries),
        len(explicit_pin_ids),
    )
    _check(checks, "unconnected_explicit_pin_ids", unconnected_explicit_pin_ids, [])
    _check(checks, "pin_ids_with_multiple_net_names", conflicting_pin_nets, {})
    _check(checks, "label_occurrence_errors", label_occurrence_errors, [])
    _check(checks, "missing_component_anchor_tags", missing_component_anchors, [])
    _check(checks, "missing_pin_anchor_tags", missing_pin_anchors, [])
    _check(
        checks,
        "missing_representative_net_label_tag_anchors",
        missing_representative_tag_anchors,
        [],
    )

    connected_pin_index = []
    for pin_id in sorted(connected_pin_ids):
        owner, number = _split_pin_id(pin_id)
        memberships = sorted(
            pin_memberships[pin_id],
            key=lambda item: (
                item["page_number"],
                item["sheet"],
                item["net_name"],
            ),
        )
        connected_pin_index.append(
            {
                "id": pin_id,
                "physical_ref": owner,
                "number": number,
                "implicit": pin_id in implicit_pin_ids,
                "net_name": sorted(pin_to_net_names[pin_id])[0],
                "memberships": memberships,
            }
        )

    validation = {"status": "passed", "checks": checks}
    return summary, validation, connected_pin_index, implicit_pin_index


def extract_schematic(input_pdf: Path) -> dict[str, Any]:
    _require_pymupdf_version()
    input_pdf = input_pdf.resolve()
    if not input_pdf.is_file():
        raise ExtractionError(f"Input PDF does not exist: {input_pdf}")

    try:
        document = fitz.open(input_pdf)
    except Exception as exc:
        raise ExtractionError(f"Could not open input PDF: {exc}") from exc

    try:
        if not document.is_pdf:
            raise ExtractionError(f"Input is not a PDF: {input_pdf}")
        sheets = _parse_toc(document)
        for sheet in sheets:
            _annotate_sheet(document, sheet)
        (
            summary,
            validation,
            connected_pin_index,
            implicit_pin_index,
        ) = _validate_and_summarize(document, sheets)
        metadata = {
            key: value or "" for key, value in sorted(document.metadata.items())
        }
        source = {
            # The generated archive sits beside its source PDF.  Keep this
            # path portable and do not leak the path of the machine that ran
            # the extractor.
            "path": input_pdf.name,
            "filename": input_pdf.name,
            "byte_length": input_pdf.stat().st_size,
            "sha256": _sha256(input_pdf),
            "pdf_metadata": metadata,
        }
    finally:
        document.close()

    return {
        "schema": SCHEMA_NAME,
        "extractor": {
            "name": "sc02_schematic_extract.py",
            "version": EXTRACTOR_VERSION,
            "pymupdf_version": getattr(fitz, "VersionBind", "unknown"),
        },
        "source": source,
        "summary": summary,
        "membership_digest": {
            "algorithm": "sha256",
            "tuple_fields": [
                "sheet",
                "component_unit_or_implicit_owner",
                "pin_id",
                "net_name",
            ],
            "sort": "lexicographic_by_all_tuple_fields",
            "serialization": "utf8_compact_json_array_ensure_ascii_false",
            "sha256": summary["membership_tuple_sha256"],
        },
        "validation": validation,
        "sheets": sheets,
        "connected_pin_index": connected_pin_index,
        "implicit_pin_index": implicit_pin_index,
    }


def _write_json(output_path: Path, model: dict[str, Any]) -> None:
    output_path = output_path.resolve()
    output_path.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = output_path.with_name(f".{output_path.name}.tmp")
    payload = json.dumps(model, ensure_ascii=False, indent=2) + "\n"
    try:
        temporary_path.write_text(payload, encoding="utf-8", newline="\n")
        os.replace(temporary_path, output_path)
    finally:
        if temporary_path.exists():
            temporary_path.unlink()


def _build_argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description=(
            "Extract the SC-02 Altium smart-PDF TOC and coordinate anchors "
            "into deterministic JSON."
        )
    )
    parser.add_argument("input_pdf", type=Path, help="SC-02 schematic PDF")
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="output JSON path",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_argument_parser().parse_args(argv)
    try:
        model = extract_schematic(args.input_pdf)
        if args.input_pdf.resolve() == args.output.resolve():
            raise ExtractionError("Output path must differ from input PDF path")
        _write_json(args.output, model)
    except (ExtractionError, OSError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    summary = model["summary"]
    print(
        f"wrote {args.output}: {summary['sheet_count']} sheets, "
        f"{summary['component_unit_count']} components, "
        f"{summary['unique_connected_pin_id_count']} connected pins, "
        f"{summary['sheet_net_entry_count']} sheet nets"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
