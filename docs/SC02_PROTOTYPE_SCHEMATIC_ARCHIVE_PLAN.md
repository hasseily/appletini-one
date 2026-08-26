# SC-02 Prototype Schematic Archive Plan

## Goal

Preserve the seven-sheet SC-02 prototype schematic in open, text-based forms
that tools and people can inspect without repeatedly decoding the Altium smart
PDF.  Keep the original PDF as the fixed source record.

This is an archive task.  It does not resolve differences between the
prototype and the production SSI-263, and it does not change the speech RTL.

## Source of truth

- Preserve the supplied PDF byte for byte and record its SHA-256.
- Take component, pin, net, and net-label membership from the PDF bookmark
  tree.  Do not infer connections from line crossings.
- Take page, component, pin, and net-label anchors from the PDF's positioned
  `CO`, `PI`, and `NL` tags.
- Keep a text-based SVG copy of each drawn sheet so no symbol or note depends
  only on a PDF renderer.

## Outputs

- Canonical, deterministic JSON with source data, anchors, evidence, counts,
  and validation results.
- A flat CSV with one row per sheet pin/net membership.
- Seven source SVG files that retain the drawn schematic and searchable text.
- Graphviz DOT and SVG connection graphs for each sheet and the whole design.
- A KiCad 10 project with an index and seven child sheets.  It uses generated
  generic symbols and exact pin/net labels; it does not claim to restore the
  lost Altium libraries or route geometry.
- A SHA-256 manifest and automated archive tests.

## Work and checkpoints

1. Archive the source PDF and verify its hash.
2. Extract and validate all structured data exposed by the smart PDF.
3. Generate CSV, SVG, Graphviz, and KiCad views from the extracted JSON.
4. Check exact tuple counts, paths, hashes, XML/S-expression syntax, and
   repeat-build byte identity.
5. Parse and export the generated project with a real KiCad 10 command-line
   build when that tool is available.
6. Document what the automated conversion can and cannot recover.
7. Commit the source, tools, generated archive, tests, and results at logical
   checkpoints, then leave this branch intact for later work.

## Acceptance rules

- The archive must contain seven sheets, 785 component-unit entries, 3,213
  explicit component-pin entries, 1,257 sheet net entries, and 3,421 unique
  connected pin IDs, including 208 implicit package pins.
- No connected pin ID may map to more than one exact net name.
- Every component and explicit pin must have a source coordinate anchor.
- Regeneration must not write the extractor host's checkout or input path into
  the archive.  It must keep historical paths that are visible text on the
  source sheets.
- Generated connectivity must equal the canonical JSON; graphics must never
  add a connection based only on geometry.

## Known limits

The PDF does not contain the original `.SchDoc`, Altium libraries, PCB data,
simulation models, design rules, fitted-part database, or a reliable record of
production-board changes.  Automated output therefore cannot restore those
items.  The copied PDF and source SVG sheets remain the visual record; the JSON
and CSV remain the exact extracted connection record.

## Result

Completed on 2026-08-26.  All seven steps above passed.  The final archive has
39 hashed files plus its manifest.  KiCad 10.0.5 parsed the generated symbol
library and full hierarchy, then exported 785 components, 1,088 named nets,
and 3,519 pin nodes with no unconnected nodes.  The archive test also rebuilt
the Python and pinned Graphviz outputs and required byte-for-byte matches.  Its
KiCad check binds the saved real-tool result to the exact JSON and project
hashes.
