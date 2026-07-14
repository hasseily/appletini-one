#!/usr/bin/env python3
"""Source-level regression test for the PAL accurate video timing model.

This is intentionally lightweight: the executable firmware build is still the
compiler gate, while this test locks in the PAL model's source-of-truth timing
constants, delayed soft-switch equations, renderer dispatch, and Vitis source
registration.

    python scripts/test_pal_video_timing_model.py
"""

from __future__ import annotations

from test_video_output_config_menu import (
    TestFailure,
    test_pal_accurate_renderer_model_is_registered,
)


def main() -> int:
    try:
        test_pal_accurate_renderer_model_is_registered()
    except TestFailure as exc:
        print(f"FAIL test_pal_accurate_renderer_model_is_registered: {exc}")
        return 1

    print("PASS test_pal_accurate_renderer_model_is_registered")
    print("1 PAL video timing test passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
