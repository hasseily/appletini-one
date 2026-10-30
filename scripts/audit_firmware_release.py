#!/usr/bin/env python3
"""Audit explicit release inputs, ELF loads and packaged bytes; never flash."""
from __future__ import annotations

import argparse
from dataclasses import asdict
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import struct
import sys
import zipfile

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build"
sys.path.insert(0, str(ROOT / "scripts"))
import image_manifest
import package_timing_firmware as timing_manifest


def need(condition, message):
    if not condition:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def unpack(fmt, data, offset):
    need(0 <= offset <= len(data) - struct.calcsize(fmt),
         f"Invalid {fmt} range at {offset:#x}")
    return struct.unpack_from(fmt, data, offset)


def checked_bytes(data, start, size):
    need(0 <= start <= len(data) and 0 <= size <= len(data) - start,
         f"Invalid byte range {start:#x}+{size:#x}")
    return data[start:start + size]


def elf32(data):
    need(data[:7] == b"\x7fELF\x01\x01\x01", "Expected little-endian ELF32")
    h = unpack("<HHIIIIIHHHHHH", data, 16)
    need(h[0] == 2 and h[1] == 40, "Expected executable ARM ELF")
    entry, phoff, shoff = h[3:6]
    phsize, phnum, shsize, shnum = h[8:12]
    need(phsize >= 32 and 0 < phnum < 1024, "Invalid ELF program table")
    checked_bytes(data, phoff, phsize * phnum)
    segments = []
    for i in range(phnum):
        kind, offset, vaddr, paddr, filesz, memsz, flags, align = unpack(
            "<8I", data, phoff + i * phsize)
        if kind == 1:
            need(filesz <= memsz and paddr + memsz <= 0x100000000,
                 "Invalid ELF load segment")
            segments.append({"vaddr": vaddr, "paddr": paddr, "filesz": filesz,
                             "memsz": memsz, "data": checked_bytes(data, offset, filesz)})
    need(any(s["filesz"] for s in segments), "ELF has no file-backed loads")
    need(shsize >= 40 and 0 < shnum < 65536, "Invalid ELF section table")
    checked_bytes(data, shoff, shsize * shnum)
    sections = [unpack("<10I", data, shoff + i * shsize) for i in range(shnum)]
    symbols = {}
    for s in sections:
        if s[1] != 2:
            continue
        need(s[6] < shnum and s[9] == 16 and s[5] % 16 == 0,
             "Invalid ELF symbol table")
        strings = sections[s[6]]
        names = checked_bytes(data, strings[4], strings[5])
        checked_bytes(data, s[4], s[5])
        for offset in range(s[4], s[4] + s[5], s[9]):
            name, value, size, info, other, index = unpack("<IIIBBH", data, offset)
            need(name < len(names), "Invalid ELF symbol name")
            end = names.find(b"\0", name)
            need(end >= name, "Unterminated ELF symbol name")
            symbols[names[name:end].decode("utf-8", errors="replace")] = value
    return entry, segments, sections, symbols


def mapped_bytes(segments, address, size, field="vaddr"):
    result = bytearray()
    cursor = address
    end = address + size
    while cursor < end:
        matches = [s for s in segments if s[field] <= cursor < s[field] + s["filesz"]]
        need(len(matches) == 1, f"No unique ELF mapping at {cursor:#x}")
        s = matches[0]
        count = min(end, s[field] + s["filesz"]) - cursor
        result.extend(s["data"][cursor - s[field]:cursor - s[field] + count])
        cursor += count
    return bytes(result)


def elf_raw_binary(data, segments, sections):
    """Reproduce objcopy's loadable, allocated, file-backed section image."""
    spans = []
    for s in sections:
        if not s[2] & 2 or s[1] == 8 or not s[5]:
            continue
        matches = [p for p in segments if p["vaddr"] <= s[3] and
                   s[3] + s[5] <= p["vaddr"] + p["filesz"]]
        need(len(matches) == 1, "Allocated section has no unique load segment")
        p = matches[0]
        address = p["paddr"] + s[3] - p["vaddr"]
        content = checked_bytes(data, s[4], s[5])
        need(content == mapped_bytes(segments, s[3], s[5]),
             "ELF section bytes differ from load bytes")
        spans.append((address, content))
    need(spans, "ELF has no loadable file-backed sections")
    spans.sort()
    start = spans[0][0]
    end = max(a + len(b) for a, b in spans)
    need(end - start <= 64 * 1024 * 1024, "Unexpected raw ELF span")
    image = bytearray(end - start)
    previous_end = start
    for address, content in spans:
        need(address >= previous_end, "Overlapping raw ELF sections")
        image[address - start:address - start + len(content)] = content
        previous_end = address + len(content)
    return bytes(image), start


def bit_payload(data):
    pos = 2 + unpack(">H", data, 0)[0]
    need(unpack(">H", data, pos)[0] == 1, "Invalid .bit header")
    pos += 2
    fields = {}
    for expected in b"abcde":
        need(checked_bytes(data, pos, 1) == bytes([expected]), "Invalid .bit tag order")
        pos += 1
        width = 4 if expected == ord("e") else 2
        size = unpack(">I" if width == 4 else ">H", data, pos)[0]
        pos += width
        content = checked_bytes(data, pos, size)
        pos += size
        if expected != ord("e"):
            fields[chr(expected)] = content.rstrip(b"\0").decode("ascii", errors="replace")
    need(pos == len(data) and len(content) % 4 == 0, "Invalid .bit payload size/tail")
    return content, fields


def bootgen_partitions(raw):
    need(unpack("<II", raw, 0x20) == (0xAA995566, 0x584C4E58), "Invalid Zynq boot header")
    need(sum(unpack("<11I", raw, 0x20)) & 0xFFFFFFFF == 0xFFFFFFFF,
         "Invalid Zynq boot header checksum")
    need(unpack("<I", raw, 0x28)[0] == 0, "Encrypted image unsupported")
    iht, pht = unpack("<II", raw, 0x98)
    version, count, pht_words, ih_words = unpack("<4I", raw, iht)
    need(version == 0x01020000 and pht_words * 4 == pht and 1 <= count <= 32,
         "Unexpected Zynq image table")
    checked_bytes(raw, pht, count * 64)
    names, seen = {}, set()
    while ih_words:
        ih = ih_words * 4
        need(ih not in seen and len(seen) < 32, "Cyclic/excessive image headers")
        seen.add(ih)
        ih_words, first_pht, reserved, nparts = unpack("<4I", raw, ih)
        need(0 < nparts <= count, "Invalid image partition count")
        encoded = checked_bytes(raw, ih + 16, 112)
        decoded = b"".join(encoded[i:i + 4][::-1] for i in range(0, 112, 4))
        name = decoded.split(b"\0", 1)[0].decode("ascii")
        need(name, "Empty Bootgen image name")
        for i in range(nparts):
            offset = first_pht * 4 + i * 64
            need(offset not in names and pht <= offset < pht + count * 64,
                 "Duplicate/outside image partition assignment")
            names[offset] = name
    result = []
    for i in range(count):
        offset = pht + i * 64
        w = unpack("<16I", raw, offset)
        need(sum(w) & 0xFFFFFFFF == 0xFFFFFFFF, "Invalid partition header checksum")
        need(w[0] == w[1] == w[2] and w[8] == w[10] == 0,
             "Expected plain, unsigned Bootgen partition")
        start, length = w[5] * 4, w[1] * 4
        need(length > 0 and offset in names, "Empty/unassigned Bootgen partition")
        payload = checked_bytes(raw, start, length)
        need(start >= pht + count * 64, "Partition overlaps its headers")
        result.append({"image": names[offset], "load_address": w[3], "entry_address": w[4],
                       "offset": start, "length": length, "attributes": w[6],
                       "sha256": digest(payload)})
    ordered = sorted(result, key=lambda p: p["offset"])
    need(all(a["offset"] + a["length"] <= b["offset"]
             for a, b in zip(ordered, ordered[1:])), "Bootgen payload partitions overlap")
    p = result[0]
    need(unpack("<5I", raw, 0x30) ==
         (p["offset"], p["length"], p["load_address"], p["entry_address"], p["length"]),
         "Boot header FSBL fields differ from first partition")
    return result


def check_elf(raw, parts, data, label):
    entry, segments, sections, symbols = elf32(data)
    need(parts and any(p["entry_address"] == entry for p in parts),
         f"Wrong {label} execution address")
    ordered = sorted(parts, key=lambda p: p["load_address"])
    need(all(a["load_address"] + a["length"] <= b["load_address"]
             for a, b in zip(ordered, ordered[1:])), f"Overlapping {label} load partitions")
    used, checks = set(), []
    for s in segments:
        cursor, end = s["paddr"], s["paddr"] + s["filesz"]
        chunks = []
        while cursor < end:
            matches = [(i, p) for i, p in enumerate(parts)
                       if p["load_address"] <= cursor < p["load_address"] + p["length"]]
            need(len(matches) == 1, f"No unique {label} partition at {cursor:#x}")
            i, p = matches[0]
            size = min(end, p["load_address"] + p["length"]) - cursor
            start = p["offset"] + cursor - p["load_address"]
            need(raw[start:start + size] == s["data"][cursor - s["paddr"]:cursor - s["paddr"] + size],
                 f"Packaged {label} load bytes differ at {cursor:#x}")
            chunks.append({"partition": i, "address": cursor, "bytes": size})
            used.add(i)
            cursor += size
        if s["filesz"]:
            checks.append({"load_address": s["paddr"], "file_bytes": s["filesz"],
                           "sha256": digest(s["data"]), "partition_chunks": chunks})
    need(used == set(range(len(parts))), f"Unexpected extra {label} partition")
    # Check gaps/alignment bytes as well. BSS is not required to be stored;
    # Bootgen may store zero fill between file-backed ranges it combines.
    zero_fill = 0
    for p in parts:
        expected = bytearray(p["length"])
        covered = []
        for s in segments:
            start = max(p["load_address"], s["paddr"])
            end = min(p["load_address"] + p["length"], s["paddr"] + s["filesz"])
            if start < end:
                expected[start - p["load_address"]:end - p["load_address"]] = s["data"][start - s["paddr"]:end - s["paddr"]]
                covered.append((start, end))
        need(covered and min(a for a, b in covered) == p["load_address"],
             f"Unexpected leading {label} partition bytes")
        last = max(b for a, b in covered)
        need(p["load_address"] + p["length"] - last < 4,
             f"Unexpected trailing {label} partition extent")
        need(raw[p["offset"]:p["offset"] + p["length"]] == expected,
             f"Unexpected nonzero {label} gap/alignment bytes")
        zero_fill += p["length"] - sum(b - a for a, b in covered)
    return {"entry_address": entry, "segments": checks, "zero_fill_bytes": zero_fill,
            "all_file_backed_bytes_match": True}, segments, sections, symbols


def check_timing(path, bit, xsa, candidate=None):
    m = timing_manifest.read_manifest(path)
    for key, expected in {"status": "exported", "build_mode": "full", "synthesis_reused": "0",
                          "route_status": "PASS", "bus_skew_status": "PASS",
                          "constraint_bounds_status": "PASS"}.items():
        need(m.get(key) == expected, f"Timing {key} must be {expected}")
    need(m.get("incremental_reference", "") == "", "Unexpected incremental reference")
    def number(key):
        value = float(m[key])
        need(math.isfinite(value), f"Non-finite timing {key}")
        return value
    floor = number("minimum_wns_ns")
    need(floor >= 0 and number("wns_ns") >= floor and number("wns_ns") > 0,
         "Setup slack fails the manifest's gate")
    need(number("implementation_setup_margin_ns") >= 0.200, "Missing implementation setup margin")
    for key in ("whs_ns", "wpws_ns"):
        need(number(key) >= 0, f"Negative timing {key}")
    for key in ("tns_ns", "ths_ns", "tpws_ns", "setup_failing_endpoints", "hold_failing_endpoints",
                "pulse_width_failing_endpoints", "unconstrained_internal_endpoints", "route_errors",
                "missing_constraint_objects", "final_fabric_user_uncertainty_ns",
                "final_pixel_user_uncertainty_ns"):
        need(number(key) == 0, f"Timing {key} must be zero")
    for key, expected in (("final_direction_a_limit_ns", 10), ("final_direction_d_limit_ns", 10),
                          ("final_phi0_release_limit_ns", 8)):
        need(number(key) == expected, f"Unexpected nominal bound {key}")
    need(m.get("bitstream_sha256") == digest(bit), "Named bitstream differs from timing manifest")
    need(m.get("xsa_sha256") == digest(xsa), "Named XSA differs from timing manifest")
    if candidate is not None:
        need(m.get("candidate_dcp_sha256") == digest(candidate), "Named DCP differs from timing manifest")
    return m


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("firmware", "fsbl", "frontend", "bitstream", "xsa", "run-manifest"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--boot-version", required=True)
    parser.add_argument("--candidate-dcp", type=Path)
    parser.add_argument("--platform-xsa", type=Path, action="append", default=[])
    parser.add_argument("--core1-elf", type=Path)
    parser.add_argument("--core1-blob", type=Path)
    parser.add_argument("--output", type=Path, default=OUT / "FIRMWARE_AUDIT.json")
    args = parser.parse_args()
    output = args.output.resolve()
    need(output.is_relative_to(ROOT) and not output.exists(),
         "Audit output must be a new file inside this project")
    need(bool(args.core1_elf) == bool(args.core1_blob), "Provide both core1 ELF and blob")
    paths = {name: getattr(args, name).resolve() for name in
             ("firmware", "fsbl", "frontend", "bitstream", "xsa", "run_manifest")}
    for name in ("candidate_dcp", "core1_elf", "core1_blob"):
        if getattr(args, name):
            paths[name] = getattr(args, name).resolve()
    for i, path in enumerate(args.platform_xsa):
        paths[f"platform_xsa_{i}"] = path.resolve()
    data = {name: path.read_bytes() for name, path in paths.items()}
    timing = check_timing(paths["run_manifest"], data["bitstream"], data["xsa"], data.get("candidate_dcp"))
    for name in paths:
        if name.startswith("platform_xsa_"):
            need(data[name] == data["xsa"], f"{name} differs from named XSA")
    with zipfile.ZipFile(paths["xsa"]) as z:
        bits = [name for name in z.namelist() if name.lower().endswith(".bit")]
        need(len(bits) == 1 and z.read(bits[0]) == data["bitstream"], "XSA embeds a different bitstream")
        xsa_member = bits[0]
    footer = image_manifest.verify_image(paths["firmware"], image_manifest.ROLE_FIRMWARE, True)
    need(len(data["firmware"]) <= 0x00600000, "Firmware exceeds the 6 MiB flash slot")
    raw = data["firmware"][:footer.payload_size]
    parts = bootgen_partitions(raw)
    expected_names = {paths[name].name for name in ("fsbl", "frontend", "bitstream")}
    need({p["image"] for p in parts} == expected_names, "Unexpected Bootgen image set")
    fsbl_parts = [p for p in parts if p["image"] == paths["fsbl"].name]
    need(len(fsbl_parts) == 1 and parts[0] is fsbl_parts[0], "Expected one first FSBL partition")
    for p in parts:
        need(p["attributes"] == (0x20 if p["image"] == paths["bitstream"].name else 0x10),
             "Wrong PS/PL destination attributes")
    pl = [p for p in parts if p["image"] == paths["bitstream"].name]
    need(len(pl) == 1, "Expected exactly one PL partition")
    expected, bit_fields = bit_payload(data["bitstream"])
    padding = (-len(expected)) % 32
    swapped = b"".join(expected[i:i + 4][::-1] for i in range(0, len(expected), 4))
    native = expected + b"\x20\0\0\0" * (padding // 4)
    swapped += b"\0\0\0\x20" * (padding // 4)
    actual = raw[pl[0]["offset"]:pl[0]["offset"] + pl[0]["length"]]
    need(actual in (native, swapped), "Packaged FPGA differs from .bit payload/NOOP padding")
    elf_checks = {}
    front_segments = front_sections = front_symbols = None
    for key in ("fsbl", "frontend"):
        selected = [p for p in parts if p["image"] == paths[key].name]
        check, segments, sections, symbols = check_elf(raw, selected, data[key], key)
        elf_checks[key] = check
        if key == "frontend":
            front_segments, front_sections, front_symbols = segments, sections, symbols
    label = ("Firmware " + args.version + "\0").encode("ascii")
    boot_label = ("Boot " + args.boot_version + "\0").encode("ascii")
    need(label in b"".join(s["data"] for s in front_segments), "Wrong loaded frontend version")
    need(boot_label not in b"".join(s["data"] for s in front_segments), "Frontend still embeds a misleading static boot label")
    front_bytes = b"".join(s["data"] for s in front_segments)
    golden_display = b"Golden Boot: %s\0" in front_bytes or (
        b"%s: %s   Available: %s\0" in front_bytes and
        b"Golden Boot\0" in front_bytes and
        "menu_platform_get_golden_boot_version" in front_symbols)
    need(golden_display, "Missing installed Golden display format")
    need("mb_golden_installed_version" in front_symbols and "golden_self_update_run" in front_symbols, "Missing installed-version/update implementation")
    core1 = {"checked": False}
    if args.core1_elf:
        entry, segments, sections, symbols = elf32(data["core1_elf"])
        binary, address = elf_raw_binary(data["core1_elf"], segments, sections)
        blob = data["core1_blob"]
        need(binary and binary == blob, "CPU1 raw allocated-section image differs from generated blob")
        start, end = front_symbols["core1_blob_start"], front_symbols["core1_blob_end"]
        need(end - start == len(blob) and mapped_bytes(front_segments, start, len(blob)) == blob,
             "Frontend CPU1 symbol span differs from generated blob")
        need(unpack("<I", mapped_bytes(front_segments, front_symbols["core1_blob_size"], 4), 0)[0] == len(blob),
             "Frontend CPU1 size word differs")
        core1 = {"checked": True, "raw_binary_matches_generated_blob": True,
                 "frontend_symbol_span_matches": True, "frontend_size_word_matches": True,
                 "packaged_frontend_load_bytes_match": True, "blob_address": start,
                 "raw_binary_load_address": address, "blob_size_bytes": len(blob), "blob_sha256": digest(blob)}
    for name, path in paths.items():
        need(path.read_bytes() == data[name], f"Input changed during audit: {name}")
    report = {"status": "PASS", "version": args.version, "boot_version": args.boot_version,
              "audited_utc": datetime.now(timezone.utc).isoformat(), "timing": timing,
              "inputs": {name: {"path": str(path), "size_bytes": len(data[name]), "sha256": digest(data[name])}
                         for name, path in paths.items()}, "manifest": asdict(footer), "partitions": parts,
              "elf_checks": elf_checks, "core1": core1,
              "bitstream": {"xsa_member": xsa_member, "xsa_embedded_bitstream_matches": True,
                            "configuration_matches": True, "endian": "native" if actual == native else "word-swapped",
                            "noop_padding_bytes": padding, "header_fields": bit_fields},
              "limitations": ["No hardware boot, listening, flash, or physical validation performed.",
                              "Binary identity and declared timing gates do not establish source-to-build identity without a separate source snapshot and build logs.",
                              "The audit supports plain unsigned unencrypted Zynq images; unsupported layouts fail.",
                              "ELF BSS is not stored in the image; startup zeroing is not executed by this audit.",
                              "Platform XSA identity is checked only for explicit --platform-xsa inputs; PS build freshness needs build evidence."],
              "audit_script": {"path": str(Path(__file__).resolve()), "sha256": digest(Path(__file__).read_bytes())}}
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8", newline="\n")
    print(json.dumps({"status": "PASS", "version": args.version,
                      "firmware_sha256": digest(data["firmware"]), "partitions": len(parts),
                      "bitstream_endian": report["bitstream"]["endian"], "core1_checked": core1["checked"],
                      "report": str(output)}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, struct.error, zipfile.BadZipFile) as exc:
        print(f"AUDIT FAILED: {exc}", file=sys.stderr)
        raise SystemExit(1)
