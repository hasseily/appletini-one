#!/usr/bin/env python3
"""Compatibility runner for the restored Blur/Dot NEON regression."""
import sys
from test_video_smoothing import main
if __name__ == "__main__":
    if "--neon" not in sys.argv: sys.argv.append("--neon")
    main()
