# PGR3 engine quirks for a future native renderer

This collects what PGR3's engine does that a native renderer has to model,
for a port of a later game on the same engine (a later Project Gotham Racing,
or another Bizarre Creations title). Each item says what the game does,
how it showed up, and what the renderer does about it. The story of how each
was found is in `native-renderer-plan.md`; this is the checklist without the
history.

Addresses are PGR3's, in the **patched** image (`default.xex` with
`default.xexp`). They will not carry over. Use them to find the equivalent
code by behaviour: `scripts/re/funcdb.py` for callers and disassembly, and
`scripts/re/match_names.py` for shape matching against named functions.

Three kinds of quirk are mixed in a port, and they carry over differently:

| Kind | Example | Expect in a later title |
|---|---|---|
| **Engine** (Bizarre's renderer) | world matrices written into the ring, the rear-view mirror's corner, vertex pulling | Most of it, possibly refactored |
| **XDK** (D3D 2099) | device offsets, `Clear`'s stencil register, no `Present` | Re-derive for the new XDK; the shapes stay close |
| **Port** (rr6's renderer) | resolution scale, discard ring | Unchanged unless the renderer is rebuilt |

---

## 1. Working method

- **Use the patched image for everything.** Codegen, the shader extractor and
  the runtime must see the same bytes. The title update changed 50 shaders,
  and the shader cache is keyed by a hash of the whole container, so a cache
  built from the unpatched XEX misses on most of them. Even `VdGlobalDevice`'s
  import moved (`0x820005A0` patched, `0x8200059C` unpatched).
  `pgr3_dump_image` writes the loaded image, and the extractor scans that.
- **Compare scenes, not frame numbers.** Game time is not tied to presents,
  so two runs of the same script are at different places by frame 6000. The
  xenos build (`PGR3_NATIVE_GPU=OFF`) is the reference. Dump both with
  `pgr3_dump_frames` and match frames by what is on screen.
- **Script input in seconds from boot,** not frames. Movie skips and the title
  screen's input window vary between runs. The camera button is **LB**, and
  it is ignored during the race intro. With no throttle once the race starts,
  the player car goes transparent.
- **xenos references need `--readback_resolve=full`**, or the dump tool reads
  resolve memory that was never written.
- **Tools that found most bugs:** `pgr3_trace_frame` (every draw, resolve,
  clear and target bind of a frame, with call chains), `pgr3_trace_vs` (vertex
  records and constants for one VS), RenderDoc captures replayed from Python
  through `qrenderdoc --python` (scripts in `scratch/rdc/`), and a per-draw
  pixel history done by replaying each draw. RenderDoc's own pixel history
  returned nothing on the 2x MSAA scene.

## 2. Frame structure

A race frame has about 2,200-4,800 indexed draws and about 26 resolves.

| Pass | Target | Notes |
|---|---|---|
| Car shadows | 512x512 (player) and 256x256 (AI) `D24FS8`, depth only, 1x | Resolved as depth into each car's shadow map |
| Environment | 512x512, format 12 (`7e3_AS_16_16_16_16`), 4x MSAA, six faces | Resolved with clear (`0x300`) into a cube |
| Rear-view mirror (cockpit views) | Top-left 336x80 of the scene surface | Resolved by rect into its own texture; see §3 |
| Scene | 1024x600 7e3 (`k_2_10_10_10_FLOAT`), 2x MSAA, depth `D24FS8` | Resolved as FP16 colour and as depth |
| Motion blur | Full-screen rect list on the scene | Reprojects through `g_blurMatrix`, only where scene alpha is 1 |
| Luminance | 64, 16, 4 FP16 chain, then two 1x1 `k_16_16_FLOAT` adaptation targets | Several targets share one base address |
| Bloom | 341x200 `k_8_8_8_8` | At night, fed by the MRT bright pass (§4) |
| Composite | `ps_8200E300`: bloom, exposure, tone map into 1024x600 | Last 3D pass; HUD drawn on top |
| Minimap | 256x256, 4x MSAA | Stack-built shaders (§7) |

- **There is no `D3DDevice_Present`.** The frame end (`sub_821507A0`) resolves
  the back buffer into the front-buffer texture itself (flags `0x300`) and
  calls `D3DDevice_Swap`. Hook Swap and present that resolve target.
- The scene renders at 1024x600 to fit 2x MSAA colour plus depth in EDRAM
  (1950 of 2048 tiles) without predicated tiling. The front buffer is
  1280x720.
- The frame-end resolve clears the scene to (0,0,0,0), but see §3 for what
  the scene clear does next.

## 3. Clears and EDRAM

These are the quirks most likely to recur, and the least visible in a trace.

- **The game calls `D3DDevice_ClearF` directly**, not only through `Clear`.
  PGR3 has four direct callers. Hooking only `Clear` silently drops:
  - the race scene's clear (`sub_8229EFC0`, flags `0x3F`): colour, depth and
    stencil, to the fog colour **with alpha 1**;
  - the mirror pass's depth clear (`sub_8229EDB0`, flags `0x10`);
  - the environment pass's clear (`sub_8229DF80`) and one more
    (`sub_821B50A0`).

  Without the scene clear, the mirror's corner survived into the scene as a
  second mirror. Hook `ClearF` and pass its float colour through unclamped,
  because the targets are HDR.
- **The rear-view mirror renders into the scene surface.** In the dash and
  in-car views, the mirror draws into the top-left 336x80 of the race surface
  and resolves that rect (`rect=0,0-336,80`, flags 0) into its texture. The
  scene's `ClearF` then wipes the corner. Expect other picture-in-picture
  views (replay insets, split screen) to do the same.
- **Surfaces share EDRAM, and the game relies on it.** The host keeps each
  surface in its own texture, so any behaviour that comes from two surfaces
  sharing tiles has to be modelled by hand:
  - **Title screen:** the race-scene `ClearF` clears the title's target to the
    sky colour. The Bink planes then render into surfaces at the same EDRAM
    base, each cleared to black first, and the menu blends the movie over
    what is left. On Xenos that is black. The host now treats a colour clear
    to zero as clearing every surface that lies wholly inside the cleared
    tiles (`ClearEdramAliases`, from the surface's `EdramInfo` and `Size`).
    Zero is safe because it reads as zero in every format.
  - **Scene alpha:** the shadow and environment passes put depth and colour
    at EDRAM bases 0 and 224, inside the scene's colour tiles. The renderer
    forces alpha 1 when a resolve-clear clears a 7e3 surface. The scene's own
    `ClearF` already writes alpha 1, so that workaround may now be redundant.
- **The depth convention is reversed:** clear to 0, test `GEQUAL`. A depth
  surface can be smaller than the colour target bound with it (Xenos depth
  follows the colour pitch), so the host grows the depth texture to cover.
- **Predicated tiling** (`BeginTiling`/`EndTiling`) is used only in the front
  end. The host draws once with no EDRAM limit: `BeginTiling` becomes the
  first tile's clear, `EndTiling` one whole-surface resolve, and the tile
  count stays 0.

## 4. Resolves

- **Depth resolves are sampled as `k_24_8_FLOAT`**, with depth in `.x`. Copy
  depth into an `R32_FLOAT` texture. The scene depth, the car shadow maps and
  several 256x256 maps are read this way. A 2x MSAA depth source takes
  sample 0 in a shader, because hardware MSAA resolve does not apply to depth.
- **A whole-surface resolve from a larger surface is a 1:1 corner copy.** The
  car shadow surfaces are also bound beside 1024x600 and 1280x720 targets, so
  the host grows them. Xenos copies a resolve with no source rect 1:1 and
  clips it to the destination, so scaling the whole surface down put a small
  car in the corner of each map, and the AI cars' shadows came out as boxes.
- **At night the bright pass writes two targets.** `ps_8200D0E0` writes the raw
  bright pass to RT0 and a compressed glow, `x/(x+k)`, to RT1, which is
  resolved with `D3DRESOLVE_RENDERTARGET1` (flags bits 0-1 = 1). Model MRT
  slot 1 with its own write mask (`RB_COLOR_MASK` bits 4-7). Without it, the
  bloom took the raw bright pass and blew out the highlights.
- **The resolve destination's format wins.** The 7e3 scene resolves into
  `k_8_8_8_8` textures (bloom, front buffer); give those `R8G8B8A8_UNORM`
  so values over 1 clamp as on Xenos. A partial resolve that converts formats
  needs the copy shader under a scissor, not a whole-surface copy.
- **Several resolve targets share one base address** (the luminance chain
  and both 1x1 adaptation targets). Mirrors keyed by address need the size
  and format as well.
- **Some textures are baked once, by a resolve, while a course loads.** The
  crowd impostor atlas (512x192 `k_6_5_5`, `0x40396320`) is drawn once and
  sampled all race. An optimisation that skips resolve copies (resolve
  aliasing) lost it, so it is off by default.

## 5. Shader constants

- **World matrices are written straight into the command ring.**
  `D3DDevice_BeginShaderConstantF4` (`sub_823D8E08`) reserves a
  `SET_CONSTANT` packet and returns its payload pointer, and the caller writes
  the float4s itself. The device's constant shadow never sees them. PGR3
  sends every scene object's world matrix this way (VS c0-c3, and
  `g_mWeightedWorld` at c64-c67), plus instance data (VS c52+, PS c20+).
  Missing this drew every object at the world origin.
- **Apply ring payloads in command order.** Copy them into the shadow at the
  next draw, the next `BeginShaderConstantF4`, or the next
  `Set*ShaderConstantF`, whichever comes first. PGR3 writes a car's matrix
  into the ring and then resets c0-c3 with `SetVertexShaderConstantF` before
  the scenery. Applying at the draw let the car's matrix win, and fog taken
  from it washed out parts of Tokyo.
- **Bool constants matter.** The device's bool file is at `+0x2780`;
  `D3DDevice_SetPixelShaderConstantB` writes PS bools from bit 128. The
  foliage and building shader `ps_820B4100` picks its diffuse map on PS b0
  and otherwise samples a building crop sheet. Uploading bools as zero made
  the foliage grey squares. Nothing sets VS bools in PGR3.

## 6. Geometry

- **The game builds its own vertex and index buffer headers.**
  `CreateVertexBuffer`/`CreateIndexBuffer` are not linked. VB `Common` is
  `0x40090001` (cached memory), IB `0x400A0001` (16-bit). The game also
  overrides `XMemAlloc`/`XMemFree`.
- **Instanced data is fetched by index arithmetic.** Ten VS compute the fetch
  index (`index mod N` per vertex, `index / N` per instance), and
  `vs_820C4738` fetches through a fetched index. Neither the input assembler
  nor CPU de-instancing can model that, so those shaders pull vertices: every
  `vfetch` not indexed by `r0.x` marks the shader as pulling, and fetches
  read raw dwords from streams 0-3 decoded per format. Every VS also gets the
  vertex index in `r0.x`, with the base vertex passed separately on D3D12.
- **Vertex formats the input assembler gets wrong:**
  - `SHORT4` positions are **integers**, not normalised. Bound SNORM they
    shrank the course onto the origin.
  - DEC3N/DEC4N normals, and the 11:11:10 UHEND3N/UHEND3 texcoord family.
  - Integer TEXCOORDs (`UBYTE4`, `UINT1`) carry the instance-transform index;
    Xenos converts them to float.
  - The minimap's `SHORT2` BLENDINDICES (colour index, edge coordinate) need
    the 16-bit swap.
- **Full-screen passes are rect lists** (primitive 8) drawn through
  `DrawVerticesUP`, three vertices each, with the viewport transform off.
  Expand each to a quad, with the fourth corner opposite the right-angle
  vertex.
- **Point sprites:** `vs_820A7608` exports point size (export 63); the host
  must expand sprites. **1D textures** are fetched as 2D at `v = 0.5`.
- **Foliage uses alpha to coverage** (`RB_COLORCONTROL` `0x87000016`, with
  the alpha test off). The per-quad dither offsets are not modelled.

## 7. Shaders

- **309 shaders, all created at boot:** 305 containers in the image (rev
  `0x0E`) and **4 built on the stack**. `sub_821545A8` (boot) and
  `sub_821DA180` (race load; the minimap pair) store each container word by
  word from instruction immediates, so no byte scan finds them. The extractor
  replays such functions in a small PPC model.
- **A developer hot-reload path ships in the game.** `sub_82154370` walks the
  list of created shaders (head `0x82544674`) and reloads
  `GAME:\DontShip\Shaders\<name>.xpu` when it changes. Retail never fires it,
  but the list names every shader (`record+12`), which helps debugging.
- **Spec constants used:** every PS needs `ALPHA_TEST`; most VS need the
  normal-format variants (`R11G11B10_NORMAL`, `DEC3N_NORMAL`). Prelink every
  subset, or the first draw of each variant stalls.
- **No runtime creation from streamed data** was seen in any mode played, but
  online play was not run. Keep the cache-miss warning.

## 8. Textures, streaming and lifetime

- **Packed resources (`.xpr`) load in place,** so most textures never reach
  `D3DResource_Destroy`. Track lifetime through
  `XPhysicalAlloc`/`XPhysicalFree`.
- **Courses stream into memory that earlier data held.** A mirror built once
  and kept until its fetch constant changes shows stale contents: magenta
  normal maps sat in diffuse slots while a course streamed. Tiled textures
  join the mirror signature through a sparse texel hash (64 samples a
  frame); linear ones through a full hash. Vertex and index buffers
  unchanged for 60 frames become "cold" (16-sample hash each frame, a full
  rehash every 8th).
- **Formats beyond rr6's:** `k_5_6_5` (expanded to RGBA8; plume has no
  565), `k_16`, `k_6_5_5`, the 7e3 back buffer (`0x1A22ABBF`), and linear
  volume textures (rows at the fetch pitch rounded to 32 blocks, slices the
  height rounded to 32 block rows apart).
- **Bink movies are decoded into planes on the GPU:** a 1280x720 luma plane
  and two 640x360 chroma planes, rendered into surfaces at EDRAM base 0 and
  resolved. The menu then composites them. The planes' EDRAM aliasing is §3's
  title-screen case.

## 9. Queries and synchronisation

- **Occlusion queries are issued for culling.** The host answers
  `GetData` at once (events done, occlusion 2^20 samples), which turns
  occlusion culling off. Real host queries are open work.
- **The query-slot pool head** (device `+0x4FDC`) must be seeded as
  `D3DDevice_Initialize` leaves it; a zeroed head handed out slots at a null
  pointer.
- **CPU reads of depth resolves, fences and queries were ruled out** as gates
  on scenery submission (`pgr3_scenery_probe` on `sub_8220D0B8`).

## 10. XDK 2099 notes

Re-derive these for whatever XDK the next title links; the method transfers.

- Pair render- and sampler-state setters by state index through the
  `{Get, Set, Default}` tables (`scripts/re/state_setters.py`). The enum
  indices are the API's, so they match across XDKs.
- `D3DDevice_Clear` and `ClearF` take Z in f1, which also uses up a GPR slot,
  so stencil is in r9 (`Clear`) or r8 (`ClearF`).
- `D3DDevice_Resolve` takes extra arguments on the stack, under a 416-byte
  frame (`508(r1)`, `516(r1)`).
- The 2099 device matches 2135 up to the pixel shader (`+0x3290`). The tiling
  and command state (`+0x32AC`-`+0x3544`) and the vertex shader (`+0x4FD8`)
  moved.

## 11. Performance

- A race is CPU-bound on draw submission at 2,200-4,100 draws a frame.
  Binding streams and textures under a lock per slot cost 8.6 ms a frame.
  Bind all slots under one lock, cache texture lookups per thread (keyed by
  fetch words, frame and a generation counter), and skip slots unchanged
  since the last draw. Draws cost about 2.5 us each after that.
- Capture a pipeline precache (`pgr3_pso_capture`) across every city and a
  night race; first-run pipeline builds cost 10-20 ms each.

---

## Bring-up checklist for a new title

1. Dump the patched image and extract shaders from it. Check the count
   against a runtime census, and look for stack-built containers (functions
   that load the container magic `0x102A` with `lis`).
2. Find the frame end: does it call `Present`, or resolve and `Swap` itself?
3. Hook `ClearF` as well as `Clear`, and log every direct caller.
4. Hook `BeginShaderConstantF4` (or its successor) before judging any scene
   geometry, and apply its payloads in command order.
5. Trace one frame of each mode and list the passes: MSAA counts, formats,
   EDRAM bases, MRT use, depth resolves, rect lists.
6. Look for surfaces sharing EDRAM bases (the trace prints the surface words)
   and for picture-in-picture views drawn into a corner of a larger surface.
7. List the vertex formats and fetch patterns; mark the shaders that compute
   their fetch index.
8. Check streamed content: do textures or buffers change in place while
   their headers stay the same?
