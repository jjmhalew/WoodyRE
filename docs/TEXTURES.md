# Texture packs (PORT EXTRA)

The original has no way to replace its textures; the port lets any texture the game uploads be swapped for a PNG of
any size, e.g. for HD packs. Code: `src/texpack.c`, hooked into the three places that upload textures:
`upload_texture` (render_gl.c: every `.tex` frame, world and models), `upload` (hud.c: every bank image - HUD, pickups,
particles, speech bubbles, sky faces, menu sheets - and the font pages) and `upload` (blackbox.c: the BlackBox images).

## 1. Names
A texture is known by a 64-bit FNV-1a hash of what the game reads from its files:
- `.tex` frame: tag `'T'`, width, height (32-bit little endian each), then the frame's 16-bit texels as stored;
- bank image: tag `'I'`, width, height, then the RGBA the port uploads (after the BGRA bottom-up conversion, or the raw
  row order of the sky faces, docs/SKY.md).

Identical texels give the same hash in every level, so one file replaces a texture everywhere. Every frame of an animated
texture is its own texture. File names are `<anything>_<16 hex digits>.png` (or just `<16 hex digits>.png`); only the
hex part counts, so a pack may rename `128x128_1a2b....png` to `grass_1a2b....png`.

## 2. Folders
Both live in the game's folder, next to `woodyre.cfg` (next to `WoodyRE.exe`, or `%LOCALAPPDATA%\WoodyRE` for the
standalone exe; the developer build uses the current directory):
- `mods\textures\` - the replacements, any subfolders (8 deep), read once at the first texture upload. When two files
  carry the same hash, the first one found wins. The log prints `texture pack: N replacement textures`.
- `mods\dump\<level>\` - written by `--dumptex`: every texture as `<w>x<h>_<hash>.png`, exactly as the game shows it
  (colour key texels transparent black). Bank 0 (`Common\<character>.rck`) goes to `mods\dump\Common\`. A texture
  that is already there is not written again. Play through a level with `--dumptex` to collect everything it uses;
  the `.tex` and the bank images are complete at level load, only the BlackBox images come later. `--dumptex all`
  loads and frees every level once and quits: everything except the BlackBox images (975 different textures on the
  English CD, 2747 files counting the copies per level).

## 3. How a replacement is drawn
- The game keeps every size, texture coordinate and HUD layout of the original; only the sampled image changes. Keep
  the aspect ratio of the original.
- All mip levels are made with a 2x2 box filter and sampled trilinearly (GL_LINEAR_MIPMAP_LINEAR); the original's own
  textures keep their 4-level 16-bit chain (render_gl.c, 0x47fa60).
- Alpha: a `.tex` group with the colour key bit (flags bit 0) gets the PNG's alpha cut at 128 and black under
  transparent texels (as the original's key, 0x47fc1e - no coloured fringes); a mip texel is opaque when at least two of
  its four sources are. Other `.tex` groups are opaque (alpha ignored; additive groups use only the colour). Bank images
  use the PNG's alpha as is.
- What cannot be replaced: polygons drawn in a flat material colour (material bit 15, ARGB1555 - Woody's own body is
  such a model), the 16 radial light textures (generated, 0x480090), the HNM films, and the rendered shadows.

## 4. An AI-upscaled pack: `make_hd_textures.bat`
A script in the repository root makes a 4x pack on the player's own PC (Windows; any Vulkan GPU):
1. `WoodyRE.exe --dumptex all` (§2);
2. `texup pre` (`tools/native/texup.c`, built by `build.bat texup`): every texture once (by hash) that the pack does
   not have yet, with a margin of half its size on every side - its own opposite edges for an opaque texture (so the
   upscaler sees it repeat and leaves no seam where it tiles), its edge texels for one with transparency. Transparent
   texels first get the colour of their opaque neighbours (bled outwards ring by ring), so the black under the colour
   key does not smear into the edges; the alpha goes to a separate grey image with the same margin;
3. Real-ESRGAN ncnn-vulkan (github.com/xinntao/Real-ESRGAN, release v0.2.5.0, downloaded once into `tools\realesrgan`
   and checked against its SHA-256) upscales both 4x; default model `realesr-animevideov3-x4` (closest to the original
   art, ~30 s for the whole game on an RX 6800), `make_hd_textures.bat anime` = `realesrgan-x4plus-anime` (crisper
   edges, flattens noisy surfaces like sand, water and grass; ~2 min);
4. `texup post` crops the margin off, takes the alpha from its own upscale (cut at 128 again where the original was
   on/off, i.e. the colour key; as is where it was soft) and writes `mods\textures\hd\<w>x<h>_<hash>.png`.
A second run only does textures the pack lacks, so deleting single PNGs (the original comes back) or adding a level's
BlackBox dump later is cheap. The pack is about 460 MB and makes a level load ~1 s longer. It is derived from the game's
own art: for the player's own use, never part of the repository or a release.
