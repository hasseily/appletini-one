#!/usr/bin/env python3
"""Check the committed SC-02 prototype schematic archive.

The normal run uses only the Python standard library.  ``--regenerate`` also
needs the pinned PyMuPDF and Viz.js packages and rebuilds every generated
artifact in a temporary directory before comparing it byte for byte with the
committed copy.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile
from typing import Any, Iterable
import xml.etree.ElementTree as ET


REPO_ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = REPO_ROOT / "schematics" / "sc02_prototype"
SOURCE_PDF = ARCHIVE / "sc-02_Final_Schematic_V1.00.pdf"
MODEL_PATH = ARCHIVE / "sc02_schematic.json"
CONNECTIVITY_DIR = ARCHIVE / "connectivity"
CONNECTIVITY_SVG_DIR = ARCHIVE / "connectivity_svg"
SOURCE_SVG_DIR = ARCHIVE / "source_svg"
KICAD_DIR = ARCHIVE / "kicad"
MANIFEST_PATH = ARCHIVE / "MANIFEST.sha256"
KICAD_VALIDATION_PATH = ARCHIVE / "kicad_validation.json"

SCHEMA_NAME = "appletini.sc02_schematic.v1"
SOURCE_SHA256 = "d0e05ea01fc5e823571e140fce5ce9f12a48f1993484d683f075151382adba35"
SOURCE_BYTES = 5_080_426
MEMBERSHIP_SHA256 = "b13d422a2b0338a3080a5e16b092374ad5afddbb8699cf532b3d418d7cdd208f"
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
    "membership_tuple_sha256": MEMBERSHIP_SHA256,
}
SHEET_NAMES = [
    "analog_1.SchDoc",
    "analog_2.SchDoc",
    "digital_1.SchDoc",
    "digital_2.SchDoc",
    "digital_3.SchDoc",
    "digital_4.SchDoc",
    "digital_5.SchDoc",
]


class TestFailure(RuntimeError):
    """Raised when the committed archive violates an invariant."""


def require(condition: bool, message: str) -> None:
    if not condition:
        raise TestFailure(message)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def tree_digest(root: Path) -> str:
    entries = [
        [path.relative_to(root).as_posix(), sha256(path)]
        for path in sorted(root.rglob("*"))
        if path.is_file()
    ]
    payload = json.dumps(entries, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def load_model() -> dict[str, Any]:
    try:
        return json.loads(MODEL_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TestFailure(f"cannot read canonical JSON: {exc}") from exc


def membership_tuples(model: dict[str, Any]) -> list[list[str]]:
    tuples: list[list[str]] = []
    for sheet in model["sheets"]:
        explicit_owners = {
            pin["id"]: component["ref"]
            for component in sheet["components"]
            for pin in component["pins"]
        }
        implicit_owners = {
            pin["id"]: pin["physical_ref"] for pin in sheet["implicit_pins"]
        }
        for net in sheet["nets"]:
            for pin_id in net["pin_ids"]:
                owner = explicit_owners.get(pin_id, implicit_owners.get(pin_id))
                require(owner is not None, f"{sheet['name']}: unknown pin {pin_id}")
                tuples.append([sheet["name"], owner, pin_id, net["name"]])
    return sorted(tuples)


def membership_digest(tuples: list[list[str]]) -> str:
    payload = json.dumps(
        tuples,
        ensure_ascii=False,
        separators=(",", ":"),
    ).encode("utf-8")
    return hashlib.sha256(payload).hexdigest()


def test_source_and_model(model: dict[str, Any]) -> None:
    require(SOURCE_PDF.stat().st_size == SOURCE_BYTES, "source PDF byte length changed")
    require(sha256(SOURCE_PDF) == SOURCE_SHA256, "source PDF SHA-256 changed")
    require(model.get("schema") == SCHEMA_NAME, "unexpected canonical JSON schema")
    require(model.get("validation", {}).get("status") == "passed", "extractor checks failed")
    require(all(check.get("passed") for check in model["validation"]["checks"]), "a JSON validation check failed")
    require(len(model["validation"]["checks"]) == 34, "unexpected validation check count")
    require(model.get("summary") == EXPECTED_SUMMARY, "canonical JSON summary changed")
    require(
        model.get("extractor", {}).get("pymupdf_version") == "1.27.2.3",
        "canonical JSON PyMuPDF version changed",
    )
    require(model["source"]["path"] == SOURCE_PDF.name, "source path is not portable")
    require(model["source"]["sha256"] == SOURCE_SHA256, "JSON source hash changed")
    require([sheet["name"] for sheet in model["sheets"]] == SHEET_NAMES, "sheet order changed")

    tuples = membership_tuples(model)
    require(len(tuples) == 3519, "membership tuple count changed")
    require(membership_digest(tuples) == MEMBERSHIP_SHA256, "membership tuple digest changed")
    require(model["membership_digest"]["sha256"] == MEMBERSHIP_SHA256, "stored membership digest changed")
    require(len(model["implicit_pin_index"]) == 208, "implicit pin index changed")
    require(
        sum(len(sheet["styled_text_spans"]) for sheet in model["sheets"]) == 15330,
        "styled text span count changed",
    )
    label_occurrences = [
        label
        for sheet in model["sheets"]
        for net in sheet["nets"]
        for label in net["net_labels"]
    ]
    require(len(label_occurrences) == 718, "net-label occurrence count changed")
    require(
        all(
            label["position_status"]
            in {
                "unique_exact_visible_text_candidate",
                "representative_tag_nearest_exact_text_candidate",
                "unresolved",
            }
            for label in label_occurrences
        ),
        "invalid net-label position status",
    )
    payload = MODEL_PATH.read_text(encoding="utf-8")
    checkout_paths = {str(REPO_ROOT), str(REPO_ROOT).replace("\\", "/")}
    require(
        not any(path in payload for path in checkout_paths),
        "JSON contains the extractor checkout path",
    )


def test_csv(model: dict[str, Any]) -> None:
    path = CONNECTIVITY_DIR / "sc02_pin_net_memberships.csv"
    with path.open("r", encoding="utf-8", newline="") as stream:
        rows = list(csv.DictReader(stream))
    require(len(rows) == 3519, "CSV row count changed")
    csv_tuples = sorted(
        [
            row["sheet_name"],
            row["component_ref"] or row["physical_ref"],
            row["pin_id"],
            row["net_name"],
        ]
        for row in rows
    )
    require(csv_tuples == membership_tuples(model), "CSV membership tuples differ from JSON")
    require(sum(row["implicit"] == "true" for row in rows) == 306, "CSV implicit membership count changed")


def xml_root(path: Path) -> ET.Element:
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as exc:
        raise TestFailure(f"invalid XML in {path.relative_to(REPO_ROOT)}: {exc}") from exc
    require(root.tag.rsplit("}", 1)[-1] == "svg", f"not an SVG file: {path}")
    for element in root.iter():
        require(element.tag.rsplit("}", 1)[-1] != "script", f"script element in {path}")
        for name, value in element.attrib.items():
            if name.rsplit("}", 1)[-1] == "href":
                require(value.startswith(("#", "data:")), f"external SVG link in {path}: {value}")
    return root


def edge_count(root: ET.Element) -> int:
    return sum(element.attrib.get("class") == "edge" for element in root.iter())


def test_svg_and_dot() -> None:
    source_svgs = sorted(SOURCE_SVG_DIR.glob("*.svg"))
    require(len(source_svgs) == 7, "source SVG sheet count changed")
    for path in source_svgs:
        xml_root(path)
        require(SOURCE_SHA256 in path.read_text(encoding="utf-8"), f"missing SVG provenance: {path.name}")

    dots = sorted(CONNECTIVITY_DIR.glob("*.dot"))
    require(len(dots) == 8, "DOT graph count changed")
    sheet_dots = [path for path in dots if path.name != "index.dot"]
    sheet_edges = sum(
        sum(" -- " in line and "pin_id=" in line for line in path.read_text(encoding="utf-8").splitlines())
        for path in sheet_dots
    )
    index_edges = sum(
        " -- " in line and "pin_id=" in line
        for line in (CONNECTIVITY_DIR / "index.dot").read_text(encoding="utf-8").splitlines()
    )
    require(sheet_edges == 3519 and index_edges == 3519, "DOT edge count changed")

    graph_svgs = sorted(CONNECTIVITY_SVG_DIR.glob("*.svg"))
    require(len(graph_svgs) == 8, "Graphviz SVG count changed")
    sheet_graphs = [path for path in graph_svgs if path.name != "index.svg"]
    require(sum(edge_count(xml_root(path)) for path in sheet_graphs) == 3519, "sheet SVG edge count changed")
    require(edge_count(xml_root(CONNECTIVITY_SVG_DIR / "index.svg")) == 3519, "index SVG edge count changed")


def validate_sexpr(text: str, path: Path) -> None:
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
            require(depth >= 0, f"unmatched ')' in {path} at byte {offset}")
    require(not in_string and depth == 0, f"unbalanced S-expression in {path}")


def test_kicad() -> None:
    expected_names = {
        "sc02_generic.kicad_sym",
        "sc02_prototype.kicad_pro",
        "sc02_prototype.kicad_sch",
        "sym-lib-table",
        *{f"sc02_sheet_{index:02d}_{name.casefold().replace('.', '_')}.kicad_sch" for index, name in enumerate(SHEET_NAMES, 1)},
    }
    paths = {path.name: path for path in KICAD_DIR.iterdir() if path.is_file()}
    require(set(paths) == expected_names, "KiCad file set changed")
    json.loads(paths["sc02_prototype.kicad_pro"].read_text(encoding="utf-8"))
    for path in paths.values():
        if path.suffix in {".kicad_sch", ".kicad_sym"} or path.name == "sym-lib-table":
            validate_sexpr(path.read_text(encoding="utf-8"), path)

    root = paths["sc02_prototype.kicad_sch"].read_text(encoding="utf-8")
    require(len(re.findall(r"(?m)^  \(sheet$", root)) == 7, "KiCad root child count changed")
    leaves = [paths[name] for name in sorted(paths) if name.startswith("sc02_sheet_")]
    leaf_text = [path.read_text(encoding="utf-8") for path in leaves]
    require(all('(paper "User" ' in text for text in leaf_text), "KiCad custom paper syntax changed")
    require(sum(len(re.findall(r"(?m)^  \(symbol$", text)) for text in leaf_text) == 785, "KiCad symbol count changed")
    require(sum(text.count("  (global_label ") for text in leaf_text) == 3519, "KiCad label count changed")
    require(sum(text.count("  (wire") for text in leaf_text) == 0, "KiCad gained routed wires")
    require(sum(text.count('(property "SC02.PinMap"') for text in leaf_text) == 785, "KiCad pin-map count changed")
    library = paths["sc02_generic.kicad_sym"].read_text(encoding="utf-8")
    require(len(re.findall(r"(?m)^  \(symbol ", library)) == 785, "KiCad library symbol count changed")


def test_kicad_validation() -> None:
    try:
        record = json.loads(KICAD_VALIDATION_PATH.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise TestFailure(f"cannot read KiCad validation record: {exc}") from exc
    require(record.get("schema") == "appletini.sc02_kicad_validation.v1", "KiCad validation schema changed")
    validator = record.get("validator", {})
    require(validator.get("name") == "kicad-cli", "unexpected KiCad validator")
    require(validator.get("version") == "10.0.5", "unexpected KiCad validator version")
    require(validator.get("container_image") == "kicad/kicad:10.0.5", "unexpected KiCad image")
    require(
        validator.get("container_digest")
        == "sha256:182c8005cb775a2c448a4c18681d489f1ff472a761885eba3e08b07e3c0564de",
        "unexpected KiCad image digest",
    )
    source = record.get("input", {})
    require(source.get("project") == "kicad/sc02_prototype.kicad_sch", "KiCad project path changed")
    require(source.get("symbol_library") == "kicad/sc02_generic.kicad_sym", "KiCad library path changed")
    require(source.get("source_model_sha256") == sha256(MODEL_PATH), "KiCad validation is stale for JSON")
    require(source.get("source_membership_sha256") == MEMBERSHIP_SHA256, "KiCad membership hash changed")
    require(source.get("kicad_file_count") == 11, "KiCad validation file count changed")
    require(source.get("kicad_tree_sha256") == tree_digest(KICAD_DIR), "KiCad validation is stale for files")
    require(source.get("validated_on_disposable_copy") is True, "KiCad disposable-copy check missing")

    checks = record.get("checks", {})
    require(checks.get("symbol_library_load") == "passed", "KiCad library did not pass")
    require(checks.get("hierarchy_load_and_upgrade") == "passed", "KiCad hierarchy did not pass")
    require(
        checks.get("hierarchical_svg_export") == {
            "status": "passed",
            "page_count": 8,
            "child_page_count": 7,
        },
        "KiCad SVG export counts changed",
    )
    require(
        checks.get("xml_netlist") == {
            "status": "passed",
            "component_count": 785,
            "net_count": 1088,
            "node_count": 3519,
            "unconnected_net_count": 0,
            "single_node_net_count": 414,
            "multi_node_net_count": 674,
        },
        "KiCad XML net-list counts changed",
    )
    erc = checks.get("erc", {})
    require(erc.get("status") == "expected_warnings_only", "KiCad ERC status changed")
    require(erc.get("total") == 1199 and erc.get("other_violation_count") == 0, "KiCad ERC totals changed")
    categories = {
        category.get("type"): (category.get("severity"), category.get("count"))
        for category in erc.get("categories", [])
    }
    require(
        categories == {
            "endpoint_off_grid": ("warning", 785),
            "isolated_pin_label": ("warning", 414),
        },
        "KiCad ERC categories changed",
    )


def test_kicad_generator_guards(model: dict[str, Any]) -> None:
    from sc02_kicad_generate import InputError, validate_archive_input

    validate_archive_input(model)
    tampered = dict(model)
    tampered["sheets"] = list(model["sheets"])
    tampered_sheet = dict(tampered["sheets"][0])
    tampered["sheets"][0] = tampered_sheet
    tampered_sheet["nets"] = list(tampered_sheet["nets"])
    tampered_net = dict(tampered_sheet["nets"][0])
    tampered_sheet["nets"][0] = tampered_net
    tampered_net["pin_ids"] = list(tampered_net["pin_ids"])[1:]
    try:
        validate_archive_input(tampered)
    except InputError:
        pass
    else:
        raise TestFailure("KiCad generator accepted altered archive memberships")

    extra_pin = dict(model)
    extra_pin["sheets"] = list(model["sheets"])
    extra_sheet = dict(extra_pin["sheets"][0])
    extra_pin["sheets"][0] = extra_sheet
    extra_sheet["components"] = list(extra_sheet["components"])
    extra_component = dict(extra_sheet["components"][0])
    extra_sheet["components"][0] = extra_component
    extra_component["pins"] = [*extra_component["pins"], {"id": "UNCONNECTED-TAMPER"}]
    try:
        validate_archive_input(extra_pin)
    except InputError:
        pass
    else:
        raise TestFailure("KiCad generator accepted an extra unconnected pin")


def test_manifest() -> None:
    lines = MANIFEST_PATH.read_text(encoding="utf-8").splitlines()
    entries: list[tuple[str, str]] = []
    for line in lines:
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, f"malformed manifest line: {line!r}")
        entries.append((match.group(1), match.group(2)))
    require([name for _, name in entries] == sorted(name for _, name in entries), "manifest is not sorted")
    actual_files = sorted(
        path.relative_to(ARCHIVE).as_posix()
        for path in ARCHIVE.rglob("*")
        if path.is_file() and path != MANIFEST_PATH
    )
    require([name for _, name in entries] == actual_files, "manifest file set changed")
    for expected_hash, name in entries:
        require(sha256(ARCHIVE / name) == expected_hash, f"manifest hash mismatch: {name}")


def compare_trees(expected: Path, actual: Path, label: str) -> None:
    expected_files = sorted(path.relative_to(expected) for path in expected.rglob("*") if path.is_file())
    actual_files = sorted(path.relative_to(actual) for path in actual.rglob("*") if path.is_file())
    require(actual_files == expected_files, f"{label} regenerated file set changed")
    for relative in expected_files:
        require((actual / relative).read_bytes() == (expected / relative).read_bytes(), f"{label} is not deterministic: {relative}")


def test_regeneration() -> None:
    from sc02_connectivity_generate import generate as generate_connectivity
    from sc02_kicad_generate import generate as generate_kicad
    from sc02_pdf_to_svg import convert as convert_svg
    from sc02_schematic_extract import _write_json, extract_schematic

    with tempfile.TemporaryDirectory(prefix="sc02-archive-test-") as temporary:
        root = Path(temporary)
        regenerated_json = root / MODEL_PATH.name
        _write_json(regenerated_json, extract_schematic(SOURCE_PDF))
        require(regenerated_json.read_bytes() == MODEL_PATH.read_bytes(), "canonical JSON is not deterministic")

        source_svg_dir = root / "source_svg"
        convert_svg(SOURCE_PDF, regenerated_json, source_svg_dir)
        compare_trees(SOURCE_SVG_DIR, source_svg_dir, "source SVG")

        connectivity_dir = root / "connectivity"
        generate_connectivity(regenerated_json, connectivity_dir)
        compare_trees(CONNECTIVITY_DIR, connectivity_dir, "connectivity")

        connectivity_svg_dir = root / "connectivity_svg"
        command = [
            "node",
            str(REPO_ROOT / "scripts" / "sc02_dot_to_svg.mjs"),
            str(connectivity_dir),
            str(connectivity_svg_dir),
        ]
        try:
            result = subprocess.run(
                command,
                cwd=REPO_ROOT,
                check=False,
                capture_output=True,
                text=True,
            )
        except OSError as exc:
            raise TestFailure(f"cannot run pinned Graphviz renderer: {exc}") from exc
        require(
            result.returncode == 0,
            f"pinned Graphviz renderer failed: {(result.stderr or result.stdout).strip()}",
        )
        compare_trees(CONNECTIVITY_SVG_DIR, connectivity_svg_dir, "Graphviz SVG")

        kicad_dir = root / "kicad"
        generate_kicad(regenerated_json, kicad_dir)
        compare_trees(KICAD_DIR, kicad_dir, "KiCad")


def parse_args(argv: Iterable[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--regenerate",
        action="store_true",
        help="rebuild Python-generated files in a temp directory and compare bytes",
    )
    return parser.parse_args(argv)


def main(argv: Iterable[str] | None = None) -> int:
    args = parse_args(argv)
    try:
        model = load_model()
        test_source_and_model(model)
        test_csv(model)
        test_svg_and_dot()
        test_kicad()
        test_kicad_validation()
        test_kicad_generator_guards(model)
        test_manifest()
        if args.regenerate:
            test_regeneration()
    except (TestFailure, OSError, KeyError, TypeError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    mode = "including byte-for-byte regeneration" if args.regenerate else "committed artifacts"
    print(f"PASS: SC-02 schematic archive ({mode})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
