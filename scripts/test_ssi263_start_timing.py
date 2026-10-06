#!/usr/bin/env python3
"""Compatibility entry point for the native SSI bus/start/reset regression.

The former tb_ssi263_start_timing bench asserted private SC-01/formant backend
signals. The native integration regression now covers actual Apple writes,
response/start ordering, all register aliases, AP/P resets and audio_valid.
"""
from test_ssi263_native_integration import main

if __name__ == "__main__":
    main()
    print("SSI263 START TIMING PASS (native integration)")
