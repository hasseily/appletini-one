from pathlib import Path
import csv, hashlib, json, shutil, subprocess
repo = Path(r"C:\Users\hasse\source\repos\gssquared")
out = Path(__file__).resolve().parent
def sha(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def check_fixture(name, insns, cycles, irq_events, irq_cycles):
    data = json.loads((out / f"profiler-test/{name}.json").read_text())
    rows = list(csv.DictReader((out / f"profiler-test/{name}.csv").open()))
    assert data["instructions"] == insns
    assert data["instruction_cycles"] == cycles
    assert data["irq_events"] == irq_events
    assert data["irq_cycles"] == irq_cycles
    assert data["guest_cycle_span"] == cycles + irq_cycles
    assert sum(int(r["instructions"]) for r in rows) == insns
    assert sum(int(r["cycles"]) for r in rows) == cycles
check_fixture("budget", 3, 9, 1, 7)
check_fixture("stopped", 1, 5, 0, 0)
ack = json.loads((out / "profiler-test/ack.json").read_text())
assert ack["seq"] == 3 and ack["state"] == "complete"
subprocess.run(["git", "diff", "--check"], cwd=repo, check=True)
shutil.copy2(repo / "build/GSSquared.exe", out / "GSSquared-profile.exe")
original = json.loads((out / "original-sha256.json").read_text())
restored = {}
for rel in ("src/cpus/base_6502.cpp", "src/gs2.cpp"):
    shutil.copy2(out / "original" / rel, repo / rel)
    restored[rel] = sha(repo / rel)
    assert restored[rel] == original[rel]
    (repo / rel).touch()  # Ensure the next ordinary build removes instrumentation.
result = {
    "profiler_tests": "passed: exact instruction budget, IRQ separation, per-PC sums, host stop, consecutive captures",
    "source_revision": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip(),
    "instrumented_executable": str(out / "GSSquared-profile.exe"),
    "instrumented_executable_sha256": sha(out / "GSSquared-profile.exe"),
    "build_executable_sha256": sha(repo / "build/GSSquared.exe"),
    "original_executable_sha256": original["build/GSSquared.exe"],
    "restored_source_sha256": restored,
    "instrumentation_header_sha256": sha(out / "GuestProfiler.hpp"),
}
assert result["instrumented_executable_sha256"] == result["build_executable_sha256"]
(out / "build-validation.json").write_text(json.dumps(result, indent=2) + "\n")
print(json.dumps(result, indent=2))
print(subprocess.check_output(["git", "status", "--short"], cwd=repo, text=True))
