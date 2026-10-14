# FIFTEEN PUZZLE startup failure

The supplied disk program has stale absolute text-pointer offsets. Its first statement redirects Applesoft to `$0869`, the digit `1` inside `Z=1`, instead of the statement separator at `$0866`. This explains the captured `?SYNTAX ERROR IN 0` before the first keyboard input. The two other destinations derived from its line-1 POKE are also three bytes too high.

This is a diagnosis of the original disk bytes and the existing emulator capture. No disk, source, or emulator state was changed for this diagnosis, and no proposed correction was executed.

## Evidence and address basis

- Original disk SHA-256: `a886567d9400af183cf85dbfca84a95e5ec25ad18d2e4036ce725c95a694fbe6`.
- Extracted `FIFTEEN_PUZZLE.tokens.bin`: 387 bytes, SHA-256 `125662b7866345ea48df0240a777d49e43e5a906d34f53c19adc966d7a31b889`.
- `../captures/FIFTEEN_PUZZLE_STARTUP.run.json` records the same tokenized SHA-256, so the inspected payload matches the run.
- This Applesoft program is based at `$0801`. The first four payload bytes are `B3 08 00 00`: the next line starts at `$08B3`, and the current line number is 0. Its body starts at `$0805`. Line 1's link is `$0981`, followed there by the end-of-program zero link. These line links and the tokenized extraction are structurally consistent; there is no evidence here of a truncated file.
- The capture reports `basic_line: 0`, `text_pointer: 2153` (`$0869`), and screen text `?SYNTAX ERROR IN 0`.

`POKE 184,...` writes `$B8`, the low byte of Applesoft's text pointer. The high byte is `$08` at these branches. These tricks need to point at the colon immediately before the intended statement; they replace the text pointer while a statement is completing.

## Startup branch

The first statement is:

```basic
0 POKE 184,21+(Z=0)*84: ... :RETURN:Z=1:HOME:DIM T(16): ...
```

RUN initializes Z to zero, so the expression produces `21 + 84 = 105 = $69`. Applesoft therefore resumes with text pointer `$0869`.

The relevant bytes in the actual payload are:

```text
$0864  3A       :
$0865  B1       RETURN
$0866  3A       :          <-- required startup destination
$0867  5A       Z
$0868  D0       =
$0869  31       1          <-- actual destination; capture stops here
$086A  3A       :
$086B  97       HOME
$086C  3A       :
$086D  86       DIM
```

The ordinary drawing-subroutine destination, `21 = $15`, is correct: `$0815` contains a colon before the POKE 50 statement. The initialization destination must be `102 = $66`, so the difference from 21 must be **81**, not 84. Changing the address difference alone would give `POKE 184,21+(Z=0)*81` for the current bytes. This is a source-derived correction, not a tested repair.

## Line-1 branches

The second pointer-changing statement is:

```basic
1 FOR T=0 TO 14:POKE 184,216+(T(T)<>T+1)*4:NEXT:END:GET A$: ...
```

Its surrounding bytes are:

```text
$08D5  3A       :          <-- required destination if current tile is correct
$08D6  82       NEXT
$08D7  3A       :
$08D8  80       END        <-- actual false-condition destination, 216
$08D9  3A       :          <-- required destination if current tile is wrong
$08DA  BE       GET
$08DB  41       A
$08DC  24       $          <-- actual true-condition destination, 220
$08DD  3A       :
$08DE  41       A
```

The intended false path is the colon before NEXT, `$D5 = 213`. The intended true path is the colon before GET, `$D9 = 217`. Their four-byte separation is correct; the common base is three too large. For the current payload the expression would need to be `213+(T(T)<>T+1)*4`, not `216+(T(T)<>T+1)*4`.

## What this establishes

All three destinations after the drawing helper are three bytes above the separators they must reach, while the initial destination before that helper is correct. This is consistent with a three-byte shortening of the helper after the constants were calculated, or constants copied from a version whose helper was three bytes longer. The original source history is not in the disk, so it does **not** establish which bytes were removed or who introduced the mismatch. There is no need to assume a faulty 65C02 instruction or damaged DOS load: the disk payload itself encodes the wrong destinations, and the runtime capture reaches exactly the first wrong address.

The proposed numeric changes, `84` to `81` and `216` to `213`, keep their tokenized lengths unchanged. They are the minimal address corrections inferred from the bytes, but remain untested. The supplied program should be classified as a **pre-existing startup syntax error**, excluded from normal completed-workload comparisons. Any future repaired variant should use a separate filename and payload hash, keeping the supplied image intact.
