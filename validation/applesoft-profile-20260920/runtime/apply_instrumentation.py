from pathlib import Path
import hashlib, json, shutil, subprocess
repo = Path(r"C:\Users\hasse\source\repos\gssquared")
out = Path(__file__).resolve().parent
backup = out / "original"
backup.mkdir(exist_ok=True)
files = ["src/cpus/base_6502.cpp", "src/gs2.cpp"]
manifest = {}
for rel in files:
    src = repo / rel
    dest = backup / rel
    if dest.exists():
        raise SystemExit(f"Refusing to overwrite existing source backup {dest}")
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest)
    manifest[rel] = hashlib.sha256(src.read_bytes()).hexdigest()
exe = repo / "build/GSSquared.exe"
shutil.copy2(exe, backup / "GSSquared.exe")
manifest["build/GSSquared.exe"] = hashlib.sha256(exe.read_bytes()).hexdigest()
(out / "original-sha256.json").write_text(json.dumps(manifest, indent=2) + "\n")
header = (out / "GuestProfiler.hpp").as_posix()
for rel in files:
    p = repo / rel
    data = p.read_bytes()
    newline = b"\r\n" if b"\r\n" in data else b"\n"
    text = data.decode().replace("\r\n", "\n")
    needle = '#include "cpu.hpp"\n'
    assert text.count(needle) == 1
    text = text.replace(needle, needle + f'#include "{header}"\n', 1)
    if rel.endswith("base_6502.cpp"):
        needle = "    system_trace_entry_t *tb = &cpu->trace_entry;\n"
        assert text.count(needle) == 1
        text = text.replace(needle,
            "    const uint16_t profile_pc = cpu->pc;\n"
            "    const uint64_t profile_cycle = clock->get_cycles();\n" + needle)
        needle = "        TRACE(if (cpu->trace) cpu->trace_buffer->add_entry(cpu->trace_entry);)\n        return 0;\n"
        assert text.count(needle) == 1
        text = text.replace(needle,
            "        gs2guest::profiler.record(profile_pc, profile_cycle, clock->get_cycles(), true);\n" + needle)
        needle = "    TRACE(if (cpu->trace) cpu->trace_buffer->add_entry(cpu->trace_entry);)\n\n    return 0;"
        assert text.count(needle) == 1
        text = text.replace(needle,
            "    gs2guest::profiler.record(profile_pc, profile_cycle, clock->get_cycles(), false);\n" + needle)
    else:
        needle = "bool run_one_frame(computer_t *computer) {\n    cpu_state *cpu = computer->cpu;\n    NClock *clock = computer->clock;\n"
        assert text.count(needle) == 1
        text = text.replace(needle, needle +
            "    gs2guest::profiler.poll_control(clock->get_cycles(), (int)clock->get_clock_mode());\n")
    p.write_bytes(text.replace("\n", newline.decode()).encode())
diff = subprocess.check_output(["git", "diff", "--", *files], cwd=repo)
(out / "gssquared-instrumentation.patch").write_bytes(diff)
print(json.dumps(manifest, indent=2))

