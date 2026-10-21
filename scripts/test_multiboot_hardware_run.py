#!/usr/bin/env python3
"""Check explicit full-run hardware selection with the real timing auditor."""

import copy
import importlib.util
import json
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
spec = importlib.util.spec_from_file_location("multiboot_builder", ROOT / "scripts/build_multiboot_arm.py")
builder = importlib.util.module_from_spec(spec)
spec.loader.exec_module(builder)
BASELINE = ROOT / "build/firmware-1.2.6-d1"


def need(condition, message):
    if not condition:
        raise AssertionError(message)


def main():
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="test-hardware-run-", dir=ROOT / "build") as temporary:
        root = Path(temporary)
        run = root / ".timing_runs/full-fixture"
        run.mkdir(parents=True)
        platform = root / "platform/export/platform/platform.xpfm"
        platform.parent.mkdir(parents=True)
        platform.write_text("fixture platform")
        platform_xsa = platform.parent / "hw/appletini_yarz_top.xsa"
        platform_xsa.parent.mkdir()
        fsbl = platform.parent / "sw/boot/fsbl.elf"
        fsbl.parent.mkdir(parents=True)
        fsbl.write_bytes(b"fresh matching platform FSBL")
        bit = run / "appletini_yarz_top.bit"
        bit.write_bytes(b"new hardware fixture")
        dcp = run / "candidate.dcp"
        dcp.write_bytes(b"fresh full implementation fixture")
        xsa = run / "appletini_yarz_top.xsa"
        with zipfile.ZipFile(xsa, "w") as archive:
            archive.writestr(bit.name, bit.read_bytes())
        platform_xsa.write_bytes(xsa.read_bytes())
        (root / "LICENSE").write_text("fixture license")
        hashes = {}
        members = {}
        for name, data in {"LICENSE": b"fixture license", "hdl/clock.sv": b"module clock; endmodule\n",
                           "scripts/build.py": b"# frozen build script\n"}.items():
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
            hashes[name] = builder.digest(path)
            members[name] = path
        records = builder.archive_sources(run / "hardware-sources.zip", members)
        builder.write_json(run / "hardware-source-manifest.json", {"files": records})
        builder.write_json(run / "source-before.json", {"files": hashes})
        builder.write_json(run / "source-after.json", {"files": hashes})
        timing = {}
        for line in (BASELINE / "TIMING_MANIFEST.txt").read_text().splitlines():
            if "=" in line:
                name, value = line.split("=", 1)
                timing[name] = value
        timing.update(run_dir=run.relative_to(root).as_posix(), build_id=run.name, git_dirty="1",
                      bitstream_sha256=builder.digest(bit), xsa_sha256=builder.digest(xsa),
                      candidate_dcp_sha256=builder.digest(dcp))
        manifest = run / "TIMING_MANIFEST.txt"

        def write_timing(values):
            manifest.write_bytes("".join(f"{name}={value}\n" for name, value in values.items()).encode("utf-8"))

        write_timing(timing)
        env = {"APP_MULTIBOOT_HARDWARE_RUN": str(run), "APP_MULTIBOOT_FSBL": str(fsbl)}

        def select(values=None, rebuild=True):
            return builder.new_hardware_inputs(platform, BASELINE, rebuild,
                                               env if values is None else values, root)

        def rejected(action, label):
            try:
                action()
            except (RuntimeError, ValueError, FileNotFoundError, KeyError, zipfile.BadZipFile):
                return
            raise AssertionError(f"accepted invalid {label}")

        need(select({}) is None, "default retained mode changed")
        rejected(lambda: select({"APP_MULTIBOOT_FSBL": str(fsbl)}), "FSBL override in retained mode")
        result = select()
        need(result["timing"]["git_dirty"] == "1", "dirty full-run provenance was changed")
        need(result["hashes"] == hashes and result["records"]["fsbl"]["sha256"] == builder.digest(fsbl),
             "selected hardware evidence differs")
        write_timing(dict(timing, implementation_setup_margin_ns="0.400"))
        need(select()["timing"]["implementation_setup_margin_ns"] == "0.400",
             "real auditor must accept a selected 0.400 ns implementation margin")
        write_timing(timing)
        print("PASS retained default and explicit dirty full-run selection")

        rejected(lambda: select(rebuild=False), "retained CPU1 for new hardware")
        rejected(lambda: select({"APP_MULTIBOOT_HARDWARE_RUN": str(run)}), "missing fresh FSBL")
        for name in ("APP_MULTIBOOT_XSA", "APP_MULTIBOOT_BITSTREAM"):
            rejected(lambda: select(dict(env, **{name: str(bit)})), "conflicting arbitrary hardware override")
        rejected(lambda: select(dict(env, APP_MULTIBOOT_HARDWARE_RUN=str(root))), "non-run directory")
        print("PASS explicit input and both-core rebuild guards")

        for name, value in (("wns_ns", "0.049"), ("minimum_wns_ns", "0.049"),
                            ("build_mode", "incremental"), ("synthesis_reused", "1"),
                            ("wns_ns", "NaN"), ("whs_ns", "-0.001"),
                            ("unconstrained_internal_endpoints", "1"),
                            ("missing_constraint_objects", "1"), ("route_status", "FAIL"),
                            ("implementation_setup_margin_ns", "0.199"),
                            ("final_phi0_release_limit_ns", "9"),
                            ("run_dir", ".timing_runs/other")):
            broken = dict(timing, **{name: value})
            write_timing(broken)
            rejected(select, f"timing {name}={value}")
        write_timing(timing)
        print("PASS real auditor timing, constraint, freshness, and 50 ps gates")

        for path in (bit, dcp, xsa, platform_xsa, fsbl):
            original = path.read_bytes()
            path.write_bytes(original + b"tamper")
            if path == fsbl:
                # Explicit input may be a separate copied ELF; it must still
                # match the FSBL from the selected exported platform.
                copied_fsbl = root / "other.elf"
                copied_fsbl.write_bytes(original)
                rejected(lambda: select(dict(env, APP_MULTIBOOT_FSBL=str(copied_fsbl))), "platform FSBL mismatch")
            else:
                rejected(select, f"artifact mismatch {path.name}")
            path.write_bytes(original)
        print("PASS bitstream, DCP, XSA, exported platform, and FSBL identity")

        changed = copy.deepcopy(hashes)
        changed["hdl/clock.sv"] = "0" * 64
        builder.write_json(run / "source-after.json", {"files": changed})
        rejected(select, "source change during FPGA build")
        builder.write_json(run / "source-after.json", {"files": hashes})
        with zipfile.ZipFile(run / "hardware-sources.zip", "a") as archive:
            archive.writestr("hdl/injected.sv", b"unexpected")
        rejected(select, "source archive injection")
        builder.archive_sources(run / "hardware-sources.zip", members)
        need(select()["hashes"] == hashes, "restored inputs must pass")
        print("PASS frozen before/after source and exact archive membership")

        # Canonical incremental implementation still synthesizes current RTL.
        # It must identify a qualified full-run DCP and demonstrate actual reuse.
        (run / "manifest.txt").write_bytes(manifest.read_bytes())
        child = root / ".timing_runs/incremental-fixture"
        child.mkdir()
        for name in ("appletini_yarz_top.bit", "appletini_yarz_top.xsa", "candidate.dcp",
                     "source-before.json", "source-after.json", "hardware-sources.zip",
                     "hardware-source-manifest.json"):
            (child / name).write_bytes((run / name).read_bytes())
        child_manifest = child / "TIMING_MANIFEST.txt"
        child_timing = dict(timing, build_mode="incremental", build_id=child.name,
                            run_dir=child.relative_to(root).as_posix(), incremental_reference=str(dcp),
                            incremental_reference_sha256=builder.digest(dcp))

        def write_child(values):
            child_manifest.write_bytes("".join(f"{k}={v}\n" for k, v in values.items()).encode())

        write_child(child_timing)
        # These columns and DCP location are Vivado 2025.2's real table format.
        reuse = ("Incremental Implementation Information\n"
                 "| Type | Matched % (of Total) | Current Reuse % (of Total) | Fixed % (of Total) | Total |\n"
                 "| Cells | 99.92 | 99.90 | 0.57 | 61933 |\n"
                 "| Nets | 99.81 | 99.50 | 0.00 | 45058 |\n"
                 f"| DCP Location: | {dcp.as_posix()} |\n")
        log = child / "implementation.log"
        report = child / "implementation-reuse.rpt"
        log.write_text(f"Command: read_checkpoint -incremental {dcp.as_posix()}\n" + reuse)
        report.write_text(reuse)
        proof = {"schema_version": 1, "reference_run": str(run),
                 "reference_manifest_sha256": builder.digest(run / "manifest.txt"),
                 "implementation_log_sha256": builder.digest(log), "reuse_report_sha256": builder.digest(report)}
        builder.write_json(child / "incremental-proof.json", proof)
        child_env = dict(env, APP_MULTIBOOT_HARDWARE_RUN=str(child))

        def select_child():
            return select(child_env)

        selected = select_child()
        need(selected["timing"]["build_mode"] == "incremental"
             and selected["timing"]["synthesis_reused"] == "0"
             and selected["incremental"]["final_reuse"]["cells"]["reused_percent"] == 99.90,
             "actual incremental mode, fresh synthesis and reuse must be preserved")
        reuse_initial = reuse.replace("| Current Reuse %", "| Initial Reuse % (of Initial Total) | Current Reuse %")
        reuse_initial = reuse_initial.replace("| 99.92 |", "| 99.92 | 99.90 |").replace("| 99.81 |", "| 99.81 | 99.50 |")
        need(builder.parse_incremental_reuse(reuse_initial, dcp)["nets"]["reused_percent"] == 99.50,
             "initial/current reuse column form must parse correctly")
        print("PASS qualified full-run reference, actual incremental mode, fresh synthesis, both reuse table formats")

        for name, value in (("synthesis_reused", "1"), ("wns_ns", "0.049"),
                            ("minimum_wns_ns", "0.049"), ("hold_failing_endpoints", "1"),
                            ("unconstrained_internal_endpoints", "1"), ("route_status", "FAIL"),
                            ("incremental_reference_sha256", "0" * 64),
                            ("incremental_reference", str(child / "candidate.dcp"))):
            write_child(dict(child_timing, **{name: value}))
            rejected(select_child, f"incremental {name}")
        write_child(child_timing)
        for path in (run / "manifest.txt", dcp, bit, xsa, root / "hdl/clock.sv", report, log):
            original = path.read_bytes()
            path.write_bytes(original + b"tamper")
            rejected(select_child, f"incremental identity/source tamper {path.name}")
            path.write_bytes(original)
        print("PASS incremental rejects stale synthesis, weak signoff, reference/evidence tampering, and source drift")

        original_log = log.read_bytes()
        original_report = report.read_bytes()
        for changed_log, changed_report in (
                (original_log.decode() + "INFO: Incremental flow is disabled. No incremental reuse Info to report.\n", reuse),
                (original_log.decode() + "INFO: Falling back to full implementation\n", reuse),
                (reuse, reuse),
                (original_log.decode(), reuse.replace("99.90", "0.00")),
                (original_log.decode(), reuse.replace(dcp.as_posix(), (child / "candidate.dcp").as_posix()))):
            log.write_text(changed_log)
            report.write_text(changed_report)
            builder.write_json(child / "incremental-proof.json", dict(proof,
                implementation_log_sha256=builder.digest(log), reuse_report_sha256=builder.digest(report)))
            rejected(select_child, "fallback, missing command, zero reuse or wrong report reference")
        log.write_bytes(original_log)
        report.write_bytes(original_report)
        builder.write_json(child / "incremental-proof.json", proof)
        need(select_child()["incremental"]["source_snapshot_current_matches"], "restored incremental proof must pass")
        print("PASS actual reuse proof rejects full fallback, missing checkpoint read, zero reuse, and wrong DCP location")
    print("PASS all explicit hardware-run checks")


if __name__ == "__main__":
    main()
