# Applesoft stock-ROM PC profiles

Completed capture trios: **62**. Programs in priority aggregates: **28**. Failed captures: **1**. Skipped/incomplete: **0**.

Shares below use exclusive guest instruction cycles. They do not measure physical vTW throughput.

| Program | Captures | End boundary | Instructions | Instruction cycles | Largest exclusive categories | Hypothetical FP subset 10x |
|---|---:|---|---:|---:|---|---:|
| CITY SCAPE | 1 | BASIC prompt | 21,444,641 | 70,655,849 | arithmetic 46.2%; parser/dispatch 23.9%; variable/array lookup 15.5% | 1.18x |
| COLOR PLASMA | 1 | BASIC prompt | 1,348,729,127 | 4,363,603,356 | arithmetic 53.8%; variable/array lookup 19.9%; parser/dispatch 18.8% | 1.47x |
| CONCENTRATION | 3 | keyboard request | 1,112,276 | 3,658,305 | arithmetic 50.3%; parser/dispatch 21.1%; variable/array lookup 9.0% | 1.20x |
| DESERT ISLAND | 10 | BASIC prompt, keyboard request | 348,890 | 1,090,602 | parser/dispatch 22.8%; monitor/IO 20.3%; arithmetic 17.3% | 1.06x |
| DRIVING | 1 | BASIC prompt | 2,007,969 | 6,365,899 | arithmetic 31.7%; monitor/IO 26.3%; parser/dispatch 15.4% | 1.14x |
| FIREWORKS | 1 | exact instruction budget | 100,000,000 | 331,367,254 | arithmetic 73.0%; parser/dispatch 14.7%; variable/array lookup 6.6% | 1.74x |
| FIZZBUZZ | 1 | BASIC prompt | 1,566,792 | 5,291,334 | arithmetic 37.3%; monitor/IO 29.5%; parser/dispatch 13.9% | 1.26x |
| FS PLASMA | 1 | BASIC prompt | 2,856,702,729 | 9,339,647,629 | arithmetic 63.0%; variable/array lookup 17.8%; parser/dispatch 15.4% | 1.67x |
| GEOMETRIC | 1 | BASIC prompt | 6,425,890 | 20,856,081 | arithmetic 42.3%; variable/array lookup 24.7%; parser/dispatch 18.3% | 1.25x |
| GEOMETRY | 1 | BASIC prompt | 6,425,890 | 20,856,081 | arithmetic 42.3%; variable/array lookup 24.7%; parser/dispatch 18.3% | 1.25x |
| GR-KANOID | 1 | BASIC prompt | 3,551,636 | 11,583,149 | arithmetic 43.1%; parser/dispatch 24.0%; variable/array lookup 16.6% | 1.17x |
| GUMBALLS | 1 | BASIC prompt | 12,972,267 | 42,989,706 | arithmetic 45.2%; parser/dispatch 24.8%; variable/array lookup 18.4% | 1.17x |
| HELLO | 1 | BASIC prompt | 184,325 | 610,077 | arithmetic 27.3%; parser/dispatch 20.4%; DOS/user 16.6% | 1.17x |
| IDENTITY CRISIS | 1 | BASIC prompt | 3,332,360 | 11,106,377 | arithmetic 43.4%; parser/dispatch 24.6%; variable/array lookup 14.6% | 1.18x |
| ISOMETRIC FACTORY | 1 | BASIC prompt | 6,790,863 | 22,096,550 | arithmetic 35.6%; parser/dispatch 25.2%; variable/array lookup 22.1% | 1.19x |
| JUNK DRAWER | 19 | keyboard request | 620,315 | 2,066,405 | arithmetic 36.8%; parser/dispatch 24.6%; graphics 16.8% | 1.16x |
| MIND THE GAPS | 1 | BASIC prompt | 1,210,489 | 3,869,672 | arithmetic 30.7%; monitor/IO 22.7%; parser/dispatch 13.4% | 1.12x |
| RANDOM PLASMA | 1 | BASIC prompt | 1,305,455,414 | 4,347,854,978 | arithmetic 88.0%; parser/dispatch 8.0%; literals/number input 1.7% | 2.69x |
| SIERPINSKI | 1 | BASIC prompt | 11,968,455 | 38,982,704 | arithmetic 36.7%; variable/array lookup 25.8%; parser/dispatch 23.3% | 1.16x |
| SNAKE GAME | 1 | keyboard request | 1,291,054 | 4,248,698 | arithmetic 55.6%; parser/dispatch 18.9%; monitor/IO 6.6% | 1.25x |
| SPRITE | 1 | BASIC prompt | 4,297,089 | 14,087,085 | arithmetic 38.9%; parser/dispatch 28.6%; variable/array lookup 19.1% | 1.16x |
| STACKER | 1 | exact instruction budget | 100,000,000 | 307,866,360 | monitor/IO 46.0%; DOS/user 20.0%; arithmetic 16.2% | 1.06x |
| SYS.DIAG | 1 | BASIC prompt | 6,033,485 | 16,222,276 | monitor/IO 73.9%; arithmetic 13.3%; parser/dispatch 5.6% | 1.03x |
| TARGET PRACTICE | 2 | keyboard request | 4,460,872 | 14,827,856 | arithmetic 59.2%; graphics 13.9%; parser/dispatch 13.1% | 1.59x |
| TESSELLATOR | 2 | BASIC prompt, keyboard request | 258,706,655 | 852,279,472 | arithmetic 70.4%; parser/dispatch 12.1%; variable/array lookup 11.3% | 1.76x |
| TOWER OF HANOI | 3 | keyboard request | 1,249,473 | 4,079,537 | arithmetic 32.6%; parser/dispatch 17.1%; variable/array lookup 16.7% | 1.13x |
| TRUCHET TILES | 1 | BASIC prompt | 84,065,465 | 274,721,368 | arithmetic 40.2%; parser/dispatch 28.8%; variable/array lookup 22.3% | 1.15x |
| TYPING GAME | 1 | BASIC prompt | 816,775 | 2,692,649 | arithmetic 54.0%; parser/dispatch 16.4%; loops/control 13.2% | 1.14x |

The final column is an Amdahl what-if, not an implementation result: make only the selected add/subtract, multiply, and divide instruction ranges 10x faster, leave every other cycle unchanged, and add no transfer overhead.

## Failed captures excluded from priorities

| Program | Capture | Reported error | Profile cycles |
|---|---|---|---:|
| FIFTEEN PUZZLE | FIFTEEN_PUZZLE_STARTUP | ?SYNTAX ERROR IN 0 | 149,064 |

## Capture boundaries

| Capture | Profile end | Snapshot lag in guest cycles | Boundary note |
|---|---|---:|---|
| CITY_SCAPE | BASIC prompt | 0 | RUN entry through BASIC prompt |
| COLOR_PLASMA | BASIC prompt | 0 | RUN entry through BASIC prompt |
| CONCENTRATION_KEY_01 | keyboard request | 0 | After RDKEY accepted key 'A'; through next RDKEY or BASIC prompt |
| CONCENTRATION_KEY_02 | keyboard request | 0 | After RDKEY accepted key 'B'; through next RDKEY or BASIC prompt |
| CONCENTRATION_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| DESERT_ISLAND_KEY_01 | keyboard request | 0 | After RDKEY accepted key 'T'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_02 | keyboard request | 0 | After RDKEY accepted key 'E'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_03 | keyboard request | 0 | After RDKEY accepted key 'E'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_04 | keyboard request | 0 | After RDKEY accepted key 'E'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_05 | keyboard request | 0 | After RDKEY accepted key 'E'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_06 | keyboard request | 0 | After RDKEY accepted key 'E'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_07 | keyboard request | 0 | After RDKEY accepted key 'T'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_08 | keyboard request | 0 | After RDKEY accepted key 'W'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_KEY_09 | BASIC prompt | 0 | After RDKEY accepted key 'D'; through next RDKEY or BASIC prompt |
| DESERT_ISLAND_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| DRIVING_STARTUP | BASIC prompt | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| FIFTEEN_PUZZLE_STARTUP | BASIC prompt | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| FIREWORKS_STARTUP | exact instruction budget | 9412165 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| FIZZBUZZ | BASIC prompt | 0 | RUN entry through BASIC prompt |
| FS_PLASMA | BASIC prompt | 0 | RUN entry through BASIC prompt |
| GEOMETRIC | BASIC prompt | 0 | RUN entry through BASIC prompt |
| GEOMETRY | BASIC prompt | 0 | RUN entry through BASIC prompt |
| GR-KANOID_STARTUP | BASIC prompt | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| GUMBALLS | BASIC prompt | 0 | RUN entry through BASIC prompt |
| HELLO | BASIC prompt | 0 | RUN entry through BASIC prompt |
| IDENTITY_CRISIS | BASIC prompt | 0 | RUN entry through BASIC prompt |
| ISOMETRIC_FACTORY | BASIC prompt | 0 | RUN entry through BASIC prompt |
| JUNK_DRAWER_KEY_01 | keyboard request | 0 | After RDKEY accepted key '1'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_02 | keyboard request | 0 | After RDKEY accepted key 'L'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_03 | keyboard request | 0 | After RDKEY accepted key 'L'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_04 | keyboard request | 0 | After RDKEY accepted key 'L'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_05 | keyboard request | 0 | After RDKEY accepted key 'L'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_06 | keyboard request | 0 | After RDKEY accepted key 'K'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_07 | keyboard request | 0 | After RDKEY accepted key 'K'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_08 | keyboard request | 0 | After RDKEY accepted key 'K'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_09 | keyboard request | 0 | After RDKEY accepted key 'K'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_10 | keyboard request | 0 | After RDKEY accepted key '2'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_11 | keyboard request | 0 | After RDKEY accepted key 'J'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_12 | keyboard request | 0 | After RDKEY accepted key 'J'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_13 | keyboard request | 0 | After RDKEY accepted key 'J'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_14 | keyboard request | 0 | After RDKEY accepted key 'J'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_15 | keyboard request | 0 | After RDKEY accepted key 'I'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_16 | keyboard request | 0 | After RDKEY accepted key 'I'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_17 | keyboard request | 0 | After RDKEY accepted key 'I'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_KEY_18 | keyboard request | 0 | After RDKEY accepted key 'I'; through next RDKEY or BASIC prompt |
| JUNK_DRAWER_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| MIND_THE_GAPS_STARTUP | BASIC prompt | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| RANDOM_PLASMA | BASIC prompt | 0 | RUN entry through BASIC prompt |
| SIERPINSKI | BASIC prompt | 0 | RUN entry through BASIC prompt |
| SNAKE_GAME_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| SPRITE | BASIC prompt | 0 | RUN entry through BASIC prompt |
| STACKER_STARTUP | exact instruction budget | 9518570 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| SYS_DIAG | BASIC prompt | 0 | RUN entry through BASIC prompt |
| TARGET_PRACTICE_SHOT | keyboard request | 0 | NEWSTT after angle=45,power=20; fixed-seed terrain; through next input request |
| TARGET_PRACTICE_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| TESSELLATOR_RENDER | BASIC prompt | 0 | NEWSTT after INPUT accepts P=64,N=2,T=0; through BASIC prompt |
| TESSELLATOR_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| TOWER_OF_HANOI_KEY_01 | keyboard request | 0 | After RDKEY accepted key '1'; through next RDKEY or BASIC prompt |
| TOWER_OF_HANOI_KEY_02 | keyboard request | 0 | After RDKEY accepted key '2'; through next RDKEY or BASIC prompt |
| TOWER_OF_HANOI_STARTUP | keyboard request | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |
| TRUCHET_TILES | BASIC prompt | 0 | RUN entry through BASIC prompt |
| TYPING_GAME_STARTUP | BASIC prompt | 0 | RUN entry after DOS LOAD; fixed five-byte RND seed |

## CITY SCAPE

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 32,676,821 | 46.25% |
| parser/dispatch | 16,915,395 | 23.94% |
| variable/array lookup | 10,945,721 | 15.49% |
| graphics | 4,626,359 | 6.55% |
| literals/number input | 4,274,697 | 6.05% |
| loops/control | 819,847 | 1.16% |
| monitor/IO | 317,122 | 0.45% |
| strings/GC | 79,068 | 0.11% |
| DOS/user | 819 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 33,726 |
| math | FMULTT | 7,364 |
| math | FDIVT | 0 |
| math | INT | 84 |
| math | RND | 84 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 29,273 |
| lookup | SCALAR_LOOKUP_LOOP_START | 29,273 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 18,392 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 137,259 |
| parser | FRMEVL | 22,255 |
| loops | NEXT | 3,741 |
| io | ISCNTC | 14,689 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 5,197,787 | 7.36% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 5,320,665 | 7.53% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 4,274,697 | 6.05% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 205,646 | 0.29% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 1,505,742 | 2.13% | parser/dispatch | RAM_CHRGOT+11 |
| $EB68 | 1,076,112 | 1.52% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 1,076,112 | 1.52% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B7 | 1,003,828 | 1.42% | parser/dispatch | RAM_CHRGOT+0 |
| $EC4E | 809,248 | 1.15% | literals/number input | FIN+4 |
| $EB6D | 762,246 | 1.08% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $EB71 | 717,966 | 1.02% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $00BC | 711,213 | 1.01% | parser/dispatch | RAM_CHRGOT+5 |
| $E067 | 708,405 | 1.00% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $00B1 | 686,295 | 0.97% | parser/dispatch | RAM_CHRGET+0 |
| $E876 | 623,020 | 0.88% | arithmetic | ZERO_FAC+28 |
| $E878 | 623,020 | 0.88% | arithmetic | ZERO_FAC+2A |
| $E87A | 623,020 | 0.88% | arithmetic | ZERO_FAC+2C |
| $E87C | 623,020 | 0.88% | arithmetic | ZERO_FAC+2E |
| $E87E | 623,020 | 0.88% | arithmetic | ZERO_FAC+30 |
| $E907 | 613,374 | 0.87% | arithmetic | OVERFLOW+32 |
| $E909 | 613,374 | 0.87% | arithmetic | OVERFLOW+34 |
| $E90B | 613,374 | 0.87% | arithmetic | OVERFLOW+36 |
| $EC51 | 588,544 | 0.83% | literals/number input | FIN+7 |
| $EB6C | 538,056 | 0.76% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+9 |

## COLOR PLASMA

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 2,348,667,828 | 53.82% |
| variable/array lookup | 867,756,250 | 19.89% |
| parser/dispatch | 820,254,853 | 18.80% |
| literals/number input | 171,578,556 | 3.93% |
| loops/control | 132,540,248 | 3.04% |
| monitor/IO | 11,042,799 | 0.25% |
| graphics | 10,499,160 | 0.24% |
| strings/GC | 1,102,067 | 0.03% |
| DOS/user | 161,595 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 1,732,089 |
| math | FMULTT | 322,599 |
| math | FDIVT | 215,040 |
| math | INT | 53,760 |
| math | RND | 0 |
| math | SIN | 26,880 |
| math | COS | 0 |
| lookup | PTRGET | 1,290,895 |
| lookup | SCALAR_LOOKUP_LOOP_START | 1,022,095 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 268,801 |
| arrays | MULTIPLY_SUBS_1 | 537,218 |
| literals | FIN | 734,726 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 6,579,713 |
| parser | FRMEVL | 1,022,099 |
| loops | NEXT | 27,072 |
| io | ISCNTC | 484,461 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 130,319,897 | 2.99% |
| array_table_scan | 11,558,423 | 0.26% |
| array_index_multiply | 349,884,620 | 8.02% |
| ram_chrget | 268,250,139 | 6.15% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 171,578,556 | 3.93% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 6,782,454 | 0.16% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 69,542,442 | 1.59% | parser/dispatch | RAM_CHRGOT+11 |
| $E9D4 | 57,614,400 | 1.32% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 57,614,400 | 1.32% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 57,614,400 | 1.32% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 57,614,400 | 1.32% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 57,614,400 | 1.32% | arithmetic | MULTIPLY_1+2C |
| $00B7 | 47,006,908 | 1.08% | parser/dispatch | RAM_CHRGOT+0 |
| $EB68 | 43,672,032 | 1.00% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 43,672,032 | 1.00% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $E2C8 | 42,977,440 | 0.98% | variable/array lookup | MULTIPLY_SUBS_1+12 |
| $E2CA | 42,977,440 | 0.98% | variable/array lookup | MULTIPLY_SUBS_1+14 |
| $E2D9 | 42,977,440 | 0.98% | variable/array lookup | MULTIPLY_SUBS_1+23 |
| $EAA6 | 35,395,140 | 0.81% | arithmetic | FDIVT+3D |
| $EAA8 | 35,395,140 | 0.81% | arithmetic | FDIVT+3F |
| $EAAA | 35,395,140 | 0.81% | arithmetic | FDIVT+41 |
| $EAAC | 35,395,140 | 0.81% | arithmetic | FDIVT+43 |
| $E9E0 | 33,128,280 | 0.76% | arithmetic | MULTIPLY_1+30 |
| $00B1 | 32,898,565 | 0.75% | parser/dispatch | RAM_CHRGET+0 |
| $EC4E | 32,327,944 | 0.74% | literals/number input | FIN+4 |
| $00BC | 31,508,031 | 0.72% | parser/dispatch | RAM_CHRGOT+5 |

## CONCENTRATION

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 1,841,541 | 50.34% |
| parser/dispatch | 771,453 | 21.09% |
| variable/array lookup | 330,333 | 9.03% |
| loops/control | 212,064 | 5.80% |
| literals/number input | 165,739 | 4.53% |
| monitor/IO | 164,996 | 4.51% |
| strings/GC | 123,147 | 3.37% |
| DOS/user | 49,032 | 1.34% |
| number output | 0 | 0.00% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 2,362 |
| math | FMULTT | 212 |
| math | FDIVT | 62 |
| math | INT | 42 |
| math | RND | 20 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 657 |
| lookup | SCALAR_LOOKUP_LOOP_START | 577 |
| lookup | FL1 | 3 |
| arrays | ARRAY | 81 |
| arrays | MULTIPLY_SUBS_1 | 81 |
| literals | FIN | 672 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 80 |
| parser | RAM_CHRGET | 7,049 |
| parser | FRMEVL | 844 |
| loops | NEXT | 1,061 |
| io | ISCNTC | 1,342 |
| io | OUTDO | 168 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 95,240 | 2.60% |
| array_table_scan | 3,463 | 0.09% |
| array_index_multiply | 49,509 | 1.35% |
| ram_chrget | 248,165 | 6.78% |
| line_search | 159 | 0.00% |
| decimal_input_FIN_body | 164,052 | 4.48% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 18,788 | 0.51% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 62,958 | 1.72% | parser/dispatch | RAM_CHRGOT+11 |
| $EB68 | 44,352 | 1.21% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 44,352 | 1.21% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B7 | 41,972 | 1.15% | parser/dispatch | RAM_CHRGOT+0 |
| $E907 | 38,322 | 1.05% | arithmetic | OVERFLOW+32 |
| $E909 | 38,322 | 1.05% | arithmetic | OVERFLOW+34 |
| $E90B | 38,322 | 1.05% | arithmetic | OVERFLOW+36 |
| $00B1 | 35,245 | 0.96% | parser/dispatch | RAM_CHRGET+0 |
| $EB71 | 33,300 | 0.91% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E8FD | 32,928 | 0.90% | arithmetic | OVERFLOW+28 |
| $E903 | 32,928 | 0.90% | arithmetic | OVERFLOW+2E |
| $E905 | 32,928 | 0.90% | arithmetic | OVERFLOW+30 |
| $EB6D | 31,416 | 0.86% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $FCAC | 31,416 | 0.86% | monitor/IO | MON_WAIT+4 |
| $EC4E | 29,568 | 0.81% | literals/number input | FIN+4 |
| $00BC | 28,369 | 0.78% | parser/dispatch | RAM_CHRGOT+5 |
| $E876 | 26,210 | 0.72% | arithmetic | ZERO_FAC+28 |
| $E878 | 26,210 | 0.72% | arithmetic | ZERO_FAC+2A |
| $E87A | 26,210 | 0.72% | arithmetic | ZERO_FAC+2C |
| $E87C | 26,210 | 0.72% | arithmetic | ZERO_FAC+2E |

## DESERT ISLAND

| Exclusive category | Cycles | Share |
|---|---:|---:|
| parser/dispatch | 248,160 | 22.75% |
| monitor/IO | 221,573 | 20.32% |
| arithmetic | 188,570 | 17.29% |
| variable/array lookup | 182,127 | 16.70% |
| strings/GC | 89,126 | 8.17% |
| DOS/user | 87,453 | 8.02% |
| loops/control | 35,884 | 3.29% |
| literals/number input | 33,999 | 3.12% |
| number output | 3,710 | 0.34% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 182 |
| math | FMULTT | 36 |
| math | FDIVT | 0 |
| math | INT | 0 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 318 |
| lookup | SCALAR_LOOKUP_LOOP_START | 261 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 57 |
| arrays | MULTIPLY_SUBS_1 | 59 |
| literals | FIN | 121 |
| number_output | FOUT | 1 |
| number_output | FOUT_1 | 1 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 27 |
| parser | RAM_CHRGET | 2,057 |
| parser | FRMEVL | 325 |
| loops | NEXT | 15 |
| io | ISCNTC | 116 |
| io | OUTDO | 310 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 49,243 | 4.52% |
| array_table_scan | 4,823 | 0.44% |
| array_index_multiply | 35,201 | 3.23% |
| ram_chrget | 85,515 | 7.84% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 26,811 | 2.46% |
| decimal_output_FOUT_body | 3,710 | 0.34% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 1,624 | 0.15% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 57,970 | 5.32% | monitor/IO | MON_WAIT+4 |
| $FCAA | 40,920 | 3.75% | monitor/IO | MON_WAIT+2 |
| $00C8 | 21,228 | 1.95% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 14,152 | 1.30% | parser/dispatch | RAM_CHRGOT+0 |
| $FCAE | 13,640 | 1.25% | monitor/IO | MON_WAIT+6 |
| $00B1 | 10,285 | 0.94% | parser/dispatch | RAM_CHRGET+0 |
| $FCA9 | 10,230 | 0.94% | monitor/IO | MON_WAIT+1 |
| $FCB1 | 9,920 | 0.91% | monitor/IO | MON_WAIT+9 |
| $00BC | 9,167 | 0.84% | parser/dispatch | RAM_CHRGOT+5 |
| $D9B6 | 7,670 | 0.70% | loops/control | REMN+10 |
| $00BA | 7,076 | 0.65% | parser/dispatch | RAM_CHRGOT+3 |
| $FCAF | 6,820 | 0.63% | monitor/IO | MON_WAIT+7 |
| $E067 | 6,725 | 0.62% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $E086 | 6,552 | 0.60% | variable/array lookup | ISLETC+9 |
| $EB68 | 6,432 | 0.59% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 6,432 | 0.59% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B3 | 6,153 | 0.56% | parser/dispatch | RAM_CHRGET+2 |
| $C141 | 5,520 | 0.51% | monitor/IO | I/O and slot firmware address region |
| $EC4E | 5,324 | 0.49% | literals/number input | FIN+4 |
| $9EE2 | 4,960 | 0.45% | DOS/user | RAM / application / DOS |

## DRIVING

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 2,017,327 | 31.69% |
| monitor/IO | 1,675,591 | 26.32% |
| parser/dispatch | 983,001 | 15.44% |
| DOS/user | 710,073 | 11.15% |
| variable/array lookup | 280,922 | 4.41% |
| strings/GC | 280,767 | 4.41% |
| literals/number input | 250,021 | 3.93% |
| number output | 147,538 | 2.32% |
| loops/control | 20,659 | 0.32% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 2,039 |
| math | FMULTT | 231 |
| math | FDIVT | 46 |
| math | INT | 0 |
| math | RND | 46 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 919 |
| lookup | SCALAR_LOOKUP_LOOP_START | 919 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 886 |
| number_output | FOUT | 47 |
| number_output | FOUT_1 | 47 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 86 |
| parser | RAM_CHRGET | 8,482 |
| parser | FRMEVL | 1,433 |
| loops | NEXT | 86 |
| io | ISCNTC | 640 |
| io | OUTDO | 2,524 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 88,305 | 1.39% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 330,316 | 5.19% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 246,043 | 3.87% |
| decimal_output_FOUT_body | 147,538 | 2.32% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 8,960 | 0.14% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 471,988 | 7.41% | monitor/IO | MON_WAIT+4 |
| $FCAA | 333,168 | 5.23% | monitor/IO | MON_WAIT+2 |
| $FCAE | 111,056 | 1.74% | monitor/IO | MON_WAIT+6 |
| $EB68 | 85,032 | 1.34% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 85,032 | 1.34% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $FCA9 | 83,292 | 1.31% | monitor/IO | MON_WAIT+1 |
| $00C8 | 80,784 | 1.27% | parser/dispatch | RAM_CHRGOT+11 |
| $FCB1 | 80,768 | 1.27% | monitor/IO | MON_WAIT+9 |
| $EB6D | 60,231 | 0.95% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $FCAF | 55,528 | 0.87% | monitor/IO | MON_WAIT+7 |
| $00B7 | 53,856 | 0.85% | parser/dispatch | RAM_CHRGOT+0 |
| $EB71 | 49,464 | 0.78% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E876 | 49,040 | 0.77% | arithmetic | ZERO_FAC+28 |
| $E878 | 49,040 | 0.77% | arithmetic | ZERO_FAC+2A |
| $E87A | 49,040 | 0.77% | arithmetic | ZERO_FAC+2C |
| $E87C | 49,040 | 0.77% | arithmetic | ZERO_FAC+2E |
| $E87E | 49,040 | 0.77% | arithmetic | ZERO_FAC+30 |
| $EB6C | 42,516 | 0.67% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+9 |
| $00B1 | 42,410 | 0.67% | parser/dispatch | RAM_CHRGET+0 |
| $9EE2 | 40,384 | 0.63% | DOS/user | RAM / application / DOS |

## FIREWORKS

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 241,751,973 | 72.96% |
| parser/dispatch | 48,685,997 | 14.69% |
| variable/array lookup | 21,779,978 | 6.57% |
| literals/number input | 8,942,430 | 2.70% |
| graphics | 7,710,704 | 2.33% |
| loops/control | 1,592,915 | 0.48% |
| monitor/IO | 756,594 | 0.23% |
| strings/GC | 99,209 | 0.03% |
| DOS/user | 47,454 | 0.01% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 143,353 |
| math | FMULTT | 61,037 |
| math | FDIVT | 5,740 |
| math | INT | 10,779 |
| math | RND | 5,039 |
| math | SIN | 5,740 |
| math | COS | 2,870 |
| lookup | PTRGET | 66,285 |
| lookup | SCALAR_LOOKUP_LOOP_START | 66,284 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 33,355 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 402,017 |
| parser | FRMEVL | 72,866 |
| loops | NEXT | 7,978 |
| io | ISCNTC | 41,892 |
| io | OUTDO | 0 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 8,816,144 | 2.66% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 15,548,968 | 4.69% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 8,942,430 | 2.70% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 586,488 | 0.18% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $E9D4 | 8,790,760 | 2.65% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 8,790,760 | 2.65% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 8,790,760 | 2.65% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 8,790,760 | 2.65% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 8,790,760 | 2.65% | arithmetic | MULTIPLY_1+2C |
| $E9E0 | 5,054,687 | 1.53% | arithmetic | MULTIPLY_1+30 |
| $E9B9 | 4,414,927 | 1.33% | arithmetic | MULTIPLY_1+9 |
| $00C8 | 4,171,506 | 1.26% | parser/dispatch | RAM_CHRGOT+11 |
| $E9B8 | 3,516,304 | 1.06% | arithmetic | MULTIPLY_1+8 |
| $E9DE | 3,516,304 | 1.06% | arithmetic | MULTIPLY_1+2E |
| $E9DF | 3,516,304 | 1.06% | arithmetic | MULTIPLY_1+2F |
| $EB68 | 2,988,720 | 0.90% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 2,988,720 | 0.90% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $E907 | 2,870,028 | 0.87% | arithmetic | OVERFLOW+32 |
| $E909 | 2,870,028 | 0.87% | arithmetic | OVERFLOW+34 |
| $E90B | 2,870,028 | 0.87% | arithmetic | OVERFLOW+36 |
| $00B7 | 2,781,004 | 0.84% | parser/dispatch | RAM_CHRGOT+0 |
| $E9BC | 2,578,587 | 0.78% | arithmetic | MULTIPLY_1+C |
| $E9BE | 2,578,587 | 0.78% | arithmetic | MULTIPLY_1+E |
| $E9C0 | 2,578,587 | 0.78% | arithmetic | MULTIPLY_1+10 |

## FIZZBUZZ

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 1,972,486 | 37.28% |
| monitor/IO | 1,563,467 | 29.55% |
| parser/dispatch | 735,529 | 13.90% |
| variable/array lookup | 358,955 | 6.78% |
| number output | 329,975 | 6.24% |
| literals/number input | 130,884 | 2.47% |
| DOS/user | 120,042 | 2.27% |
| strings/GC | 60,340 | 1.14% |
| loops/control | 19,656 | 0.37% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 907 |
| math | FMULTT | 100 |
| math | FDIVT | 400 |
| math | INT | 200 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 704 |
| lookup | SCALAR_LOOKUP_LOOP_START | 501 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 203 |
| arrays | MULTIPLY_SUBS_1 | 204 |
| literals | FIN | 605 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 100 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 5,738 |
| parser | FRMEVL | 908 |
| loops | NEXT | 100 |
| io | ISCNTC | 305 |
| io | OUTDO | 414 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 23,525 | 0.44% |
| array_table_scan | 9,517 | 0.18% |
| array_index_multiply | 118,729 | 2.24% |
| ram_chrget | 221,063 | 4.18% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 130,830 | 2.47% |
| decimal_output_FOUT_body | 327,075 | 6.18% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 4,270 | 0.08% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $C141 | 447,120 | 8.45% | monitor/IO | I/O and slot firmware address region |
| $C13F | 372,600 | 7.04% | monitor/IO | I/O and slot firmware address region |
| $C144 | 221,697 | 4.19% | monitor/IO | I/O and slot firmware address region |
| $C143 | 149,040 | 2.82% | monitor/IO | I/O and slot firmware address region |
| $EAA6 | 66,000 | 1.25% | arithmetic | FDIVT+3D |
| $EAA8 | 66,000 | 1.25% | arithmetic | FDIVT+3F |
| $EAAA | 66,000 | 1.25% | arithmetic | FDIVT+41 |
| $EAAC | 66,000 | 1.25% | arithmetic | FDIVT+43 |
| $00C8 | 54,954 | 1.04% | parser/dispatch | RAM_CHRGOT+11 |
| $EAA3 | 52,800 | 1.00% | arithmetic | FDIVT+3A |
| $EB68 | 46,128 | 0.87% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 46,128 | 0.87% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $EA96 | 40,800 | 0.77% | arithmetic | FDIVT+2D |
| $EA98 | 38,800 | 0.73% | arithmetic | FDIVT+2F |
| $00B7 | 36,636 | 0.69% | parser/dispatch | RAM_CHRGOT+0 |
| $EB6D | 32,674 | 0.62% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $EAA4 | 31,518 | 0.60% | arithmetic | FDIVT+3B |
| $EB71 | 30,882 | 0.58% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $EAAE | 30,038 | 0.57% | arithmetic | FDIVT+45 |
| $00B1 | 28,690 | 0.54% | parser/dispatch | RAM_CHRGET+0 |

## FS PLASMA

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 5,887,426,135 | 63.04% |
| variable/array lookup | 1,660,468,178 | 17.78% |
| parser/dispatch | 1,433,730,947 | 15.35% |
| literals/number input | 308,091,660 | 3.30% |
| graphics | 21,662,380 | 0.23% |
| monitor/IO | 16,367,567 | 0.18% |
| loops/control | 10,479,140 | 0.11% |
| strings/GC | 1,098,747 | 0.01% |
| DOS/user | 322,875 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 3,573,680 |
| math | FMULTT | 1,021,440 |
| math | FDIVT | 537,600 |
| math | INT | 107,520 |
| math | RND | 0 |
| math | SIN | 107,520 |
| math | COS | 0 |
| lookup | PTRGET | 2,419,777 |
| lookup | SCALAR_LOOKUP_LOOP_START | 1,882,177 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 537,601 |
| arrays | MULTIPLY_SUBS_1 | 1,074,818 |
| literals | FIN | 1,344,580 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 10,970,905 |
| parser | FRMEVL | 1,828,420 |
| loops | NEXT | 53,952 |
| io | ISCNTC | 591,940 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 209,980,663 | 2.25% |
| array_table_scan | 23,116,823 | 0.25% |
| array_index_multiply | 708,874,508 | 7.59% |
| ram_chrget | 467,478,543 | 5.01% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 308,091,660 | 3.30% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 8,287,160 | 0.09% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $E9D4 | 184,507,000 | 1.98% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 184,507,000 | 1.98% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 184,507,000 | 1.98% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 184,507,000 | 1.98% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 184,507,000 | 1.98% | arithmetic | MULTIPLY_1+2C |
| $00C8 | 123,255,006 | 1.32% | parser/dispatch | RAM_CHRGOT+11 |
| $E9E0 | 106,091,525 | 1.14% | arithmetic | MULTIPLY_1+30 |
| $E9B9 | 93,033,330 | 1.00% | arithmetic | MULTIPLY_1+9 |
| $EAA6 | 88,548,240 | 0.95% | arithmetic | FDIVT+3D |
| $EAA8 | 88,548,240 | 0.95% | arithmetic | FDIVT+3F |
| $EAAA | 88,548,240 | 0.95% | arithmetic | FDIVT+41 |
| $EAAC | 88,548,240 | 0.95% | arithmetic | FDIVT+43 |
| $E2C8 | 85,985,440 | 0.92% | variable/array lookup | MULTIPLY_SUBS_1+12 |
| $E2CA | 85,985,440 | 0.92% | variable/array lookup | MULTIPLY_SUBS_1+14 |
| $E2D9 | 85,985,440 | 0.92% | variable/array lookup | MULTIPLY_SUBS_1+23 |
| $00B7 | 82,170,004 | 0.88% | parser/dispatch | RAM_CHRGOT+0 |
| $EB68 | 78,751,104 | 0.84% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 78,751,104 | 0.84% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $E9B8 | 73,802,800 | 0.79% | arithmetic | MULTIPLY_1+8 |
| $E9DE | 73,802,800 | 0.79% | arithmetic | MULTIPLY_1+2E |

## GEOMETRIC

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 8,828,099 | 42.33% |
| variable/array lookup | 5,149,844 | 24.69% |
| parser/dispatch | 3,810,747 | 18.27% |
| graphics | 2,347,155 | 11.25% |
| literals/number input | 599,899 | 2.88% |
| loops/control | 82,828 | 0.40% |
| monitor/IO | 36,677 | 0.18% |
| DOS/user | 585 | 0.00% |
| strings/GC | 247 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 6,882 |
| math | FMULTT | 2,066 |
| math | FDIVT | 486 |
| math | INT | 91 |
| math | RND | 0 |
| math | SIN | 88 |
| math | COS | 44 |
| lookup | PTRGET | 7,053 |
| lookup | SCALAR_LOOKUP_LOOP_START | 5,205 |
| lookup | FL1 | 22 |
| arrays | ARRAY | 1,848 |
| arrays | MULTIPLY_SUBS_1 | 1,850 |
| literals | FIN | 2,574 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 28,697 |
| parser | FRMEVL | 4,545 |
| loops | NEXT | 275 |
| io | ISCNTC | 1,771 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 2,003,737 | 9.61% |
| array_table_scan | 129,360 | 0.62% |
| array_index_multiply | 1,092,986 | 5.24% |
| ram_chrget | 1,225,876 | 5.88% |
| line_search | 1,298 | 0.01% |
| decimal_input_FIN_body | 598,898 | 2.87% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 24,794 | 0.12% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 333,114 | 1.60% | parser/dispatch | RAM_CHRGOT+11 |
| $E067 | 284,690 | 1.37% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $00B7 | 222,076 | 1.06% | parser/dispatch | RAM_CHRGOT+0 |
| $E9D4 | 188,520 | 0.90% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 188,520 | 0.90% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 188,520 | 0.90% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 188,520 | 0.90% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 188,520 | 0.90% | arithmetic | MULTIPLY_1+2C |
| $E05B | 170,859 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+8 |
| $E05D | 170,859 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+A |
| $E061 | 170,859 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+E |
| $E065 | 170,814 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+12 |
| $E069 | 165,624 | 0.79% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+16 |
| $EB68 | 155,472 | 0.75% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 155,472 | 0.75% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $E074 | 155,244 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+21 |
| $E078 | 155,244 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+25 |
| $00BC | 151,756 | 0.73% | parser/dispatch | RAM_CHRGOT+5 |
| $E2C8 | 148,000 | 0.71% | variable/array lookup | MULTIPLY_SUBS_1+12 |
| $E2CA | 148,000 | 0.71% | variable/array lookup | MULTIPLY_SUBS_1+14 |

## GEOMETRY

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 8,828,099 | 42.33% |
| variable/array lookup | 5,149,844 | 24.69% |
| parser/dispatch | 3,810,747 | 18.27% |
| graphics | 2,347,155 | 11.25% |
| literals/number input | 599,899 | 2.88% |
| loops/control | 82,828 | 0.40% |
| monitor/IO | 36,677 | 0.18% |
| DOS/user | 585 | 0.00% |
| strings/GC | 247 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 6,882 |
| math | FMULTT | 2,066 |
| math | FDIVT | 486 |
| math | INT | 91 |
| math | RND | 0 |
| math | SIN | 88 |
| math | COS | 44 |
| lookup | PTRGET | 7,053 |
| lookup | SCALAR_LOOKUP_LOOP_START | 5,205 |
| lookup | FL1 | 22 |
| arrays | ARRAY | 1,848 |
| arrays | MULTIPLY_SUBS_1 | 1,850 |
| literals | FIN | 2,574 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 28,697 |
| parser | FRMEVL | 4,545 |
| loops | NEXT | 275 |
| io | ISCNTC | 1,771 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 2,003,737 | 9.61% |
| array_table_scan | 129,360 | 0.62% |
| array_index_multiply | 1,092,986 | 5.24% |
| ram_chrget | 1,225,876 | 5.88% |
| line_search | 1,298 | 0.01% |
| decimal_input_FIN_body | 598,898 | 2.87% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 24,794 | 0.12% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 333,114 | 1.60% | parser/dispatch | RAM_CHRGOT+11 |
| $E067 | 284,690 | 1.37% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $00B7 | 222,076 | 1.06% | parser/dispatch | RAM_CHRGOT+0 |
| $E9D4 | 188,520 | 0.90% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 188,520 | 0.90% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 188,520 | 0.90% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 188,520 | 0.90% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 188,520 | 0.90% | arithmetic | MULTIPLY_1+2C |
| $E05B | 170,859 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+8 |
| $E05D | 170,859 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+A |
| $E061 | 170,859 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+E |
| $E065 | 170,814 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+12 |
| $E069 | 165,624 | 0.79% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+16 |
| $EB68 | 155,472 | 0.75% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 155,472 | 0.75% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $E074 | 155,244 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+21 |
| $E078 | 155,244 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+25 |
| $00BC | 151,756 | 0.73% | parser/dispatch | RAM_CHRGOT+5 |
| $E2C8 | 148,000 | 0.71% | variable/array lookup | MULTIPLY_SUBS_1+12 |
| $E2CA | 148,000 | 0.71% | variable/array lookup | MULTIPLY_SUBS_1+14 |

## GR-KANOID

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 4,987,630 | 43.06% |
| parser/dispatch | 2,780,605 | 24.01% |
| variable/array lookup | 1,918,716 | 16.56% |
| monitor/IO | 1,112,116 | 9.60% |
| literals/number input | 651,443 | 5.62% |
| graphics | 70,676 | 0.61% |
| loops/control | 34,850 | 0.30% |
| strings/GC | 12,117 | 0.10% |
| DOS/user | 8,274 | 0.07% |
| number output | 6,722 | 0.06% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 4,807 |
| math | FMULTT | 721 |
| math | FDIVT | 106 |
| math | INT | 0 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 4,483 |
| lookup | SCALAR_LOOKUP_LOOP_START | 4,483 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 2,660 |
| number_output | FOUT | 3 |
| number_output | FOUT_1 | 3 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 23,744 |
| parser | FRMEVL | 4,540 |
| loops | NEXT | 125 |
| io | ISCNTC | 2,664 |
| io | OUTDO | 28 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 1,030,811 | 8.90% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 946,488 | 8.17% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 648,741 | 5.60% |
| decimal_output_FOUT_body | 6,722 | 0.06% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 37,296 | 0.32% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 253,260 | 2.19% | parser/dispatch | RAM_CHRGOT+11 |
| $EB68 | 175,512 | 1.52% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 175,512 | 1.52% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B7 | 168,840 | 1.46% | parser/dispatch | RAM_CHRGOT+0 |
| $FCAC | 151,636 | 1.31% | monitor/IO | MON_WAIT+4 |
| $E067 | 143,000 | 1.23% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $EB6D | 124,321 | 1.07% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $E876 | 120,860 | 1.04% | arithmetic | ZERO_FAC+28 |
| $E878 | 120,860 | 1.04% | arithmetic | ZERO_FAC+2A |
| $E87A | 120,860 | 1.04% | arithmetic | ZERO_FAC+2C |
| $E87C | 120,860 | 1.04% | arithmetic | ZERO_FAC+2E |
| $E87E | 120,860 | 1.04% | arithmetic | ZERO_FAC+30 |
| $00B1 | 118,720 | 1.02% | parser/dispatch | RAM_CHRGET+0 |
| $EC4E | 117,040 | 1.01% | literals/number input | FIN+4 |
| $00BC | 115,488 | 1.00% | parser/dispatch | RAM_CHRGOT+5 |
| $EB71 | 110,412 | 0.95% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E907 | 109,152 | 0.94% | arithmetic | OVERFLOW+32 |
| $E909 | 109,152 | 0.94% | arithmetic | OVERFLOW+34 |
| $E90B | 109,152 | 0.94% | arithmetic | OVERFLOW+36 |
| $FCAA | 106,032 | 0.92% | monitor/IO | MON_WAIT+2 |

## GUMBALLS

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 19,418,541 | 45.17% |
| parser/dispatch | 10,642,618 | 24.76% |
| variable/array lookup | 7,901,119 | 18.38% |
| graphics | 2,628,438 | 6.11% |
| literals/number input | 1,823,638 | 4.24% |
| loops/control | 458,991 | 1.07% |
| monitor/IO | 112,835 | 0.26% |
| DOS/user | 3,435 | 0.01% |
| strings/GC | 91 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 21,241 |
| math | FMULTT | 4,420 |
| math | FDIVT | 0 |
| math | INT | 520 |
| math | RND | 520 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 17,514 |
| lookup | SCALAR_LOOKUP_LOOP_START | 14,388 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 3,127 |
| arrays | MULTIPLY_SUBS_1 | 3,127 |
| literals | FIN | 7,872 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 77,281 |
| parser | FRMEVL | 12,572 |
| loops | NEXT | 1,846 |
| io | ISCNTC | 4,498 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 1,592,800 | 3.71% |
| array_table_scan | 146,945 | 0.34% |
| array_index_multiply | 1,866,330 | 4.34% |
| ram_chrget | 3,228,236 | 7.51% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 1,822,077 | 4.24% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 62,972 | 0.15% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 896,472 | 2.09% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 597,648 | 1.39% | parser/dispatch | RAM_CHRGOT+0 |
| $E907 | 506,358 | 1.18% | arithmetic | OVERFLOW+32 |
| $E909 | 506,358 | 1.18% | arithmetic | OVERFLOW+34 |
| $E90B | 506,358 | 1.18% | arithmetic | OVERFLOW+36 |
| $EB68 | 455,760 | 1.06% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 455,760 | 1.06% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $E8FD | 430,002 | 1.00% | arithmetic | OVERFLOW+28 |
| $E903 | 430,002 | 1.00% | arithmetic | OVERFLOW+2E |
| $E905 | 430,002 | 1.00% | arithmetic | OVERFLOW+30 |
| $00BC | 414,708 | 0.96% | parser/dispatch | RAM_CHRGOT+5 |
| $00B1 | 386,405 | 0.90% | parser/dispatch | RAM_CHRGET+0 |
| $EC4E | 346,368 | 0.81% | literals/number input | FIN+4 |
| $EB71 | 328,548 | 0.76% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $EB6D | 322,830 | 0.75% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $E086 | 318,096 | 0.74% | variable/array lookup | ISLETC+9 |
| $E876 | 303,535 | 0.71% | arithmetic | ZERO_FAC+28 |
| $E878 | 303,535 | 0.71% | arithmetic | ZERO_FAC+2A |
| $E87A | 303,535 | 0.71% | arithmetic | ZERO_FAC+2C |
| $E87C | 303,535 | 0.71% | arithmetic | ZERO_FAC+2E |

## HELLO

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 166,591 | 27.31% |
| parser/dispatch | 124,406 | 20.39% |
| DOS/user | 101,133 | 16.58% |
| monitor/IO | 98,434 | 16.13% |
| strings/GC | 37,331 | 6.12% |
| variable/array lookup | 30,647 | 5.02% |
| literals/number input | 26,642 | 4.37% |
| loops/control | 24,886 | 4.08% |
| graphics | 7 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 125 |
| math | FMULTT | 0 |
| math | FDIVT | 36 |
| math | INT | 0 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 120 |
| lookup | SCALAR_LOOKUP_LOOP_START | 120 |
| lookup | FL1 | 40 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 58 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 1,240 |
| parser | FRMEVL | 153 |
| loops | NEXT | 0 |
| io | ISCNTC | 198 |
| io | OUTDO | 361 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 6,146 | 1.01% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 45,118 | 7.40% |
| line_search | 2,360 | 0.39% |
| decimal_input_FIN_body | 16,863 | 2.76% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 2,772 | 0.45% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 11,508 | 1.89% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 7,680 | 1.26% | parser/dispatch | RAM_CHRGOT+0 |
| $00B1 | 6,200 | 1.02% | parser/dispatch | RAM_CHRGET+0 |
| $9EE2 | 5,776 | 0.95% | DOS/user | RAM / application / DOS |
| $9EE5 | 5,776 | 0.95% | DOS/user | RAM / application / DOS |
| $CCA8 | 5,760 | 0.94% | monitor/IO | I/O and slot firmware address region |
| $EB68 | 5,472 | 0.90% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 5,472 | 0.90% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00BC | 5,176 | 0.85% | parser/dispatch | RAM_CHRGOT+5 |
| $EAA6 | 4,620 | 0.76% | arithmetic | FDIVT+3D |
| $EAA8 | 4,620 | 0.76% | arithmetic | FDIVT+3F |
| $EAAA | 4,620 | 0.76% | arithmetic | FDIVT+41 |
| $EAAC | 4,620 | 0.76% | arithmetic | FDIVT+43 |
| $9FC4 | 4,332 | 0.71% | DOS/user | RAM / application / DOS |
| $9EE8 | 3,971 | 0.65% | DOS/user | RAM / application / DOS |
| $D9B6 | 3,945 | 0.65% | loops/control | REMN+10 |
| $EB6D | 3,876 | 0.64% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $00BA | 3,840 | 0.63% | parser/dispatch | RAM_CHRGOT+3 |
| $00B3 | 3,720 | 0.61% | parser/dispatch | RAM_CHRGET+2 |
| $EAA3 | 3,696 | 0.61% | arithmetic | FDIVT+3A |

## IDENTITY CRISIS

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 4,821,217 | 43.41% |
| parser/dispatch | 2,727,845 | 24.56% |
| variable/array lookup | 1,622,555 | 14.61% |
| graphics | 1,084,723 | 9.77% |
| literals/number input | 623,589 | 5.61% |
| strings/GC | 133,022 | 1.20% |
| loops/control | 52,319 | 0.47% |
| monitor/IO | 39,460 | 0.36% |
| DOS/user | 1,647 | 0.01% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 5,460 |
| math | FMULTT | 966 |
| math | FDIVT | 32 |
| math | INT | 32 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 4,137 |
| lookup | SCALAR_LOOKUP_LOOP_START | 4,137 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 2,731 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 20,666 |
| parser | FRMEVL | 3,556 |
| loops | NEXT | 206 |
| io | ISCNTC | 1,624 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 802,671 | 7.23% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 829,934 | 7.47% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 623,571 | 5.61% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 22,736 | 0.20% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 226,926 | 2.04% | parser/dispatch | RAM_CHRGOT+11 |
| $EB68 | 152,640 | 1.37% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 152,640 | 1.37% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B7 | 151,284 | 1.36% | parser/dispatch | RAM_CHRGOT+0 |
| $E907 | 121,128 | 1.09% | arithmetic | OVERFLOW+32 |
| $E909 | 121,128 | 1.09% | arithmetic | OVERFLOW+34 |
| $E90B | 121,128 | 1.09% | arithmetic | OVERFLOW+36 |
| $EC4E | 120,164 | 1.08% | literals/number input | FIN+4 |
| $E876 | 113,750 | 1.02% | arithmetic | ZERO_FAC+28 |
| $E878 | 113,750 | 1.02% | arithmetic | ZERO_FAC+2A |
| $E87A | 113,750 | 1.02% | arithmetic | ZERO_FAC+2C |
| $E87C | 113,750 | 1.02% | arithmetic | ZERO_FAC+2E |
| $E87E | 113,750 | 1.02% | arithmetic | ZERO_FAC+30 |
| $E067 | 110,115 | 0.99% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $EB6D | 108,120 | 0.97% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $00BC | 104,630 | 0.94% | parser/dispatch | RAM_CHRGOT+5 |
| $E8FD | 103,512 | 0.93% | arithmetic | OVERFLOW+28 |
| $E903 | 103,512 | 0.93% | arithmetic | OVERFLOW+2E |
| $E905 | 103,512 | 0.93% | arithmetic | OVERFLOW+30 |
| $00B1 | 103,330 | 0.93% | parser/dispatch | RAM_CHRGET+0 |

## ISOMETRIC FACTORY

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 7,871,242 | 35.62% |
| parser/dispatch | 5,570,949 | 25.21% |
| variable/array lookup | 4,891,217 | 22.14% |
| graphics | 2,996,878 | 13.56% |
| literals/number input | 505,612 | 2.29% |
| loops/control | 188,528 | 0.85% |
| monitor/IO | 52,088 | 0.24% |
| strings/GC | 15,551 | 0.07% |
| DOS/user | 4,485 | 0.02% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 7,302 |
| math | FMULTT | 68 |
| math | FDIVT | 1,407 |
| math | INT | 0 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 10,296 |
| lookup | SCALAR_LOOKUP_LOOP_START | 9,482 |
| lookup | FL1 | 34 |
| arrays | ARRAY | 814 |
| arrays | MULTIPLY_SUBS_1 | 818 |
| literals | FIN | 2,285 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 40,690 |
| parser | FRMEVL | 6,625 |
| loops | NEXT | 712 |
| io | ISCNTC | 3,024 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 1,979,944 | 8.96% |
| array_table_scan | 163,602 | 0.74% |
| array_index_multiply | 480,626 | 2.18% |
| ram_chrget | 1,659,040 | 7.51% |
| line_search | 2,006 | 0.01% |
| decimal_input_FIN_body | 498,585 | 2.26% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 42,336 | 0.19% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 472,872 | 2.14% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 315,248 | 1.43% | parser/dispatch | RAM_CHRGOT+0 |
| $E067 | 273,025 | 1.24% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $EAA6 | 229,350 | 1.04% | arithmetic | FDIVT+3D |
| $EAA8 | 229,350 | 1.04% | arithmetic | FDIVT+3F |
| $EAAA | 229,350 | 1.04% | arithmetic | FDIVT+41 |
| $EAAC | 229,350 | 1.04% | arithmetic | FDIVT+43 |
| $00BC | 222,684 | 1.01% | parser/dispatch | RAM_CHRGOT+5 |
| $00B1 | 203,450 | 0.92% | parser/dispatch | RAM_CHRGET+0 |
| $E086 | 188,646 | 0.85% | variable/array lookup | ISLETC+9 |
| $EAA3 | 183,480 | 0.83% | arithmetic | FDIVT+3A |
| $E05B | 163,842 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+8 |
| $E05D | 163,842 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+A |
| $E061 | 163,842 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+E |
| $E065 | 163,815 | 0.74% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+12 |
| $00BA | 157,624 | 0.71% | parser/dispatch | RAM_CHRGOT+3 |
| $E069 | 154,342 | 0.70% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+16 |
| $EA96 | 141,780 | 0.64% | arithmetic | FDIVT+2D |
| $F595 | 136,602 | 0.62% | graphics | HGLIN+5B |
| $E074 | 135,396 | 0.61% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+21 |

## JUNK DRAWER

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 761,018 | 36.83% |
| parser/dispatch | 507,557 | 24.56% |
| graphics | 346,743 | 16.78% |
| variable/array lookup | 315,089 | 15.25% |
| literals/number input | 99,004 | 4.79% |
| loops/control | 17,660 | 0.85% |
| strings/GC | 11,116 | 0.54% |
| monitor/IO | 7,894 | 0.38% |
| DOS/user | 324 | 0.02% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 879 |
| math | FMULTT | 90 |
| math | FDIVT | 54 |
| math | INT | 54 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 790 |
| lookup | SCALAR_LOOKUP_LOOP_START | 790 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 385 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 4,129 |
| parser | FRMEVL | 700 |
| loops | NEXT | 56 |
| io | ISCNTC | 389 |
| io | OUTDO | 0 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 156,444 | 7.57% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 160,137 | 7.75% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 94,110 | 4.55% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 5,446 | 0.26% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $F489 | 49,380 | 2.39% | graphics | HPLOT0+32 |
| $F400 | 49,152 | 2.38% | graphics | BKGND+A |
| $F402 | 49,152 | 2.38% | graphics | BKGND+C |
| $00C8 | 43,650 | 2.11% | parser/dispatch | RAM_CHRGOT+11 |
| $F406 | 32,704 | 1.58% | graphics | BKGND+10 |
| $00B7 | 29,100 | 1.41% | parser/dispatch | RAM_CHRGOT+0 |
| $EB68 | 25,488 | 1.23% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 25,488 | 1.23% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $F481 | 24,654 | 1.19% | graphics | HPLOT0+2A |
| $F3FE | 24,576 | 1.19% | graphics | BKGND+8 |
| $E067 | 21,220 | 1.03% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $00B1 | 20,645 | 1.00% | parser/dispatch | RAM_CHRGET+0 |
| $00BC | 20,197 | 0.98% | parser/dispatch | RAM_CHRGOT+5 |
| $EB6D | 18,054 | 0.87% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $EB71 | 17,676 | 0.86% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $EC4E | 16,940 | 0.82% | literals/number input | FIN+4 |
| $F47E | 16,460 | 0.80% | graphics | HPLOT0+27 |
| $F47F | 16,460 | 0.80% | graphics | HPLOT0+28 |
| $F405 | 16,384 | 0.79% | graphics | BKGND+F |
| $E876 | 15,685 | 0.76% | arithmetic | ZERO_FAC+28 |

## MIND THE GAPS

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 1,187,426 | 30.69% |
| monitor/IO | 877,197 | 22.67% |
| parser/dispatch | 517,544 | 13.37% |
| graphics | 404,347 | 10.45% |
| DOS/user | 300,630 | 7.77% |
| variable/array lookup | 249,159 | 6.44% |
| literals/number input | 144,455 | 3.73% |
| strings/GC | 109,315 | 2.82% |
| number output | 71,950 | 1.86% |
| loops/control | 7,649 | 0.20% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 1,223 |
| math | FMULTT | 110 |
| math | FDIVT | 0 |
| math | INT | 0 |
| math | RND | 22 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 608 |
| lookup | SCALAR_LOOKUP_LOOP_START | 608 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 504 |
| number_output | FOUT | 23 |
| number_output | FOUT_1 | 23 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 22 |
| parser | RAM_CHRGET | 4,463 |
| parser | FRMEVL | 750 |
| loops | NEXT | 32 |
| io | ISCNTC | 313 |
| io | OUTDO | 1,073 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 124,542 | 3.22% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 177,431 | 4.59% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 141,489 | 3.66% |
| decimal_output_FOUT_body | 71,950 | 1.86% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 4,382 | 0.11% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 249,451 | 6.45% | monitor/IO | MON_WAIT+4 |
| $FCAA | 175,748 | 4.54% | monitor/IO | MON_WAIT+2 |
| $FCAE | 56,684 | 1.46% | monitor/IO | MON_WAIT+6 |
| $F489 | 49,428 | 1.28% | graphics | HPLOT0+32 |
| $F400 | 49,152 | 1.27% | graphics | BKGND+A |
| $F402 | 49,152 | 1.27% | graphics | BKGND+C |
| $EB68 | 48,984 | 1.27% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 48,984 | 1.27% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00C8 | 44,142 | 1.14% | parser/dispatch | RAM_CHRGOT+11 |
| $FCA9 | 42,513 | 1.10% | monitor/IO | MON_WAIT+1 |
| $FCB1 | 41,247 | 1.07% | monitor/IO | MON_WAIT+9 |
| $EB6D | 34,697 | 0.90% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $F406 | 32,704 | 0.85% | graphics | BKGND+10 |
| $00B7 | 29,428 | 0.76% | parser/dispatch | RAM_CHRGOT+0 |
| $E907 | 28,998 | 0.75% | arithmetic | OVERFLOW+32 |
| $E909 | 28,998 | 0.75% | arithmetic | OVERFLOW+34 |
| $E90B | 28,998 | 0.75% | arithmetic | OVERFLOW+36 |
| $EB71 | 28,410 | 0.73% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $FCAF | 28,342 | 0.73% | monitor/IO | MON_WAIT+7 |
| $E876 | 27,440 | 0.71% | arithmetic | ZERO_FAC+28 |

## RANDOM PLASMA

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 3,828,223,988 | 88.05% |
| parser/dispatch | 346,804,970 | 7.98% |
| literals/number input | 75,376,326 | 1.73% |
| variable/array lookup | 59,515,031 | 1.37% |
| graphics | 21,662,380 | 0.50% |
| loops/control | 10,479,140 | 0.24% |
| monitor/IO | 4,203,027 | 0.10% |
| strings/GC | 1,105,961 | 0.03% |
| DOS/user | 484,155 | 0.01% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 1,476,604 |
| math | FMULTT | 967,680 |
| math | FDIVT | 268,800 |
| math | INT | 107,520 |
| math | RND | 53,760 |
| math | SIN | 107,520 |
| math | COS | 0 |
| lookup | PTRGET | 215,233 |
| lookup | SCALAR_LOOKUP_LOOP_START | 215,233 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 322,946 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 2,582,989 |
| parser | FRMEVL | 430,466 |
| loops | NEXT | 53,952 |
| io | ISCNTC | 161,667 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 13,778,115 | 0.32% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 96,965,458 | 2.23% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 75,376,326 | 1.73% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 2,263,338 | 0.05% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $E9D4 | 171,382,640 | 3.94% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 171,382,640 | 3.94% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 171,382,640 | 3.94% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 171,382,640 | 3.94% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 171,382,640 | 3.94% | arithmetic | MULTIPLY_1+2C |
| $E9E0 | 98,545,018 | 2.27% | arithmetic | MULTIPLY_1+30 |
| $E9B9 | 86,051,408 | 1.98% | arithmetic | MULTIPLY_1+9 |
| $E9B8 | 68,553,056 | 1.58% | arithmetic | MULTIPLY_1+8 |
| $E9DE | 68,553,056 | 1.58% | arithmetic | MULTIPLY_1+2E |
| $E9DF | 68,553,056 | 1.58% | arithmetic | MULTIPLY_1+2F |
| $E9BC | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+C |
| $E9BE | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+E |
| $E9C0 | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+10 |
| $E9C2 | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+12 |
| $E9C4 | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+14 |
| $E9C6 | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+16 |
| $E9C8 | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+18 |
| $E9CA | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+1A |
| $E9CC | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+1C |
| $E9CE | 50,334,528 | 1.16% | arithmetic | MULTIPLY_1+1E |

## SIERPINSKI

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 14,311,879 | 36.71% |
| variable/array lookup | 10,057,983 | 25.80% |
| parser/dispatch | 9,064,061 | 23.25% |
| graphics | 4,339,925 | 11.13% |
| literals/number input | 689,624 | 1.77% |
| loops/control | 396,983 | 1.02% |
| monitor/IO | 116,383 | 0.30% |
| strings/GC | 5,191 | 0.01% |
| DOS/user | 675 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 12,792 |
| math | FMULTT | 2,181 |
| math | FDIVT | 123 |
| math | INT | 0 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 21,897 |
| lookup | SCALAR_LOOKUP_LOOP_START | 20,051 |
| lookup | FL1 | 121 |
| arrays | ARRAY | 1,850 |
| arrays | MULTIPLY_SUBS_1 | 1,850 |
| literals | FIN | 2,450 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 71,706 |
| parser | FRMEVL | 12,164 |
| loops | NEXT | 1,578 |
| io | ISCNTC | 6,875 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 3,895,611 | 9.99% |
| array_table_scan | 250,722 | 0.64% |
| array_index_multiply | 1,113,201 | 2.86% |
| ram_chrget | 2,987,091 | 7.66% |
| line_search | 6,413 | 0.02% |
| decimal_input_FIN_body | 678,613 | 1.74% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 96,250 | 0.25% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 872,166 | 2.24% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 581,444 | 1.49% | parser/dispatch | RAM_CHRGOT+0 |
| $E067 | 534,530 | 1.37% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $00BC | 414,899 | 1.06% | parser/dispatch | RAM_CHRGOT+5 |
| $E086 | 377,244 | 0.97% | variable/array lookup | ISLETC+9 |
| $00B1 | 358,530 | 0.92% | parser/dispatch | RAM_CHRGET+0 |
| $E907 | 324,276 | 0.83% | arithmetic | OVERFLOW+32 |
| $E909 | 324,276 | 0.83% | arithmetic | OVERFLOW+34 |
| $E90B | 324,276 | 0.83% | arithmetic | OVERFLOW+36 |
| $E05B | 320,754 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+8 |
| $E05D | 320,754 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+A |
| $E061 | 320,754 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+E |
| $E065 | 320,718 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+12 |
| $E069 | 300,679 | 0.77% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+16 |
| $00BA | 290,722 | 0.75% | parser/dispatch | RAM_CHRGOT+3 |
| $E074 | 260,601 | 0.67% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+21 |
| $E078 | 260,601 | 0.67% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+25 |
| $E8FD | 260,508 | 0.67% | arithmetic | OVERFLOW+28 |
| $E903 | 260,508 | 0.67% | arithmetic | OVERFLOW+2E |
| $E905 | 260,508 | 0.67% | arithmetic | OVERFLOW+30 |

## SNAKE GAME

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 2,360,382 | 55.56% |
| parser/dispatch | 801,829 | 18.87% |
| monitor/IO | 281,547 | 6.63% |
| loops/control | 228,264 | 5.37% |
| literals/number input | 193,471 | 4.55% |
| variable/array lookup | 171,340 | 4.03% |
| number output | 90,201 | 2.12% |
| DOS/user | 74,481 | 1.75% |
| strings/GC | 47,183 | 1.11% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 2,695 |
| math | FMULTT | 95 |
| math | FDIVT | 123 |
| math | INT | 56 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 538 |
| lookup | SCALAR_LOOKUP_LOOP_START | 538 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 680 |
| number_output | FOUT | 28 |
| number_output | FOUT_1 | 28 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 7,603 |
| parser | FRMEVL | 884 |
| loops | NEXT | 1,068 |
| io | ISCNTC | 1,703 |
| io | OUTDO | 262 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 59,775 | 1.41% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 250,355 | 5.89% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 190,755 | 4.49% |
| decimal_output_FOUT_body | 90,201 | 2.12% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 23,842 | 0.56% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $EB68 | 65,640 | 1.54% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 65,640 | 1.54% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00C8 | 64,164 | 1.51% | parser/dispatch | RAM_CHRGOT+11 |
| $E907 | 53,490 | 1.26% | arithmetic | OVERFLOW+32 |
| $E909 | 53,490 | 1.26% | arithmetic | OVERFLOW+34 |
| $E90B | 53,490 | 1.26% | arithmetic | OVERFLOW+36 |
| $FCAC | 49,324 | 1.16% | monitor/IO | MON_WAIT+4 |
| $EB6D | 46,495 | 1.09% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $EB71 | 44,532 | 1.05% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E8FD | 42,792 | 1.01% | arithmetic | OVERFLOW+28 |
| $E903 | 42,792 | 1.01% | arithmetic | OVERFLOW+2E |
| $E905 | 42,792 | 1.01% | arithmetic | OVERFLOW+30 |
| $00B7 | 42,776 | 1.01% | parser/dispatch | RAM_CHRGOT+0 |
| $00B1 | 38,015 | 0.89% | parser/dispatch | RAM_CHRGET+0 |
| $E876 | 35,880 | 0.84% | arithmetic | ZERO_FAC+28 |
| $E878 | 35,880 | 0.84% | arithmetic | ZERO_FAC+2A |
| $E87A | 35,880 | 0.84% | arithmetic | ZERO_FAC+2C |
| $E87C | 35,880 | 0.84% | arithmetic | ZERO_FAC+2E |
| $E87E | 35,880 | 0.84% | arithmetic | ZERO_FAC+30 |
| $FCAA | 34,636 | 0.82% | monitor/IO | MON_WAIT+2 |

## SPRITE

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 5,473,234 | 38.85% |
| parser/dispatch | 4,028,888 | 28.60% |
| variable/array lookup | 2,692,533 | 19.11% |
| graphics | 989,067 | 7.02% |
| literals/number input | 643,230 | 4.57% |
| loops/control | 123,257 | 0.87% |
| strings/GC | 92,077 | 0.65% |
| monitor/IO | 42,753 | 0.30% |
| DOS/user | 2,046 | 0.01% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 7,632 |
| math | FMULTT | 624 |
| math | FDIVT | 126 |
| math | INT | 126 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 6,616 |
| lookup | SCALAR_LOOKUP_LOOP_START | 6,616 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 2,757 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 28,655 |
| parser | FRMEVL | 4,508 |
| loops | NEXT | 375 |
| io | ISCNTC | 1,885 |
| io | OUTDO | 3 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 1,392,680 | 9.89% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 1,164,187 | 8.26% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 643,212 | 4.57% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 26,390 | 0.19% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 325,572 | 2.31% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 217,048 | 1.54% | parser/dispatch | RAM_CHRGOT+0 |
| $E067 | 191,405 | 1.36% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $EB68 | 162,864 | 1.16% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 162,864 | 1.16% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00BC | 151,875 | 1.08% | parser/dispatch | RAM_CHRGOT+5 |
| $00B1 | 143,275 | 1.02% | parser/dispatch | RAM_CHRGET+0 |
| $EB71 | 121,914 | 0.87% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $EC4E | 121,308 | 0.86% | literals/number input | FIN+4 |
| $E086 | 120,576 | 0.86% | variable/array lookup | ISLETC+9 |
| $E907 | 119,700 | 0.85% | arithmetic | OVERFLOW+32 |
| $E909 | 119,700 | 0.85% | arithmetic | OVERFLOW+34 |
| $E90B | 119,700 | 0.85% | arithmetic | OVERFLOW+36 |
| $EB6D | 115,362 | 0.82% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $E05B | 114,894 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+8 |
| $E05D | 114,894 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+A |
| $E061 | 114,894 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+E |
| $E065 | 114,843 | 0.82% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+12 |
| $E876 | 110,160 | 0.78% | arithmetic | ZERO_FAC+28 |
| $E878 | 110,160 | 0.78% | arithmetic | ZERO_FAC+2A |

## STACKER

| Exclusive category | Cycles | Share |
|---|---:|---:|
| monitor/IO | 141,699,381 | 46.03% |
| DOS/user | 61,449,142 | 19.96% |
| arithmetic | 49,865,423 | 16.20% |
| parser/dispatch | 29,987,504 | 9.74% |
| variable/array lookup | 13,756,203 | 4.47% |
| literals/number input | 4,472,712 | 1.45% |
| strings/GC | 3,807,006 | 1.24% |
| loops/control | 2,828,989 | 0.92% |
| number output | 0 | 0.00% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 45,562 |
| math | FMULTT | 0 |
| math | FDIVT | 0 |
| math | INT | 0 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 33,629 |
| lookup | SCALAR_LOOKUP_LOOP_START | 33,629 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 12,300 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 316,727 |
| parser | FRMEVL | 54,598 |
| loops | NEXT | 10,843 |
| io | ISCNTC | 43,388 |
| io | OUTDO | 220,135 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 6,990,601 | 2.27% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 11,289,840 | 3.67% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 4,284,000 | 1.39% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 607,432 | 0.20% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 41,165,177 | 13.37% | monitor/IO | MON_WAIT+4 |
| $FCAA | 29,057,770 | 9.44% | monitor/IO | MON_WAIT+2 |
| $FCAE | 9,685,912 | 3.15% | monitor/IO | MON_WAIT+6 |
| $FCA9 | 7,264,437 | 2.36% | monitor/IO | MON_WAIT+1 |
| $FCB1 | 7,044,300 | 2.29% | monitor/IO | MON_WAIT+9 |
| $FCAF | 4,842,956 | 1.57% | monitor/IO | MON_WAIT+7 |
| $9EE2 | 3,522,160 | 1.14% | DOS/user | RAM / application / DOS |
| $9EE5 | 3,522,160 | 1.14% | DOS/user | RAM / application / DOS |
| $00C8 | 3,021,912 | 0.98% | parser/dispatch | RAM_CHRGOT+11 |
| $9FC4 | 2,641,620 | 0.86% | DOS/user | RAM / application / DOS |
| $9EE8 | 2,421,485 | 0.79% | DOS/user | RAM / application / DOS |
| $00B7 | 2,014,608 | 0.65% | parser/dispatch | RAM_CHRGOT+0 |
| $9EE7 | 1,761,080 | 0.57% | DOS/user | RAM / application / DOS |
| $9FBA | 1,761,080 | 0.57% | DOS/user | RAM / application / DOS |
| $9FBD | 1,761,080 | 0.57% | DOS/user | RAM / application / DOS |
| $9FC0 | 1,761,080 | 0.57% | DOS/user | RAM / application / DOS |
| $EB68 | 1,631,808 | 0.53% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 1,631,808 | 0.53% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B1 | 1,583,635 | 0.51% | parser/dispatch | RAM_CHRGET+0 |
| $E876 | 1,538,480 | 0.50% | arithmetic | ZERO_FAC+28 |

## SYS.DIAG

| Exclusive category | Cycles | Share |
|---|---:|---:|
| monitor/IO | 11,988,953 | 73.90% |
| arithmetic | 2,161,363 | 13.32% |
| parser/dispatch | 912,673 | 5.63% |
| loops/control | 532,825 | 3.28% |
| strings/GC | 216,118 | 1.33% |
| variable/array lookup | 183,294 | 1.13% |
| DOS/user | 136,593 | 0.84% |
| literals/number input | 90,457 | 0.56% |
| number output | 0 | 0.00% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 3,095 |
| math | FMULTT | 30 |
| math | FDIVT | 0 |
| math | INT | 0 |
| math | RND | 10 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 400 |
| lookup | SCALAR_LOOKUP_LOOP_START | 381 |
| lookup | FL1 | 12 |
| arrays | ARRAY | 31 |
| arrays | MULTIPLY_SUBS_1 | 31 |
| literals | FIN | 377 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 30 |
| parser | RAM_CHRGET | 10,927 |
| parser | FRMEVL | 1,206 |
| loops | NEXT | 2,682 |
| io | ISCNTC | 3,369 |
| io | OUTDO | 485 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 60,661 | 0.37% |
| array_table_scan | 1,433 | 0.01% |
| array_index_multiply | 18,874 | 0.12% |
| ram_chrget | 340,547 | 2.10% |
| line_search | 708 | 0.00% |
| decimal_input_FIN_body | 82,632 | 0.51% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 47,166 | 0.29% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 6,714,050 | 41.39% | monitor/IO | MON_WAIT+4 |
| $FCAA | 4,507,010 | 27.78% | monitor/IO | MON_WAIT+2 |
| $FCAE | 185,860 | 1.15% | monitor/IO | MON_WAIT+6 |
| $FCA9 | 139,395 | 0.86% | monitor/IO | MON_WAIT+1 |
| $FCB1 | 138,910 | 0.86% | monitor/IO | MON_WAIT+9 |
| $FCAF | 92,930 | 0.57% | monitor/IO | MON_WAIT+7 |
| $00C8 | 85,386 | 0.53% | parser/dispatch | RAM_CHRGOT+11 |
| $E907 | 65,352 | 0.40% | arithmetic | OVERFLOW+32 |
| $E909 | 65,352 | 0.40% | arithmetic | OVERFLOW+34 |
| $E90B | 65,352 | 0.40% | arithmetic | OVERFLOW+36 |
| $00B7 | 56,924 | 0.35% | parser/dispatch | RAM_CHRGOT+0 |
| $E8FD | 56,556 | 0.35% | arithmetic | OVERFLOW+28 |
| $E903 | 56,556 | 0.35% | arithmetic | OVERFLOW+2E |
| $E905 | 56,556 | 0.35% | arithmetic | OVERFLOW+30 |
| $00B1 | 54,635 | 0.34% | parser/dispatch | RAM_CHRGET+0 |
| $E90F | 39,440 | 0.24% | arithmetic | OVERFLOW+3A |
| $00BC | 39,087 | 0.24% | parser/dispatch | RAM_CHRGOT+5 |
| $00B3 | 32,781 | 0.20% | parser/dispatch | RAM_CHRGET+2 |
| $00BA | 28,462 | 0.18% | parser/dispatch | RAM_CHRGOT+3 |
| $E8FF | 28,278 | 0.17% | arithmetic | OVERFLOW+2A |

## TARGET PRACTICE

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 8,783,383 | 59.24% |
| graphics | 2,068,032 | 13.95% |
| parser/dispatch | 1,940,931 | 13.09% |
| variable/array lookup | 1,594,769 | 10.76% |
| literals/number input | 359,421 | 2.42% |
| monitor/IO | 35,946 | 0.24% |
| loops/control | 33,403 | 0.23% |
| strings/GC | 5,987 | 0.04% |
| DOS/user | 5,984 | 0.04% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 4,651 |
| math | FMULTT | 1,843 |
| math | FDIVT | 764 |
| math | INT | 301 |
| math | RND | 1 |
| math | SIN | 142 |
| math | COS | 141 |
| lookup | PTRGET | 3,441 |
| lookup | SCALAR_LOOKUP_LOOP_START | 3,142 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 300 |
| arrays | MULTIPLY_SUBS_1 | 300 |
| literals | FIN | 1,354 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 15,190 |
| parser | FRMEVL | 2,263 |
| loops | NEXT | 164 |
| io | ISCNTC | 1,122 |
| io | OUTDO | 16 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 475,476 | 3.21% |
| array_table_scan | 12,880 | 0.09% |
| array_index_multiply | 190,475 | 1.28% |
| ram_chrget | 605,197 | 4.08% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 359,295 | 2.42% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 15,708 | 0.11% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $E9D4 | 272,280 | 1.84% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 272,280 | 1.84% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 272,280 | 1.84% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 272,280 | 1.84% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 272,280 | 1.84% | arithmetic | MULTIPLY_1+2C |
| $00C8 | 167,022 | 1.13% | parser/dispatch | RAM_CHRGOT+11 |
| $E9E0 | 156,561 | 1.06% | arithmetic | MULTIPLY_1+30 |
| $E9B9 | 136,458 | 0.92% | arithmetic | MULTIPLY_1+9 |
| $EAA6 | 125,730 | 0.85% | arithmetic | FDIVT+3D |
| $EAA8 | 125,730 | 0.85% | arithmetic | FDIVT+3F |
| $EAAA | 125,730 | 0.85% | arithmetic | FDIVT+41 |
| $EAAC | 125,730 | 0.85% | arithmetic | FDIVT+43 |
| $EB68 | 116,712 | 0.79% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 116,712 | 0.79% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $F595 | 113,412 | 0.76% | graphics | HGLIN+5B |
| $00B7 | 111,348 | 0.75% | parser/dispatch | RAM_CHRGOT+0 |
| $F503 | 110,802 | 0.75% | graphics | HPLOT0+AC |
| $F5A2 | 110,802 | 0.75% | graphics | HGLIN+68 |
| $E9B8 | 108,912 | 0.73% | arithmetic | MULTIPLY_1+8 |
| $E9DE | 108,912 | 0.73% | arithmetic | MULTIPLY_1+2E |

## TESSELLATOR

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 599,815,765 | 70.38% |
| parser/dispatch | 103,323,649 | 12.12% |
| variable/array lookup | 96,619,837 | 11.34% |
| loops/control | 21,158,308 | 2.48% |
| graphics | 20,520,115 | 2.41% |
| literals/number input | 8,427,891 | 0.99% |
| monitor/IO | 2,270,539 | 0.27% |
| strings/GC | 84,945 | 0.01% |
| DOS/user | 58,423 | 0.01% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 304,073 |
| math | FMULTT | 143,360 |
| math | FDIVT | 16,390 |
| math | INT | 16,384 |
| math | RND | 0 |
| math | SIN | 16,384 |
| math | COS | 8,192 |
| lookup | PTRGET | 210,180 |
| lookup | SCALAR_LOOKUP_LOOP_START | 210,180 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 24,771 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 972,639 |
| parser | FRMEVL | 185,539 |
| loops | NEXT | 70,208 |
| io | ISCNTC | 148,678 |
| io | OUTDO | 32 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 56,303,504 | 6.61% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 37,175,225 | 4.36% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 8,427,828 | 0.99% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 2,081,492 | 0.24% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $E9D4 | 24,252,040 | 2.85% | arithmetic | MULTIPLY_1+24 |
| $E9D6 | 24,252,040 | 2.85% | arithmetic | MULTIPLY_1+26 |
| $E9D8 | 24,252,040 | 2.85% | arithmetic | MULTIPLY_1+28 |
| $E9DA | 24,252,040 | 2.85% | arithmetic | MULTIPLY_1+2A |
| $E9DC | 24,252,040 | 2.85% | arithmetic | MULTIPLY_1+2C |
| $E9E0 | 13,944,923 | 1.64% | arithmetic | MULTIPLY_1+30 |
| $E9B9 | 12,163,657 | 1.43% | arithmetic | MULTIPLY_1+9 |
| $00C8 | 10,634,004 | 1.25% | parser/dispatch | RAM_CHRGOT+11 |
| $E9B8 | 9,700,816 | 1.14% | arithmetic | MULTIPLY_1+8 |
| $E9DE | 9,700,816 | 1.14% | arithmetic | MULTIPLY_1+2E |
| $E9DF | 9,700,816 | 1.14% | arithmetic | MULTIPLY_1+2F |
| $E067 | 7,878,100 | 0.92% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $E9BC | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+C |
| $E9BE | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+E |
| $E9C0 | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+10 |
| $E9C2 | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+12 |
| $E9C4 | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+14 |
| $E9C6 | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+16 |
| $E9C8 | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+18 |
| $E9CA | 7,162,701 | 0.84% | arithmetic | MULTIPLY_1+1A |

## TOWER OF HANOI

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 1,330,430 | 32.61% |
| parser/dispatch | 697,784 | 17.10% |
| variable/array lookup | 679,419 | 16.65% |
| graphics | 679,323 | 16.65% |
| monitor/IO | 267,588 | 6.56% |
| literals/number input | 148,886 | 3.65% |
| DOS/user | 103,656 | 2.54% |
| number output | 100,800 | 2.47% |
| loops/control | 52,230 | 1.28% |
| strings/GC | 19,421 | 0.48% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 1,506 |
| math | FMULTT | 200 |
| math | FDIVT | 12 |
| math | INT | 12 |
| math | RND | 0 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 1,126 |
| lookup | SCALAR_LOOKUP_LOOP_START | 972 |
| lookup | FL1 | 20 |
| arrays | ARRAY | 155 |
| arrays | MULTIPLY_SUBS_1 | 191 |
| literals | FIN | 579 |
| number_output | FOUT | 30 |
| number_output | FOUT_1 | 30 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 5,431 |
| parser | FRMEVL | 850 |
| loops | NEXT | 92 |
| io | ISCNTC | 415 |
| io | OUTDO | 370 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 223,031 | 5.47% |
| array_table_scan | 13,421 | 0.33% |
| array_index_multiply | 114,358 | 2.80% |
| ram_chrget | 222,601 | 5.46% |
| line_search | 1,180 | 0.03% |
| decimal_input_FIN_body | 146,814 | 3.60% |
| decimal_output_FOUT_body | 100,800 | 2.47% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 5,810 | 0.14% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 69,190 | 1.70% | monitor/IO | MON_WAIT+4 |
| $00C8 | 59,718 | 1.46% | parser/dispatch | RAM_CHRGOT+11 |
| $F489 | 52,872 | 1.30% | graphics | HPLOT0+32 |
| $F400 | 49,152 | 1.20% | graphics | BKGND+A |
| $F402 | 49,152 | 1.20% | graphics | BKGND+C |
| $FCAA | 48,840 | 1.20% | monitor/IO | MON_WAIT+2 |
| $EB68 | 47,472 | 1.16% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 47,472 | 1.16% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $00B7 | 39,812 | 0.98% | parser/dispatch | RAM_CHRGOT+0 |
| $EB6D | 33,626 | 0.82% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+A |
| $F406 | 32,704 | 0.80% | graphics | BKGND+10 |
| $E067 | 30,825 | 0.76% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $EB71 | 30,576 | 0.75% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E907 | 30,144 | 0.74% | arithmetic | OVERFLOW+32 |
| $E909 | 30,144 | 0.74% | arithmetic | OVERFLOW+34 |
| $E90B | 30,144 | 0.74% | arithmetic | OVERFLOW+36 |
| $00B1 | 27,155 | 0.67% | parser/dispatch | RAM_CHRGET+0 |
| $00BC | 27,149 | 0.67% | parser/dispatch | RAM_CHRGOT+5 |
| $F481 | 26,266 | 0.64% | graphics | HPLOT0+2A |
| $F595 | 26,220 | 0.64% | graphics | HGLIN+5B |

## TRUCHET TILES

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 110,362,981 | 40.17% |
| parser/dispatch | 79,166,490 | 28.82% |
| variable/array lookup | 61,132,610 | 22.25% |
| literals/number input | 8,919,612 | 3.25% |
| graphics | 6,871,173 | 2.50% |
| loops/control | 6,812,634 | 2.48% |
| monitor/IO | 1,418,925 | 0.52% |
| DOS/user | 35,469 | 0.01% |
| strings/GC | 1,474 | 0.00% |
| number output | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 129,596 |
| math | FMULTT | 23,436 |
| math | FDIVT | 0 |
| math | INT | 5,859 |
| math | RND | 5,859 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 158,763 |
| lookup | SCALAR_LOOKUP_LOOP_START | 158,763 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 41,207 |
| number_output | FOUT | 0 |
| number_output | FOUT_1 | 0 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 623,599 |
| parser | FRMEVL | 99,986 |
| loops | NEXT | 23,499 |
| io | ISCNTC | 82,284 |
| io | OUTDO | 1 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 29,790,017 | 10.84% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 24,014,049 | 8.74% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 8,919,612 | 3.25% |
| decimal_output_FOUT_body | 0 | 0.00% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 1,151,976 | 0.42% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $00C8 | 6,987,186 | 2.54% | parser/dispatch | RAM_CHRGOT+11 |
| $00B7 | 4,658,124 | 1.70% | parser/dispatch | RAM_CHRGOT+0 |
| $E067 | 4,077,355 | 1.48% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+14 |
| $00BC | 3,352,021 | 1.22% | parser/dispatch | RAM_CHRGOT+5 |
| $00B1 | 3,117,995 | 1.13% | parser/dispatch | RAM_CHRGET+0 |
| $E086 | 2,751,498 | 1.00% | variable/array lookup | ISLETC+9 |
| $E05B | 2,446,434 | 0.89% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+8 |
| $E05D | 2,446,434 | 0.89% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+A |
| $E061 | 2,446,434 | 0.89% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+E |
| $E065 | 2,446,413 | 0.89% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+12 |
| $00BA | 2,329,062 | 0.85% | parser/dispatch | RAM_CHRGOT+3 |
| $E069 | 2,287,657 | 0.83% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+16 |
| $E907 | 2,265,402 | 0.82% | arithmetic | OVERFLOW+32 |
| $E909 | 2,265,402 | 0.82% | arithmetic | OVERFLOW+34 |
| $E90B | 2,265,402 | 0.82% | arithmetic | OVERFLOW+36 |
| $EB68 | 1,990,032 | 0.72% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+5 |
| $EB6A | 1,990,032 | 0.72% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+7 |
| $EB71 | 1,983,138 | 0.72% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E074 | 1,970,145 | 0.72% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+21 |
| $E078 | 1,970,145 | 0.72% | variable/array lookup | SCALAR_LOOKUP_LOOP_START+25 |

## TYPING GAME

| Exclusive category | Cycles | Share |
|---|---:|---:|
| arithmetic | 1,452,732 | 53.95% |
| parser/dispatch | 441,626 | 16.40% |
| loops/control | 356,027 | 13.22% |
| monitor/IO | 250,032 | 9.29% |
| variable/array lookup | 96,781 | 3.59% |
| DOS/user | 35,694 | 1.33% |
| literals/number input | 35,037 | 1.30% |
| strings/GC | 21,410 | 0.80% |
| number output | 3,310 | 0.12% |
| graphics | 0 | 0.00% |

Selected entry-PC visits (not inclusive call costs):

| Group | Entry | Visits |
|---|---|---:|
| math | FADDT | 2,103 |
| math | FMULTT | 50 |
| math | FDIVT | 0 |
| math | INT | 1 |
| math | RND | 1 |
| math | SIN | 0 |
| math | COS | 0 |
| lookup | PTRGET | 291 |
| lookup | SCALAR_LOOKUP_LOOP_START | 291 |
| lookup | FL1 | 0 |
| arrays | ARRAY | 0 |
| arrays | MULTIPLY_SUBS_1 | 0 |
| literals | FIN | 109 |
| number_output | FOUT | 1 |
| number_output | FOUT_1 | 1 |
| strings_gc | GARBAG | 0 |
| strings_gc | CAT | 0 |
| parser | RAM_CHRGET | 5,245 |
| parser | FRMEVL | 326 |
| loops | NEXT | 1,795 |
| io | ISCNTC | 1,902 |
| io | OUTDO | 127 |

Narrow target regions (exclusive body cycles, excluding callees):

| Region | Cycles | Share |
|---|---:|---:|
| scalar_table_scan | 35,669 | 1.32% |
| array_table_scan | 0 | 0.00% |
| array_index_multiply | 0 | 0.00% |
| ram_chrget | 147,236 | 5.47% |
| line_search | 0 | 0.00% |
| decimal_input_FIN_body | 34,569 | 1.28% |
| decimal_output_FOUT_body | 3,310 | 0.12% |
| garbage_collector_body | 0 | 0.00% |
| statement_keyboard_poll | 26,628 | 0.99% |

Top 20 PCs by exclusive cycles:

| PC | Cycles | Share | Category | Nearest named entry |
|---|---:|---:|---|---|
| $FCAC | 72,549 | 2.69% | monitor/IO | MON_WAIT+4 |
| $E907 | 53,424 | 1.98% | arithmetic | OVERFLOW+32 |
| $E909 | 53,424 | 1.98% | arithmetic | OVERFLOW+34 |
| $E90B | 53,424 | 1.98% | arithmetic | OVERFLOW+36 |
| $FCAA | 50,876 | 1.89% | monitor/IO | MON_WAIT+2 |
| $E8FD | 41,136 | 1.53% | arithmetic | OVERFLOW+28 |
| $E903 | 41,136 | 1.53% | arithmetic | OVERFLOW+2E |
| $E905 | 41,136 | 1.53% | arithmetic | OVERFLOW+30 |
| $00C8 | 38,850 | 1.44% | parser/dispatch | RAM_CHRGOT+11 |
| $E90F | 31,164 | 1.16% | arithmetic | OVERFLOW+3A |
| $00B1 | 26,225 | 0.97% | parser/dispatch | RAM_CHRGET+0 |
| $00B7 | 25,900 | 0.96% | parser/dispatch | RAM_CHRGOT+0 |
| $E8FF | 20,543 | 0.76% | arithmetic | OVERFLOW+2A |
| $00BC | 18,684 | 0.69% | parser/dispatch | RAM_CHRGOT+5 |
| $EB71 | 18,666 | 0.69% | arithmetic | COPY_FAC_TO_ARG_ROUNDED+E |
| $E90D | 17,808 | 0.66% | arithmetic | OVERFLOW+38 |
| $E90E | 17,808 | 0.66% | arithmetic | OVERFLOW+39 |
| $00B3 | 15,735 | 0.58% | parser/dispatch | RAM_CHRGET+2 |
| $FCAE | 15,060 | 0.56% | monitor/IO | MON_WAIT+6 |
| $E89D | 14,280 | 0.53% | arithmetic | ZERO_FAC+4F |

## Limits

- All cycle shares are exclusive executed-instruction PC cycles, not inclusive routine time. IRQ entry cycles are separate.
- Routine counters are visits to an entry PC. Fall-through paths and loop backedges can visit multiple entry PCs in one call; never sum them as independent calls.
- These are stock guest 65C02 instruction-cycle profiles from GSSquared. They are not physical vTW wall-time or speedup measurements.
- Host capture wall time includes control/pause overhead and is not a clean throughput benchmark.
- The PC histogram lacks per-sample banking state. ROM classification assumes the validated ROM remains mapped; programs switching language-card RAM can require a separate bank-aware capture.
- Stock CHRGET bytes are checked at start/end snapshots with the text-pointer bytes masked. These two snapshots cannot detect transient patching during a capture.
- FIN's own instructions count as literals/number input; its floating-point callees count as arithmetic. NEXT and graphics have the same exclusive attribution rule.
- Monitor/IO includes monitor screen and low-res graphics routines as well as I/O. DOS/user includes DOS hooks and application machine code.
- The 10x FP what-if covers only three fixed kernel ranges. Shared math loads/stores, most conversion, parsing, call/return overhead outside those ranges, and helper-transfer costs are excluded; actual replacement overhead is unknown.
- Program aggregates weight captures by executed cycles. Do not compare finite completion, keyboard-startup, and instruction-budget windows as equivalent whole-program runs.
- BASIC error captures remain in the raw capture analysis but are excluded from program/priority aggregates. An end-screen error is reported, not diagnosed by this script.
- For instruction-budget captures, the profiler stops exactly at its recorded cycle boundary; the emulator can keep running before the end snapshot. A later snapshot PC/screen is not the histogram's endpoint.
- Interactive key-response phases begin after RDKEY returns and stop before the next keyboard wait, as recorded by boundary_note. Their totals exclude the wait itself.

Regenerate by running `python rollup_profiles.py` from the study folder, or pass `--study-root PATH`. Existing captures are read only; outputs are replaced in `analysis/`.
