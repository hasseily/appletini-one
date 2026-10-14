"""Summarize completed GSSquared PC captures without controlling the emulator.

Rerun while captures are being collected. Incomplete/mismatched trios are
reported as skipped. Generated JSON/CSV/Markdown stay under analysis/.
"""
from __future__ import annotations

import argparse
from bisect import bisect_right
from collections import Counter, defaultdict
import csv
import hashlib
import io
import json
from pathlib import Path


ROOT = Path(__file__).resolve().parent
GROUPS = {
    "math": "FADD FADDT FSUB FSUBT FMULT FMULTT FDIV FDIVT MUL10 DIV10 FCOMP FCOMP2 ROUND_FAC INT LOG EXP SQR FPWRT RND SIN COS TAN ATN",
    "lookup": "PTRGET PTRGET2 PTRGET3 SCALAR_LOOKUP_LOOP_START MAKE_NEW_VARIABLE FNDLIN FL1 FRM_VARIABLE",
    "arrays": "ARRAY ARRAY_LOOKUP_START MAKE_NEW_ARRAY FIND_ARRAY_ELEMENT MULTIPLY_SUBSCRIPT MULTIPLY_SUBS_1",
    "literals": "FIN LINGET VAL ADDACC GETEXP INPUT READ",
    "number_output": "FOUT FOUT_1 PRINT_FAC LINPRT STR",
    "strings_gc": "GARBAG FIND_HIGHEST_STRING MOVE_HIGHEST_STRING_TO_TOP GETSPA CAT MOVSTR STRLIT LEFTSTR RIGHTSTR MIDSTR STRCMP",
    "parser": "RAM_CHRGET RAM_CHRGOT FRMEVL FRM_ELEMENT EXECUTE_STATEMENT NEWSTT",
    "loops": "FOR NEXT NEXT_1 GOTO GOSUB RETURN IF ONGOTO",
    "io": "ISCNTC PRINT OUTDO MON_COUT MON_RDKEY MON_HOME MON_PREAD MON_WAIT PEEK POKE WAIT",
}
# A deliberately narrow subset, rather than all of the broad arithmetic category.
# Shared loads/stores, numeric conversion, expression parsing, and transcendental
# orchestration remain unchanged in this what-if. Boundaries are ROM-pinned.
FP_KERNELS = [
    {"start": 0xE7A0, "end_exclusive": 0xE8DA, "label": "add/subtract, normalization, complement, overflow path"},
    {"start": 0xE97F, "end_exclusive": 0xE9E3, "label": "multiply entries and bit loop"},
    {"start": 0xEA66, "end_exclusive": 0xEAE6, "label": "divide entries and bit loop"},
]
TARGET_REGIONS = [
    ("scalar_table_scan", 0xE053, 0xE07D),
    ("array_table_scan", 0xE169, 0xE196),
    ("array_index_multiply", 0xE2AD, 0xE2DE),
    ("ram_chrget", 0x00B1, 0x00C9),
    ("line_search", 0xD61A, 0xD649),
    ("decimal_input_FIN_body", 0xEC4A, 0xED0A),
    ("decimal_output_FOUT_body", 0xED34, 0xEE64),
    ("garbage_collector_body", 0xE484, 0xE597),
    ("statement_keyboard_poll", 0xD858, 0xD863),
]


def percent(value, total):
    return 100.0 * value / total if total else 0.0


def load_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def number(value):
    value = str(value).strip()
    return int(value, 16) if value.lower().startswith("0x") else int(value)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--study-root", type=Path, default=ROOT)
    args = parser.parse_args()
    root = args.study_root.resolve()
    out = root / "analysis"
    out.mkdir(exist_ok=True)
    map_path = root / "maps" / "applesoft_enhanced_map.json"
    mapping = load_json(map_path)
    intervals = mapping["intervals"]
    starts = [i["start"] for i in intervals]
    entries = list(mapping["entries"])
    # The ordinary PRINT path enters FOUT at ED36, bypassing FOUT's ED34
    # C=0 setup. This address is in the pinned disassembly and Enhanced ROM.
    # Include it here so the rollup can work with the already-issued map v1.
    if not any(e["name"] == "FOUT_1" for e in entries):
        entries.append({"name": "FOUT_1", "address": 0xED36, "address_hex": "0xED36"})
    by_name = {e["name"]: e for e in entries}
    entry_addresses = sorted(set(e["address"] for e in entries))
    names_at = defaultdict(list)
    for e in entries:
        names_at[e["address"]].append(e["name"])

    def interval_at(pc):
        return intervals[bisect_right(starts, pc) - 1]

    def chrget_status(run):
        signature = bytes.fromhex(by_name["RAM_CHRGET"]["signature_hex"])
        mask = bytes.fromhex(by_name["RAM_CHRGET"]["signature_mask_hex"])
        checks = {}
        for side in ("start", "end"):
            value = run.get(side, {}).get("zero_page_hex")
            if not value:
                checks[side] = None
                continue
            zp = bytes.fromhex(value)
            actual = zp[0xB1:0xB1 + len(signature)]
            checks[side] = len(actual) == len(signature) and all(
                (a & m) == (b & m) for a, b, m in zip(actual, signature, mask)
            )
        checks["stock_at_both_snapshots"] = checks["start"] is True and checks["end"] is True
        return checks

    def summarize(hist, irq_cycles=0, chrget_stock=True):
        total_i = sum(v[0] for v in hist.values())
        total_c = sum(v[1] for v in hist.values())
        cat_i = Counter()
        cat_c = Counter()
        for pc, (instructions, cycles) in hist.items():
            category = interval_at(pc)["category"]
            if 0xB1 <= pc < 0xC9 and not chrget_stock:
                category = "DOS/user"
            cat_i[category] += instructions
            cat_c[category] += cycles
        categories = [{
            "category": category,
            "instructions": cat_i[category], "cycles": cat_c[category],
            "instruction_share_pct": percent(cat_i[category], total_i),
            "cycle_share_pct": percent(cat_c[category], total_c),
        } for category in mapping["categories"]]
        categories.sort(key=lambda x: -x["cycles"])
        top = []
        for pc, (instructions, cycles) in sorted(hist.items(), key=lambda item: (-item[1][1], item[0]))[:20]:
            index = bisect_right(entry_addresses, pc) - 1
            nearest = entry_addresses[index] if index >= 0 else None
            interval = interval_at(pc)
            # Do not carry an unrelated entry label across a category boundary.
            if nearest is not None and nearest < interval["start"]:
                nearest = None
            top.append({
                "pc": pc, "pc_hex": f"${pc:04X}",
                "instructions": instructions, "cycles": cycles,
                "cycle_share_pct": percent(cycles, total_c),
                "category": "DOS/user" if 0xB1 <= pc < 0xC9 and not chrget_stock else interval["category"],
                "interval_label": interval["label"],
                "nearest_named_entry": None if nearest is None else "/".join(names_at[nearest]),
                "offset_from_named_entry": None if nearest is None else pc - nearest,
            })
        visits = []
        for group, names in GROUPS.items():
            for name in names.split():
                e = by_name[name]
                instructions, cycles = hist.get(e["address"], (0, 0))
                visits.append({
                    "group": group, "entry": name, "pc": e["address"],
                    "pc_hex": e["address_hex"], "instruction_visits": instructions,
                    "cycles_at_entry_instruction_only": cycles,
                })
        kernel_c = sum(c for pc, (_, c) in hist.items() if any(
            k["start"] <= pc < k["end_exclusive"] for k in FP_KERNELS
        ))
        accounted = total_c + irq_cycles
        fraction = kernel_c / accounted if accounted else 0
        amdahl = {
            "selected_kernel_cycles": kernel_c,
            "selected_kernel_share_of_instruction_cycles_pct": percent(kernel_c, total_c),
            "selected_kernel_share_of_accounted_cycles_pct": percent(kernel_c, accounted),
            "assumed_kernel_speedup": 10,
            "hypothetical_overall_speedup": 1 / (1 - fraction + fraction / 10),
            "hypothetical_accounted_cycles": accounted - kernel_c + kernel_c / 10,
            "interpretation": "What-if only: exactly 10x faster execution in the selected FP instruction ranges, all other cycles unchanged, and no added trap/transfer overhead. This is not a measured or promised speedup.",
        }
        regions = []
        for name, low, high in TARGET_REGIONS:
            cycles = sum(c for pc, (_, c) in hist.items() if low <= pc < high)
            regions.append({"region": name, "start": low, "end_exclusive": high,
                            "cycles": cycles, "cycle_share_pct": percent(cycles, total_c)})
        return dict(instructions=total_i, instruction_cycles=total_c,
                    irq_cycles=irq_cycles, accounted_cycles=accounted,
                    category_shares=categories, top20_pcs=top,
                    entry_instruction_visits=visits, target_regions=regions,
                    fp_kernel_10x_what_if=amdahl)

    captures = []
    histograms = {}
    skipped = []
    for path in sorted((root / "captures").glob("*.csv")):
        runid = path.stem
        if runid.upper().startswith("VERIFY_"):
            continue
        plain_path = path.with_suffix(".json")
        run_path = path.with_suffix(".run.json")
        missing = [p.name for p in (plain_path, run_path) if not p.exists()]
        if missing:
            skipped.append({"runid": runid, "reason": "incomplete capture trio", "missing": missing})
            continue
        try:
            plain = load_json(plain_path)
            run = load_json(run_path)
            raw = path.read_bytes()
            hist = {}
            for row in csv.DictReader(io.StringIO(raw.decode("utf-8-sig"))):
                pc = number(row["pc"])
                assert 0 <= pc < 0x10000
                assert pc not in hist, "duplicate PC row"
                hist[pc] = (number(row["instructions"]), number(row["cycles"]))
            hist_i = sum(v[0] for v in hist.values())
            hist_c = sum(v[1] for v in hist.values())
            assert hist_i == plain["histogram_instructions"] == plain["instructions"], "instruction totals differ"
            assert hist_c == plain["histogram_cycles"] == plain["instruction_cycles"], "cycle totals differ"
            irq_c = plain.get("irq_cycles", 0)
            assert hist_c + irq_c == plain["accounted_cycles"], "accounted cycles differ"
            assert plain["runid"] == runid == run["runid"], "run ids differ"
            guard = chrget_status(run)
            result = summarize(hist, irq_c, guard["stock_at_both_snapshots"])
            start_pc = run.get("start", {}).get("registers", {}).get("pc")
            end_pc = run.get("end", {}).get("registers", {}).get("pc")
            budget_stop = plain.get("reason") == "instruction_budget"
            boundary = "exact instruction budget" if budget_stop else "BASIC prompt" if run.get("finished_at_prompt") else (
                "keyboard request" if run.get("keyboard_stop") and end_pc == 0xFD0C else
                "instruction budget / other partial boundary"
            )
            error_lines = [line.strip() for line in run.get("end", {}).get("text", [])
                           if line.lstrip().startswith("?") and "ERROR" in line]
            start_error_lines = [line.strip() for line in run.get("start", {}).get("text", [])
                                 if line.lstrip().startswith("?") and "ERROR" in line]
            new_errors = [line for line in error_lines if line not in start_error_lines]
            failed = bool(new_errors)
            snapshot_cycle = run.get("end", {}).get("registers", {}).get("cycle")
            profiler_end_cycle = plain.get("end_guest_cycle")
            snapshot_lag = (snapshot_cycle - profiler_end_cycle
                            if snapshot_cycle is not None and profiler_end_cycle is not None else None)
            result.update({
                "program": run["program"], "runid": runid,
                "tokenized_sha256": run.get("tokenized_sha256"),
                "csv_sha256": hashlib.sha256(raw).hexdigest(),
                "sources": [str(p.relative_to(root)) for p in (path, plain_path, run_path)],
                "profiler_reason": plain.get("reason"),
                "instruction_limit": plain.get("instruction_limit", run.get("limit")),
                "start_pc": start_pc, "end_snapshot_pc": end_pc,
                "profiler_end_guest_cycle": profiler_end_cycle,
                "end_snapshot_guest_cycle": snapshot_cycle,
                "snapshot_after_profile_cycles": snapshot_lag,
                "end_pc": end_pc if snapshot_lag == 0 else None,
                "end_boundary": boundary, "finished_at_prompt": run.get("finished_at_prompt", False),
                "boundary_note": run.get("boundary_note"),
                "program_status": "failed: BASIC error" if failed else "captured",
                "basic_error_lines": new_errors,
                "include_in_priority_aggregates": not failed,
                "guest_cycle_span": plain.get("guest_cycle_span"),
                "guest_span_equals_accounted_cycles": plain.get("guest_cycle_span") == result["accounted_cycles"],
                "capture_wall_seconds": plain.get("capture_wall_seconds"),
                "chrget_snapshot_guard": guard,
            })
            captures.append(result)
            histograms[runid] = hist
        except (OSError, ValueError, KeyError, AssertionError) as error:
            skipped.append({"runid": runid, "reason": str(error)})

    programs = []
    grouped = defaultdict(list)
    for capture in captures:
        if capture["include_in_priority_aggregates"]:
            grouped[capture["program"]].append(capture)
    failed_captures = [r for r in captures if not r["include_in_priority_aggregates"]]
    for program, runs in sorted(grouped.items()):
        combined = defaultdict(lambda: [0, 0])
        for run in runs:
            for pc, (instructions, cycles) in histograms[run["runid"]].items():
                combined[pc][0] += instructions
                combined[pc][1] += cycles
        stock = all(r["chrget_snapshot_guard"]["stock_at_both_snapshots"] for r in runs)
        result = summarize(combined, sum(r["irq_cycles"] for r in runs), stock)
        result.update({"program": program, "capture_count": len(runs),
                       "runids": [r["runid"] for r in runs],
                       "end_boundaries": sorted(set(r["end_boundary"] for r in runs)),
                       "aggregation": "Sum of available captures for this program; repeated or partial captures remain separately available."})
        programs.append(result)

    limitations = [
        "All cycle shares are exclusive executed-instruction PC cycles, not inclusive routine time. IRQ entry cycles are separate.",
        "Routine counters are visits to an entry PC. Fall-through paths and loop backedges can visit multiple entry PCs in one call; never sum them as independent calls.",
        "These are stock guest 65C02 instruction-cycle profiles from GSSquared. They are not physical vTW wall-time or speedup measurements.",
        "Host capture wall time includes control/pause overhead and is not a clean throughput benchmark.",
        "The PC histogram lacks per-sample banking state. ROM classification assumes the validated ROM remains mapped; programs switching language-card RAM can require a separate bank-aware capture.",
        "Stock CHRGET bytes are checked at start/end snapshots with the text-pointer bytes masked. These two snapshots cannot detect transient patching during a capture.",
        "FIN's own instructions count as literals/number input; its floating-point callees count as arithmetic. NEXT and graphics have the same exclusive attribution rule.",
        "Monitor/IO includes monitor screen and low-res graphics routines as well as I/O. DOS/user includes DOS hooks and application machine code.",
        "The 10x FP what-if covers only three fixed kernel ranges. Shared math loads/stores, most conversion, parsing, call/return overhead outside those ranges, and helper-transfer costs are excluded; actual replacement overhead is unknown.",
        "Program aggregates weight captures by executed cycles. Do not compare finite completion, keyboard-startup, and instruction-budget windows as equivalent whole-program runs.",
        "BASIC error captures remain in the raw capture analysis but are excluded from program/priority aggregates. An end-screen error is reported, not diagnosed by this script.",
        "For instruction-budget captures, the profiler stops exactly at its recorded cycle boundary; the emulator can keep running before the end snapshot. A later snapshot PC/screen is not the histogram's endpoint.",
        "Interactive key-response phases begin after RDKEY returns and stop before the next keyboard wait, as recorded by boundary_note. Their totals exclude the wait itself.",
    ]
    validation_path = root / "capture-validation.json"
    payload = {
        "schema_version": 1,
        "map_path": str(map_path.relative_to(root)),
        "map_sha256": hashlib.sha256(map_path.read_bytes()).hexdigest(),
        "rom_sha256": mapping["metadata"]["rom_sha256"],
        "capture_validation": load_json(validation_path) if validation_path.exists() else None,
        "fp_kernel_ranges": FP_KERNELS,
        "limitations": limitations, "captures": captures, "programs": programs,
        "failed_captures": [{k: r[k] for k in ("program", "runid", "program_status", "basic_error_lines", "instructions", "instruction_cycles")} for r in failed_captures],
        "skipped": skipped,
    }
    (out / "profiles.json").write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    def write_csv(filename, fields, rows):
        with (out / filename).open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    all_rows = [("capture", r["runid"], r) for r in captures] + [("program", r["program"], r) for r in programs]
    write_csv("category_shares.csv", ["scope", "id", "program", "category", "instructions", "cycles", "instruction_share_pct", "cycle_share_pct"], (
        dict(scope=scope, id=identifier, program=r["program"], **c)
        for scope, identifier, r in all_rows for c in r["category_shares"]
    ))
    write_csv("routine_entries.csv", ["scope", "id", "program", "group", "entry", "pc", "pc_hex", "instruction_visits", "cycles_at_entry_instruction_only"], (
        dict(scope=scope, id=identifier, program=r["program"], **e)
        for scope, identifier, r in all_rows for e in r["entry_instruction_visits"]
    ))
    write_csv("top_pcs.csv", ["scope", "id", "program", "pc", "pc_hex", "instructions", "cycles", "cycle_share_pct", "category", "interval_label", "nearest_named_entry", "offset_from_named_entry"], (
        dict(scope=scope, id=identifier, program=r["program"], **p)
        for scope, identifier, r in all_rows for p in r["top20_pcs"]
    ))
    write_csv("target_regions.csv", ["scope", "id", "program", "region", "start", "end_exclusive", "cycles", "cycle_share_pct"], (
        dict(scope=scope, id=identifier, program=r["program"], **region)
        for scope, identifier, r in all_rows for region in r["target_regions"]
    ))

    lines = ["# Applesoft stock-ROM PC profiles", "",
             f"Completed capture trios: **{len(captures)}**. Programs in priority aggregates: **{len(programs)}**. Failed captures: **{len(failed_captures)}**. Skipped/incomplete: **{len(skipped)}**.", "",
             "Shares below use exclusive guest instruction cycles. They do not measure physical vTW throughput.", "",
             "| Program | Captures | End boundary | Instructions | Instruction cycles | Largest exclusive categories | Hypothetical FP subset 10x |",
             "|---|---:|---|---:|---:|---|---:|"]
    for r in programs:
        dominant = "; ".join(f"{c['category']} {c['cycle_share_pct']:.1f}%" for c in r["category_shares"][:3])
        lines.append(f"| {r['program']} | {r['capture_count']} | {', '.join(r['end_boundaries'])} | {r['instructions']:,} | {r['instruction_cycles']:,} | {dominant} | {r['fp_kernel_10x_what_if']['hypothetical_overall_speedup']:.2f}x |")
    lines += ["", "The final column is an Amdahl what-if, not an implementation result: make only the selected add/subtract, multiply, and divide instruction ranges 10x faster, leave every other cycle unchanged, and add no transfer overhead.", ""]
    if failed_captures:
        lines += ["## Failed captures excluded from priorities", "", "| Program | Capture | Reported error | Profile cycles |", "|---|---|---|---:|"]
        for r in failed_captures:
            lines.append(f"| {r['program']} | {r['runid']} | {'; '.join(r['basic_error_lines'])} | {r['instruction_cycles']:,} |")
        lines.append("")
    lines += ["## Capture boundaries", "", "| Capture | Profile end | Snapshot lag in guest cycles | Boundary note |", "|---|---|---:|---|"]
    for r in captures:
        note = (r["boundary_note"] or "RUN entry through BASIC prompt").replace("|", "/")
        lines.append(f"| {r['runid']} | {r['end_boundary']} | {r['snapshot_after_profile_cycles']} | {note} |")
    lines.append("")
    for r in programs:
        lines += [f"## {r['program']}", "", "| Exclusive category | Cycles | Share |", "|---|---:|---:|"]
        for c in r["category_shares"]:
            lines.append(f"| {c['category']} | {c['cycles']:,} | {c['cycle_share_pct']:.2f}% |")
        lines += ["", "Selected entry-PC visits (not inclusive call costs):", "", "| Group | Entry | Visits |", "|---|---|---:|"]
        important = {"FADDT", "FMULTT", "FDIVT", "INT", "SIN", "COS", "RND", "PTRGET", "SCALAR_LOOKUP_LOOP_START", "FL1", "ARRAY", "MULTIPLY_SUBS_1", "FIN", "FOUT", "FOUT_1", "GARBAG", "CAT", "RAM_CHRGET", "FRMEVL", "NEXT", "ISCNTC", "OUTDO"}
        for e in r["entry_instruction_visits"]:
            if e["entry"] in important:
                lines.append(f"| {e['group']} | {e['entry']} | {e['instruction_visits']:,} |")
        lines += ["", "Narrow target regions (exclusive body cycles, excluding callees):", "", "| Region | Cycles | Share |", "|---|---:|---:|"]
        for region in r["target_regions"]:
            lines.append(f"| {region['region']} | {region['cycles']:,} | {region['cycle_share_pct']:.2f}% |")
        lines += ["", "Top 20 PCs by exclusive cycles:", "", "| PC | Cycles | Share | Category | Nearest named entry |", "|---|---:|---:|---|---|"]
        for p in r["top20_pcs"]:
            label = p["nearest_named_entry"]
            if label is not None:
                label += f"+{p['offset_from_named_entry']:X}"
            else:
                label = p["interval_label"]
            lines.append(f"| {p['pc_hex']} | {p['cycles']:,} | {p['cycle_share_pct']:.2f}% | {p['category']} | {label} |")
        lines.append("")
    lines += ["## Limits", ""] + [f"- {x}" for x in limitations] + [""]
    if skipped:
        lines += ["## Skipped captures", ""] + [f"- {r['runid']}: {r['reason']}" for r in skipped] + [""]
    lines += ["Regenerate by running `python rollup_profiles.py` from the study folder, or pass `--study-root PATH`. Existing captures are read only; outputs are replaced in `analysis/`.", ""]
    (out / "PROFILES.md").write_text("\n".join(lines), encoding="utf-8")
    print(f"Summarized {len(captures)} captures / {len(programs)} programs; skipped {len(skipped)}")
    for r in programs:
        print(r["program"], f"{r['instruction_cycles']:,} cycles", ", ".join(f"{c['category']} {c['cycle_share_pct']:.1f}%" for c in r["category_shares"][:4]))
    print(out / "PROFILES.md")


if __name__ == "__main__":
    main()
