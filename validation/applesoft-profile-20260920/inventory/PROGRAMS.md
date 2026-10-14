# Disk inventory for Applesoft profiling

Source: `C:/Users/hasse/Downloads/short-programs.dsk`

SHA-256: `a886567d9400af183cf85dbfca84a95e5ec25ad18d2e4036ce725c95a694fbe6`

Format: 143,360-byte DOS-order DOS 3.3 image, 35 tracks, 16 sectors per track, volume 254. The catalog contains 29 Applesoft files and no other active files. `GEOMETRY` and `GEOMETRIC` have identical tokenized payloads: there are 28 distinct programs. The disk has not been changed. `inspect_disk.py` reads the source and creates the JSON inventory, exact tokenized payloads, and readable listings beside this file.

The following are source-derived workload descriptions, not timing results. Run commands take the form `RUN RANDOM PLASMA` at the DOS Applesoft prompt. Most graphics programs leave graphics enabled when they finish, so an invisible prompt is not proof they are still running. Measure completion at the BASIC input loop or another explicit CPU-state boundary.

## Best unattended full-program measurements

| File | Behavior and expected work | Profiling value |
|---|---|---|
| RANDOM PLASMA | Finite 280 by 192 image: 53,760 pixels, 107,520 SIN evaluations, 53,760 RND evaluations | Transcendental math, repeated literal parsing, scalar lookup, HCOLOR/HPLOT; repeats SIN(Y*.1) for every X |
| FS PLASMA | Finite 280 by 192 image: 53,760 pixels, 107,520 SIN evaluations; error diffusion uses A(2,281) | Math plus heavy 2D array indexing: ten array reads/writes per pixel; repeats row SIN and constants |
| COLOR PLASMA | Finite 140 by 192 pixel pairs: 26,880 SIN evaluations; error diffusion uses A(2,281) | 2D array indexing, comparisons, scalar lookup, parsing/statement dispatch, conditional HPLOT, REM handling |
| SIERPINSKI | Finite recursive triangle construction with D=4 | Arrays, recursion, HPLOT lines, float arithmetic; directly changes interpreter text pointer |
| GUMBALLS | Finite image, 20 columns by 13 balls = 260 balls, 1,820 HPLOT statements | Array access, RND, addition/multiplication, line rasterization |
| CITY SCAPE | Finite image with four layers of buildings | RND, multiplication, loop dispatch, horizontal lines; CALL 62454 fills the screen |
| FIZZBUZZ | Finite output for N=1 through 100 | FP division, INT/comparison, STR$, string array lookup, PRINT/scroll |
| SPRITE | Finite decoded sprite, 126 encoded pieces | MID$/ASC, temporary strings, arithmetic, HPLOT; CALL -3082 fills the screen |
| ISOMETRIC FACTORY | Finite scene; 11 first-pass blocks and six second-pass blocks | Array/scalar lookups, integer-variable conversion, arithmetic, long HPLOT segments |
| GEOMETRY | Finite, 11 orientations; four SIN and four COS evaluations per orientation | Array lookup, arithmetic, HPLOT polygons, SQR and power at setup |
| GEOMETRIC | Exact duplicate of GEOMETRY | Skip duplicate timing unless checking run repeatability |
| TRUCHET TILES | Finite 3-pixel tile pattern with random diagonal orientation | Tiny HPLOT operations, RND, loop/variable overhead |
| IDENTITY CRISIS | Finite run-length decoded image, 32 source rows drawn double height | MID$/ASC, temporary strings, integer conversion and division, HPLOT lines |
| SYS.DIAG | Finite fake diagnosis, five messages after banner | Strings, DEF FN, RND, 2,505 explicit delay-loop iterations; SPEED=160 deliberately adds output delays |
| HELLO | Finite title/instructions display | READ/string comparison, LEN, centering, output; boot startup, not a heavy benchmark |

`SYS.DIAG` restores SPEED=255 at its END, but it deliberately uses SPEED=160 during the test. Report its monitor delay cost separately; silently overriding SPEED would change the workload. The visible names and instructions in HELLO are disk contents and were treated as data.

## Inputs, interactive programs, and bounded phases

| File | Inputs and end behavior | Useful reproducible phase |
|---|---|---|
| TESSELLATOR | INPUT requests period, coefficient, threshold; then draws once and returns | Use `64,2,0` and measure from accepted input through completion. There are 4,096 base cells, four trig calls per cell, then repeated screen tiles. Smaller period shifts the share from math toward plotting; period must be positive and at most 192 to avoid off-screen Y values |
| FIREWORKS | Endless alternating bursts; Ctrl-C can interrupt Applesoft | First C=3 and C=7 pair, or one complete pair after setup. A pair has 284 rise iterations and 164 radial iterations, 164 SIN plus 164 COS calls, and 1,016 HPLOT statements. Stop before the next C=3 burst; exclude HGR2 setup for steady state |
| SNAKE GAME | Any key turns; left-arrow reverses turn direction. Collision pauses at GET; Esc there exits, other keys rerun | No-input startup through first collision, excluding post-collision delay and GET wait. Separately measure the FOR 0..1000 delay if desired |
| STACKER | Any key stops current horizontal sweep; repeats upward until width is lost or top reached; SPEED=245 in sweep | Startup through first complete right-and-left sweep without input; stop before repeated sweep. Capture real output delay separately. Input-dependent full games need a scripted key schedule |
| DRIVING | Any key initiates a jump; strobe clears when jump starts. Ends on collision | Fixed number of road updates after initialization, or no-input collision with the same initial random state. Strings scroll with MID$, concatenation, RND; speaker and keyboard soft-switch accesses occur |
| DESERT ISLAND | GET commands T=take, E=east, W=west, D=dig; ends on treasure | Full deterministic nine-command solution `TEEEEETWD`; inject one key at each GET. Measure non-wait work. Strings, concatenation, comparisons, arrays |
| TYPING GAME | Type the displayed letter before it crosses left edge; a miss ends the game; up to 20 successful rounds | No-input first round through SCORE output. The nested FOR delay is intentional and dominates a separate category |
| TOWER OF HANOI | Keys 1,2,3 select source/destination towers; eight disks; ends when all reach tower 3 | Initial eight-disk draw through first GET, then a fixed legal move 1 to 2. Full solution requires 255 moves, 510 keypresses. Useful arrays, validation comparisons, and graphics |
| TARGET PRACTICE | Repeated INPUT requests angle,power; progress requires hitting each of ten targets | Initial terrain render through first INPUT. A shot such as `45,20` gives a fixed bounded phase from accepted input to next INPUT on a fixed random terrain. Avoid counting waits as BASIC work |
| JUNK DRAWER | Endless GET-driven drawing editor: I=up, J=left, K=down, L=right; digits 0..7 set color | Initialization then fixed key sequence `1LLLLKKKK2JJJJIIII`, measured only while handling keys; HPLOT and arithmetic |
| CONCENTRATION | Keys A..T select cards (lowercase is mapped to uppercase); waits between choices; ends after ten pairs | Initial 20-card shuffle and board render through first GET, then a fixed pair A,B through next GET. Explicit FOR 0..999 delay after pair. Directly changes interpreter text pointer |
| FIFTEEN PUZZLE | I/J/K/L move blank up/left/down/right; exits when sorted; initial random adjacent swap | Initial setup and board through first GET, then legal keys L,K,J,I. Array access, STR$/MID$, output, direct interpreter-pointer changes |
| GR-KANOID | Paddle 0 controls bat; three lives, ends after third missed ball | Setup plus fixed number of ball updates with a fixed paddle position. PDL reads are part of emulated I/O cost and should be separated from interpreter cost |
| MIND THE GAPS | Paddle 0 controls scrolling; button 0 increases jump height; ends on landing in gap | Setup plus fixed number of updates with fixed paddle/button state. If paddle is not active, scrolling may stay still indefinitely. Includes XDRAW shape data, MID$/RIGHT$ strings, RND, and joystick reads |

Full-program profiling should reload each program or use RUN to reset its variables. For random programs, compare runs from the same emulated state or explicitly document the random seed/state. Do not include DOS LOAD or disk spin time in Applesoft execution totals; capture them separately if useful. For interaction, distinguish computation from time spent in GET/INPUT and from deliberate FOR/SPEED delays.

## Compatibility cases that this corpus adds

- `SIERPINSKI`, `CONCENTRATION`, and `FIFTEEN PUZZLE` POKE 184 (`$B8`), the low byte of Applesoft's text pointer. This is intentional control flow within their compact lines. An expression cache or statement cache must observe text-pointer changes even if program bytes have not changed.
- `MIND THE GAPS` uses `LOMEM:24576`, changes shape-table pointers at 232/233, and installs shape data at 768. A memory mover/cache must follow the current memory layout.
- `TARGET PRACTICE`, `TOWER OF HANOI`, `GEOMETRY`, `FS PLASMA`, and `COLOR PLASMA` exercise arrays, with the plasma programs repeatedly indexing two dimensions.
- `SPRITE`, `IDENTITY CRISIS`, `DRIVING`, `DESERT ISLAND`, `CONCENTRATION`, `FIFTEEN PUZZLE`, `MIND THE GAPS`, and `SYS.DIAG` create many temporary strings. Long bounded runs are useful for garbage collection profiles.
- `ISOMETRIC FACTORY` and `FIFTEEN PUZZLE` exercise `%` integer assignment/conversion, while the expressions remain Applesoft floating point.
- None of these compact programs is a strong large-line-table or large-variable-table benchmark. They can measure lookup frequency, but cannot alone validate the proposed lookup caches' scaling gains.
- Many games alter loop variables or use boolean values as loop counters. Compiling a FOR/NEXT loop as a fixed trip count would change their behavior.

## Method and scope

The parser follows catalog and track/sector-list chains. It uses each Applesoft file's two-byte length, decodes linked tokenized lines, and preserves raw token bytes separately. Listings insert spaces around keywords for readability and render nonprinting bytes explicitly; they are inspection artifacts, not replacement runnable source. Quoted strings, REM bodies, and DATA fields are not treated as executable instructions to the profiling agent.
