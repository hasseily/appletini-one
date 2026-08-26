# SC-02 Prototype Schematic Archive

This folder preserves the seven-sheet SC-02 prototype schematic in open,
text-based forms.  The supplied Altium smart PDF stays here as the fixed source
record.

The conversion is automated.  It never decides that crossing lines connect.
It takes component, pin, net, and label membership from the PDF bookmark tree.

## Source record

- File: `sc-02_Final_Schematic_V1.00.pdf`
- Size: 5,080,426 bytes
- SHA-256:
  `d0e05ea01fc5e823571e140fce5ce9f12a48f1993484d683f075151382adba35`
- PDF pages: 7
- Original sheet names: `analog_1.SchDoc`, `analog_2.SchDoc`, and
  `digital_1.SchDoc` through `digital_5.SchDoc`

## Which file to use

- Use `sc02_schematic.json` for exact extracted data and source evidence.
- Use `connectivity/sc02_pin_net_memberships.csv` for a flat pin/net table.
- Use `source_svg/` for the full drawn sheets in plain XML, with searchable
  text and embedded images.
- Open `kicad/sc02_prototype.kicad_pro` for a KiCad 10 connection view.
- Use `connectivity/*.dot` or `connectivity_svg/*.svg` for component/net
  graphs.
- Use the copied PDF when the exact source rendering matters.

## What the archive contains

The canonical JSON records:

- 785 component-unit entries and 484 physical component references;
- 3,213 explicit component pins;
- 208 unique implicit package pins and all 306 of their sheet memberships;
- 1,257 sheet net entries and 1,088 distinct exact net names;
- 3,519 sheet pin/net memberships over 3,421 unique connected pin IDs;
- 718 net-label occurrences over 383 labeled sheet nets;
- 15,330 styled page-text spans;
- every component, explicit-pin, and hidden net-label tag anchor; and
- a SHA-256 over the sorted connection tuples:
  `b13d422a2b0338a3080a5e16b092374ad5afddbb8699cf532b3d418d7cdd208f`.

The PDF exposes one hidden net-label tag per labeled sheet net, not one per
drawn label.  The archive therefore keeps all 718 label occurrences, the 383
hidden tag anchors, every exact visible-text candidate, and the full styled
page text as separate facts.  Strict matching resolves 209 label positions;
509 stay marked unresolved.  The generator does not guess their positions.

## KiCad project

The KiCad files form a valid KiCad 10 project with one index sheet and seven
child sheets.  Each source component unit gets a generated generic symbol.
Each pin gets its exact source ID, pin number, net name, and implicit-pin flag
as symbol fields.  Symbol and pin placement keeps the source anchors.  A global
label sits on each pin, which gives KiCad the exact extracted net list without
drawn-wire guesses.

KiCad 10.0.5 checked a disposable copy of the project and its symbol library.
The exported XML net list contains exactly 785 components, 1,088 nets, and
3,519 nodes, with no unconnected nodes.  It also exported the index and all
seven child sheets as SVG.  `kicad_validation.json` binds that result to the
SHA-256 of the canonical JSON and a SHA-256 over all 11 KiCad files.  The
archive test checks both links, so a changed model or project cannot keep a
stale pass result.

KiCad reports two expected ERC warning classes:

- 785 off-grid warnings, because the generated symbols keep the exact PDF
  anchor positions; and
- 414 one-pin-net warnings, matching the 414 extracted nets that have one
  membership.

It also warns about annotation because 556 exact source unit names, such as
`U117A` and `C10B`, do not use KiCad's normal trailing-number form.  The archive
keeps those source names unchanged.  They are unique.

This is an electrical archive, not a recovered Altium design.  The generic
KiCad symbol bodies do not claim to restore the source symbol art, part types,
pin electrical types, or route shapes.  Use `source_svg/` for the drawn view.

## Rebuild

Run these commands from the repository root:

```powershell
python -m pip install -r scripts\requirements-sc02-schematic.txt
npm ci --prefix scripts\sc02_dot_renderer
python scripts\sc02_schematic_extract.py schematics\sc02_prototype\sc-02_Final_Schematic_V1.00.pdf --output schematics\sc02_prototype\sc02_schematic.json
python scripts\sc02_pdf_to_svg.py schematics\sc02_prototype\sc-02_Final_Schematic_V1.00.pdf --model schematics\sc02_prototype\sc02_schematic.json --output-dir schematics\sc02_prototype\source_svg
python scripts\sc02_connectivity_generate.py schematics\sc02_prototype\sc02_schematic.json --output-dir schematics\sc02_prototype\connectivity
python scripts\sc02_kicad_generate.py schematics\sc02_prototype\sc02_schematic.json --output-dir schematics\sc02_prototype\kicad
node scripts\sc02_dot_to_svg.mjs schematics\sc02_prototype\connectivity schematics\sc02_prototype\connectivity_svg
python scripts\sc02_archive_manifest.py schematics\sc02_prototype --output schematics\sc02_prototype\MANIFEST.sha256
```

The pinned packages are PyMuPDF 1.27.2.3 and `@viz-js/viz` 3.25.0, which embeds
Graphviz 14.1.3.  The DOT renderer loads that checked local package by default
and stops if its Graphviz version differs.  Another `dot -Tsvg` build can show
the DOT files, but it need not produce the same SVG bytes.

Run the archive checks with:

```powershell
python scripts\test_sc02_schematic_archive.py
python scripts\test_sc02_schematic_archive.py --regenerate
```

The second command rebuilds the JSON, source SVG, CSV/DOT, Graphviz SVG, and
KiCad files in a temporary folder and requires byte-for-byte matches.

## Limits

The smart PDF does not contain the original `.SchDoc`, Altium libraries, PCB
data, simulation models, design rules, fitted-part database, or a reliable list
of production-board changes.  It also does not turn nearby value, footprint,
or population text into typed part fields.  The archive keeps that text and the
drawn sheets, but it does not assign meaning that the PDF does not state in its
structured data.

The source sheets print historical `Q:\altium files\...` paths in their title
blocks.  The JSON and source SVG keep that visible source text on purpose.
They do not store the path of the checkout or the host that ran the extractor.

The archive cannot settle prototype-versus-production questions, such as the
final P2/R301 population, without another source.  It does remove the need to
decode the PDF again for the data that the PDF does expose.
