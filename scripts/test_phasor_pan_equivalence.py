#!/usr/bin/env python3
"""Compare production PSG and SSI pan arithmetic with the original RTL."""
from pathlib import Path
import re

from test_w65c02_core import run, vivado_tool

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "build" / "phasor_pan_equivalence"
REFERENCE = ROOT / "scripts/fixtures/ssi263_native/pan_reference.svh"


def extract_function(source: str, name: str) -> str:
    match = re.search(rf"function automatic[^;]*?\b{name}\([^;]*;.*?endfunction", source, re.S)
    if match is None:
        raise ValueError(f"Missing {name}")
    return match.group()


def main() -> None:
    OUT.mkdir(parents=True, exist_ok=True)
    functions = [REFERENCE.read_text()]
    for file, name in (("mockingboard.sv", "apply_pan_gain"),
                       ("ssi263_stereo_mixer.sv", "pan_coefficient")):
        path = f"hdl/apple/{file}"
        functions.append(extract_function((ROOT / path).read_text(), name))

    bench = OUT / "tb_phasor_pan_equivalence.sv"
    bench.write_text("module tb_phasor_pan_equivalence;\n" + "\n".join(functions) + """
    initial begin
        for (int code = 0; code < 32; code++) begin
            for (int sample = 0; sample < 256; sample++) begin
                if (apply_pan_gain(sample[7:0], code[4:0]) !==
                    apply_pan_gain_reference(sample[7:0], code[4:0]))
                    $fatal(1, "PSG pan mismatch sample=%0d gain=%0d", sample, code);
            end
            for (int gain = 0; gain < 65536; gain++) begin
                if (pan_coefficient(gain[15:0], code[4:0]) !==
                    pan_coefficient_reference(gain[15:0], code[4:0]))
                    $fatal(1, "SSI pan mismatch gain=%0d balance=%0d", gain, code);
            end
        end
        $display("PHASOR PAN EQUIVALENCE PASS");
        $finish;
    end
endmodule
""")
    run([vivado_tool("xvlog"), "--sv", str(bench)], OUT, OUT / "xvlog.log")
    run([vivado_tool("xelab"), "tb_phasor_pan_equivalence", "-s", "phasor_pan_equivalence"],
        OUT, OUT / "xelab.log")
    output = run([vivado_tool("xsim"), "phasor_pan_equivalence", "--runall"],
                 OUT, OUT / "xsim.log")
    if "PHASOR PAN EQUIVALENCE PASS" not in output or "Fatal:" in output:
        raise RuntimeError(output)
    print("Phasor pan equivalence passed (8,192 PSG and 2,097,152 SSI cases)")


if __name__ == "__main__":
    main()
