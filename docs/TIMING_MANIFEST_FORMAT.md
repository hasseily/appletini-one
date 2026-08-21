# Timing manifest format

Timing-run manifests use a strict line format so the Tcl signoff tools and the
Python firmware packager accept the same files.

- Files contain UTF-8 text with LF line endings. CR and NUL bytes are invalid.
- A data line is `key=value`. The first `=` separates the fields; later `=`
  characters belong to the value.
- Keys match `[A-Za-z][A-Za-z0-9_.-]*`. Empty, spaced, and duplicate keys are
  invalid.
- Values may be empty and may contain spaces, but not CR, LF, or NUL.
- Empty lines and lines whose first byte is `#` are ignored. Indented comments
  are invalid.
- Writers sort keys and end every data line with LF.

The shared fixtures in `scripts/testdata/timing_manifest` and
`scripts/test_timing_manifest_format.py` check both implementations.
