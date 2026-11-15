# RDP / F3DEX2 renderer: sources and what is NOT verified

`src/render/rdp.c` (software RDP), `src/render/f3dex2.c` (display-list interpreter standing in for the F3DEX2 microcode) and
`src/render/vi_out.c` (video-interface scan-out) were written from documents, not from the console's hardware or microcode source.
This file says which statements rest on a document and which do not. Source comments marked `FROM KNOWLEDGE` mean: I did not have a
document open that states it; the item was decided from memory and then looked at on rendered frames (a picture looking right is
weaker evidence than the document, and says nothing about rare cases).

## Documents the code cites
* The decomp's own `lib/ultralib/include/PR/gbi.h` - every command ENCODING the game uses (combiner input numbers, render-mode bits, image
  formats, F3DEX2 command numbers). The test `tests/native/render/rdp_gbi_check.c` builds its display lists with this header's macros.
* n64brew wiki, "Reality Display Processor" pages (command layouts, other-modes bits, z-compare) and the fast64 "Graphics Pipeline Details"
  pages (colour combiner, blender).
* Nintendo N64 Programming Manual, section 11.7 (lighting: the microcode normalises the transformed light direction) and the F3DEX3 porting
  guide's statement that "F3D through F3DEX2 normalize light directions in the microcode".
* libultra manual page of `gSPTextureRectangle` (copy-mode `dsdx` scale).
* Nintendo N64 Programming Manual 14.1 "Texture Rectangles" (ultra64.ca copy): S and T are the coordinates at the rectangle's top-left
  corner, `dsdx = 1 << 10` is "1 texel per pixel", bilinear uses "only three points", and average mode needs the start point 0.5 ("set the
  start point to 0.5 and then step by 1 texel per pixel"). Section 12.5 (texture filter): point = nearest texel, bilinear = the three
  nearest texels. The manual does NOT state a half-texel rule for bilinear sampling in words.

## Checked by a test (`tests/native/render/run_render_checks.py`)
* Colour-combiner field decoding for five modes (`G_CC_SHADE`, `G_CC_PRIMITIVE`, primitive x shade, `(PRIM-ENV)*SHADE+ENV`,
  `(SHADE-ENV)*PRIM`), computed against the documented equation `(A - B) * C + D`.
* A texture rectangle drawn 1:1 (`s0 = 0`, `dsdx = 1.0`) shows the texels exactly, point-sampled AND bilinear (test "texture rectangle 1:1").
  The old sampler subtracted half a texel before the bilinear filter; the test fails with it (white column split 120/120 over two columns) and
  passes now. This was the "fuzzy, horizontally misaligned" battle text of the Windows tester (their report: battle HUD text).
* Vertex lighting does not depend on a scale in the modelview matrix (regression for the dark Pokemon), tolerance 9 of 255. The old code
  fails 6 of these checks, the current code passes all.

## NOT verified (each is a place where the picture may differ from the console)
| Where | Statement | Status |
|---|---|---|
| `f3dex2.c` lighting | exact fixed-point scaling of the lit colour (about 1% in theory) | unchecked, no reference available |
| `f3dex2.c` texgen | the look-at based texture-coordinate generation also uses normalised directions | by analogy with lighting, NOT documented |
| `f3dex2.c` | the viewport transform flips y; front faces have negative area after the flip | FROM KNOWLEDGE, judged on frames |
| `f3dex2.c` | which vertex gives the colour of a flat-shaded triangle | FROM KNOWLEDGE |
| `f3dex2.c` | clip-plane handling | FROM KNOWLEDGE |
| `rdp.c` TMEM | the layout of texture memory (every second row has its 32-bit halves swapped; RGBA32 split low/high 2 KiB; palettes replicated 4 times) | FROM KNOWLEDGE - the n64brew TMEM page was not reachable. Loads and reads use the same address function, so pictures are consistent even if the layout differs from the hardware's |
| `rdp.c` bilinear | the 3-point filter (three texels, not four) | the manual says "three points"; WHICH three (the triangle choice by `fs + ft < 32`) is FROM KNOWLEDGE |
| `rdp.c` bilinear | texel `i` sits AT coordinate `i` (no half-texel shift) | follows from the manual's average-mode statement (start point 0.5 = between texels 0 and 1); not stated in words for the general case. Triangles use the same sampler, so a wrong rule would shift 3D textures by half a texel |
| `rdp.c` coverage | the 3-bit coverage stored in the top bits of the colour (value 7 for a full pixel) | FROM KNOWLEDGE |
| `rdp.c` rectangles | lower-right corner inclusive in fill/copy mode, exclusive otherwise | FROM KNOWLEDGE |
| `rdp.c` copy mode | `dsdx` four times too large in copy mode | FROM KNOWLEDGE (libultra man page, not re-read) |
| `rdp.c` | no anti-aliasing coverage, no dithering model beyond what the comments say | known deviation, reported at run time |
| `vi_out.c` | interlace: field 1 starts one line lower (`vStart` 35 / 37 in half-line units) | taken from libultra's video-mode tables in the decomp; shown to remove the vertical shake in menus on Linux |
| all | whether a Windows window shows the picture without flicker | NEVER RUN here (no Windows); a back buffer was added on the reasoning that the old direct drawing erased and redrew each frame |

Run `grep -n "FROM KNOWLEDGE" src/render/*.c` for the exact lines; this table can lag behind the code.
