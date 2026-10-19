# Native renderer plan for pgr3

Template: `D:\rr6` (Ridge Racer 6), whose native renderer is itself a port of
re:Blue's. rr6's `docs/xenosrecomp-integration-plan.md` records how each piece
was found and fixed; this plan follows the same phases and points at it rather
than repeating it. SDK: rexglue 0.10.0.

---

## Status (2026-10-19)

| Phase | State |
|---|---|
| 0. Baseline and tooling | **In progress.** The xenos build boots into a race under a scripted pad. Ported from rr6: `pgr3_input_script`, `pgr3_null_audio`, `pgr3_dump_frames`, `pgr3_trace_frame`, the abort logger, `scripts/re/funcdb.py`; added `pgr3_dump_raw` and `scripts/re/match_names.py`. See Phase 0 findings. |
| 1. Shader extraction | **Done.** The `pgr3_shaders` build target extracts **309** shaders from the patched image (`pgr3_dump_image` + `scripts/extract_shaders.py`). The runtime census covers all 309, byte for byte, with nothing missing. See Phase 1 findings. |
| 2. XenosRecomp | **Done.** All 309 shaders compile to DXIL and SPIR-V with `REBLUE_RECOMP PGR3_RECOMP`, on branch `pgr3` of the fork in `thirdparty/XenosRecomp` (uncommitted). RR6's 176 still compile, and their HLSL changes only as intended. See Phase 2 findings. |
| 3. Build integration | **Done.** Both configurations build. Every build makes the shader cache; `PGR3_NATIVE_GPU=ON` builds `pgr3.exe` (D3D12) and `pgr3_vk.exe` (Vulkan) on plume with the cache and 548 prelinked variants. Until Phase 5 they log that the renderer is missing and exit. |
| 4. D3D identification (XDK 2099) | **Done.** `config/d3d.toml` names 74 functions, including `BeginTiling`/`EndTiling` and `XPhysicalAlloc`/`Free`. `src/gpu/d3d.h` has the 2099 device and resource layouts, every field checked against PGR3's code. The render setup is confirmed: 1024x600, 2x MSAA, 7e3. See Phase 4 findings. |
| 5. Native renderer bring-up | **In progress.** rr6's renderer is ported and runs PGR3 on D3D12 and Vulkan from boot through the front end, garage and menus into a London race. The race now has its full scenery: trees, crowd, fences, lane markings and textured buildings. Instanced fetches are pulled in the shader. Rect lists are expanded to quads. Depth resolves feed the shadow maps. Constants the game writes into the ring, and the bool constants, reach the host. MSAA is modelled (2x scene, 4x environment). Open: other cities and modes against xenos. See Phase 5 progress. |
| 6. Hardening and performance | **In progress.** Race frames hold 60 fps at the default 2x scale; see Phase 6 progress. |
| 7. Geometry Wars (`gw.xex`) | Out of scope until 5 is done. |

## What we know about PGR3

### The image is the patched one

The SDK loads `default.xex` and applies the sibling `default.xexp` (title
update) in `UserModule` (`rexglue-sdk/src/system/user_module.cpp:104`), and
codegen goes through the same `Runtime::LoadXexImage`. So codegen, the function
addresses in `pgr3_config.toml`, and the guest memory the renderer sees are all
the **patched** image. Anything we derive at build time must come from that
image too.

### XDK libraries

| Library | PGR3 (`default.xex`, patched) | RR6 |
|---|---|---|
| D3D9 | **2.0.2099.6** | 2.0.2135.0 |
| D3DX9, XGRAPHC | 2.0.2099.3 | 2.0.2135.0 |
| XAPILIB | 2.0.2099.11 | 2.0.2135.1 |
| Extra | XMP, XHV; the patch adds XFFB 2.0.4025.2 | XNET |

2099 is the launch XDK, one revision before RR6's 2135. Expect the same API
shapes and a similar `D3DDevice` layout, but **no rr6 address or offset carries
over**; every one is re-derived (Phase 4).

### Shaders (census of `default_patched.xex`)

- **305 unique shaders (103 VS, 202 PS) from 332 container references**, all
  container **rev 0x0E** (`0x102A0E00`/`0x102A0E01`), the revision rr6's
  XenosRecomp fork already parses.
- Compiler stamp "Xbox 360 Shader Compiler 2.0.2099.6" on 297; 8 have a valid
  CTAB with an empty creator string (`ps_8200DC90`, `ps_8200E300`,
  `ps_8200EA08`, `ps_820B6360`, `vs_820AEDA0`, `vs_820AF2F8`, `vs_820B0060`,
  `vs_820B0BA8`). Possibly middleware (Bink?); check in Phase 2.
- 176 to 3,932 bytes each (median ~1.4 KB, 472 KB total), VA
  `0x8200C380`..`0x820DD888`.
- Against the unpatched `default.xex`: 255 shaders have the same microcode but
  differ in uninitialised padding (definition-table header `+0x2C`) and CTAB
  bytes; **50 are new microcode** (45 VS in `0x82094AD0`..`0x820A49B0`, 5 PS in
  `0x82089110`..`0x8208A230`). Because the cache key is XXH3 over the whole
  container, a cache built from the unpatched XEX would miss on 203 of 305.
- rr6's `scripts/extract_shaders.py` finds the same set once it reads the
  patched image (encryption and basic compression are the same).
- **Plus 4 stack-built shaders**, which no byte scan finds (Phase 1 findings).
  That makes **309** in all (105 VS, 204 PS).
- Scratch scanner: `scratch/scan_shaders.py` (gitignored) (generic over
  container revisions; reuses rr6's `load_image`).

### Content that the renderer will meet

- `Game/Areas/*_cubemaps*.xpr`, `Materials/*.dds`: packed resources loaded in
  place, so (as in RR6) most textures will never reach `D3DResource_Destroy`;
  lifetime will come from `XPhysicalAlloc`/`XPhysicalFree`.
- `.bik` movies (43): Bink, decoded on the CPU into textures; expect linear,
  CPU-rewritten planes like RR6's movie path.
- `.stream2`, `.pak`: streamed course data, so mirrors will churn during a race.
- **Render setup (confirmed in Phase 4):** the race scene renders at 1024x600
  with **2x MSAA** into **7e3** (`k_2_10_10_10_FLOAT`) EDRAM colour and
  `D24FS8` depth, sized to fit EDRAM without tiling, and resolves to FP16
  textures. Environment passes use **4x MSAA**. So the native renderer needs
  re:Blue's MSAA resolves back (rr6 removed them in `bb8e5ea`; restore from
  `bb8e5ea^`) and a host format for 7e3 (Phase 4 findings).
- `gw.xex`: Geometry Wars, a **separate title executable** (LZX-compressed)
  with its own statically linked D3D9 2099 and its own shaders. It is not in
  `pgr3_manifest.toml`. See Phase 7.

### Phase 0 findings

**D3D 2099 is nearly 2135.** `scripts/re/match_names.py` compares rr6's named
functions with pgr3's by instruction shape (immediates masked). About 60 of
rr6's 77 D3D names match at 1.00, differing only in branch targets and a few
device offsets. Not carried over yet: the one-instruction tail calls, the
VB/IB creators and the `Get*` accessors (see the header of `config/d3d.toml`).

- **No `D3DDevice_Present`.** The game's frame end, `sub_821507A0`, calls
  `D3DDevice_Resolve` (flags `0x300`) and then `D3DDevice_Swap`
  (`0x823CADB8`) itself. The frame counter, and later the native renderer,
  hook `Swap`.
- **Device offsets:** render targets `+0x30F8`, textures `+0x31A0`, streams and
  index buffer are as in 2135. The high fields sit 0x10 lower (vertex shader
  `+0x4FD8`, draw fields `+0x34D0`). The rest is Phase 4 work.
- **Fences:** `InsertFence` `0x823D1280`, `IsFencePending` `0x823D12B8`,
  `BlockOnFence` `0x823D0D50`.
- **Shader creation:** `D3DDevice_CreateVertexShader` `0x823CA900` and
  `D3DDevice_CreatePixelShader` `0x823CA810` take the container in r3 and
  return the shader object, as in 2135.

**The xenos baseline runs.** Under `pgr3_input_script` the game boots (logo
at frame ~900, Press Start at ~1500, profile at ~2100) and A presses take it
through Gotham Career, Solo Career and a street race on Medium, in the race
from frame ~4800. RT drives. The game presents at 60 Hz while booting and 30
Hz in menus and races. The front buffer is 1280x720 `k_8_8_8_8`.

**A race frame** (trace of frame 6000, `pgr3_trace_frame`): about 4,800
indexed draws and 26 resolves per present.

| Pass | Targets |
|---|---|
| Shadows | six depth-only passes (no colour target), resolved with flags `0x4` (depth) |
| Environment/reflection | several passes, resolved with clear (`0x300`) into a `k_16_16_16_16_EXPAND` target |
| Scene | surface `0x40008000`: resolved as FP16 colour (`k_16_16_16_16_FLOAT`, 1024x600) and as depth (`k_24_8_FLOAT`, flags `0x14`) |
| Luminance | 64x64, 16x16, 4x4 FP16 (all at one base), then two 1x1 `k_16_16_FLOAT` adaptation targets (at one base) |
| Bloom | 341x200 `k_8_8_8_8` |
| Final | tone-mapped 1024x600 `k_8_8_8_8` with the HUD; the minimap is a 256x256 target |

So the native renderer needs, beyond rr6: **depth resolves** (RR6 never had
one), **FP16 resolve targets**, resolves into **aliased targets** (a chain
reusing one base), and `SetRenderTarget` slot 1 (bound, though to null, in
every pass seen).

**Determinism:** race frames do not repeat by frame number. Game time is not
tied to presents, so two runs of the same script are at different places on
the track by frame 6000 (rr6 found the same). Comparisons use matching
scenes, not frame numbers: menus and the race start, or a frame picked by
content.

**Dump tool fixes over rr6's:**
- Xenos endian 1 is 8-in-16 and 3 is 16-in-32; rr6 had the two swaps
  exchanged (it only met 8-in-32).
- Tiled readback lands **4 KiB** past the fetch base at both 32 and 64bpp (rr6
  read this as one 32x32 tile; fitted from `pgr3_dump_raw` bytes with
  `scratch/untile.py`).
- Each resolve is dumped as soon as it lands (`InsertFence` +
  `BlockOnFence`), named by its sequence in the frame
  (`pgr3_frame_N_r<S>_<VA>_<W>x<H>.bmp`). rr6 dumped three presents later,
  which reads the wrong contents when a target's memory is reused.
- `k_16_16_FLOAT` and `k_16_16_16_16_FLOAT` decode (clamped to 0..1, peak
  logged); rr6 treated format 10 (`k_8_8`) as 32bpp.

---

## Approach

The same as rr6's, for the same reason: XenosRecomp output can only be used by
a renderer with its binding model, so the native renderer replaces the xenos
plugin instead of feeding it (rr6 plan, section 3).

- **Wrap, don't replace.** The game's own D3D still creates and owns every
  guest object; the host mirrors them by guest address and reads draw state
  from the device's register shadows (rr6 plan, milestone 2).
- **Port from rr6, not re:Blue.** rr6 is the closer starting point: the same
  container revision, an adjacent XDK, the same kind of statically linked D3D,
  and it has already shed the Blue Dragon code.
- **Keep the xenos build working** behind `PGR3_NATIVE_GPU=OFF`. It is the
  reference every native step is compared against.

### What ports from rr6

| | Files (rr6 `src/`) |
|---|---|
| **As is** (rename `rr6`→`pgr3`, `RR6_`→`PGR3_`) | `gpu/`: `device*`, `frame_ring`, `host_heap`, `host_resource_heap`, `bindless*`, `deferred_destroy`, `graveyard`, `device_lost`, `dred`, `format`, `byte_swap`, `sampler_cache`, `texture_upload`, `texture_replace`, `surface_registry`, `present`, `perf_stats`, `gpu_profiling`, `frame_dump`, `imgui_overlay_drawer`, `pipeline/pipeline_cache`, `pipeline_state`, `pso_recorder`, `shaders/{guest_shaders,shader_linker,dxc_link}`, `shaders/hlsl/*`; `core/*`; `platform/*` (`input_script`, `silent_audio`, `xenos_resolve_dump`, `guest_abort_log`, `fatal_dialog`) |
| **Re-derive for XDK 2099** | `gpu/d3d.h` (device and resource layouts), `config/d3d.toml`, `config/xapi.toml`, every address in `gpu/hooks/*.cpp` (`kVdGlobalDeviceImport`, the `Clear` register quirk, the `Resolve` stack argument, the `Destroy` type switch) |
| **Revisit for PGR3's sizes and passes** | `gpu/output.*` (1280x720 canvas, the 320x180 glow special case), `resolve.cpp` (design width, glow upscale, the FXAA hook on `ps_82062AB8`), `constant_buffers.cpp` (the `ps_82060208` resolve-pattern fit), `hooks/state.cpp` (the 1440x800 depth growth), `device.h` defaults |
| **Drop** | `game/lod.cpp`, `game/xbox_live.cpp` (RR6 gameplay hooks), `pipeline/cache/pipeline_state_cache.h` (start empty; it is RR6 data) |
| **Submodules** | `thirdparty/plume` (stuken `rr6` branch), `thirdparty/XenosRecomp` (stuken `rr6` branch, then a `pgr3` branch or a shared one; see Phase 2), `thirdparty/rexglue-sdk` (stuken `main`), miniz, zstd |
| **Build** | `cmake/native_gpu.cmake`, `cmake/shader_cache.cmake`, `cmake/shaders.cmake`, `tools/prelink_shader_cache.cpp`, the top-level `CMakeLists.txt` layout, `CMakePresets.json` |
| **Scripts** | `scripts/re/funcdb.py` (codegen-output queries), `scripts/extract_shaders.py` (with the image change in Phase 1) |

---

## Plan

### Phase 0: Baseline and comparison tooling

Every later fix in rr6 was found by diffing intermediate surfaces against the
xenos build, so the reference comes first.

1. **Done:** the xenos build runs from a fresh tree (codegen, reconfigure,
   build) against the SDK install at `c53c48a` (stuken/rexglue-sdk, the
   commit rr6 pins). **Open:** pin it as the `thirdparty/rexglue-sdk`
   submodule, as rr6 does.
2. **Done:** tooling ported (see Phase 0 findings), with the frame counter on
   `D3DDevice_Swap` and the dump fixes listed there.
3. **Partly done:** reference dumps of the main menu and a race start (Gotham
   Career, Solo Career, the first street race, Medium), kept in `scratch/`.
   The script is
   `start@1500,a@2100,a@2700,...,a@4800` (A every 300 frames), then
   `rt@4800+N`. **Open:** the garage, a race in each city sunny and night, a
   replay and a Bink movie. These need menu scripts.
4. **Done:** `CMakeLists.txt` now uses `src/pgr3.rc.in`.

**Exit (revised):** a scripted run reaches a race in the xenos build, and its
dumps decode every 32bpp and FP16 resolve. Frame-for-frame repeatability is
not achievable (Phase 0 findings), so references are matched by scene.

### Phase 1: Shader extraction from the patched image

The extractor must read exactly the bytes the game passes to
`D3DDevice_Create*Shader`.

1. **Recommended:** a small host tool, `pgr3_dump_image`, that brings up the
   SDK `Runtime` the way `codegen_context.cpp` does, loads `game:\default.xex`
   (so `UserModule` applies `default.xexp`), and writes the image from guest
   memory. Give `extract_shaders.py` an `--image` mode that skips decryption
   and scans a raw image. The bytes then match the runtime by construction,
   and the build needs only the retail files.
2. **Interim:** point the extractor at `assets/default_patched.xex`, which it
   already reads. It needs a hand-made file in `assets/`, so it should not be
   the documented path.
3. Avoid porting LZX delta patching to Python; the SDK already has it
   (`XexModule::ApplyPatch`, `lzx.cpp`).
4. Once the shader hooks exist (Phase 4), log the hash of every created
   container and dump any miss. Play every mode once to rule `.pak`/`.stream2`
   in or out as a shader source. RR6 found no runtime shader creation; PGR3 has
   more shaders and streams its content, so this has to be checked rather than
   assumed.

**Exit:** the build extracts 305 shaders from the patched image; the runtime
census shows 100% hits.

**Done**, with 309 shaders rather than 305:

- `tools/dump_image.cpp` builds `pgr3_dump_image`, which loads the XEX through a
  tool-mode `Runtime` (title update applied) and writes
  `generated/default.image`. `extract_shaders.py --image <image> <xex>`
  scans it, taking the load address from the XEX header; the PE header's
  `ImageBase` is the PE default `0x00400000`. The `pgr3_shaders` target (in
  `ALL`) reruns when the XEX, the patch, the tool or the script changes. The
  build no longer needs Python's `cryptography`.
- The 305 image shaders are byte-identical to the scan of the hand-made
  `default_patched.xex`, so that file is no longer needed.
- Step 4 ran early: `pgr3_shader_census` (`src/platform/shader_census.cpp`,
  xenos build) writes every created container to `<user>/shader_census/`, and
  `scripts/check_shader_census.py` compares that with `generated/shaders`.

### Phase 1 findings

- **Four shaders are built on the stack from instruction immediates.**
  `sub_821545A8` (one VS and one PS, during boot) and `sub_821DA180` (one VS
  and one PS, while a race loads) store each container word by word (`li`,
  `lis`/`ori` to registers, `stw` to `r1+N`; `sub_821545A8` is 335 stores with
  no conditional branch). They then pass `r1+N` to `D3DDevice_Create*Shader`
  or a wrapper. The census found them as containers outside the image (at
  `0x7018xxxx`, a thread stack). `extract_shaders.py` now replays every
  function that materialises `lis rX,0x102A` in a small PPC model (constant
  instructions evaluated, stores into a model stack, conditional branches
  falling through) and takes each complete container passed in r3 or r5 to a
  call. They are named after the creating call (`ps_82154A7C`, `vs_82155054`,
  `vs_821DA5E0`, `ps_821DAA64`), with `origin` = `stack:sub_...` in
  `shaders.csv`.
- **All 332 image references are created at boot** (by frame ~1500), as in
  RR6.
- **A developer hot-reload path is still in the game.** `sub_82154370` walks
  the list of created shaders (head at `0x82544674`), stats
  `GAME:\DontShip\Shaders\<name>.xpu`, and recreates a shader whose file
  changed. Retail has no `DontShip` folder, so it never fires. The native
  renderer should leave it alone, but its list also names every shader
  (`record+12`), which may help debugging.
- **No shader is stored loose in the game data:** a scan of all 6 GB of
  `assets/` for rev-0x0E containers finds none. Containers inside compressed
  archives cannot be ruled out this way, so the native renderer should warn on
  a cache miss (rr6's hit-rate log does).
- Coverage of the census so far: boot, the front end, Gotham Career menus and
  one race. Every image shader and both stack-built pairs were created by
  then.

### Phase 2: XenosRecomp for PGR3

1. Start from rr6's fork, which already has the rev 0x0E header, definition
   table and literal-register handling, the co-issue snapshot fix, `ALPHA_TEST`,
   user clip distances, `g_NdcScale`/`g_NdcOffset`, `SNAP_TO_VIEWPORT_EDGES`
   and `PointSize`. Run it over the 309 shaders unchanged first, with the
   `RR6_RECOMP` path; this measures how far 2099 containers differ from 2135.
2. Check the 2099 containers against the 0x0E layout notes (rr6 plan,
   "Container revision 0x0E"): the VS words `[6..10]`, the definition table and
   the literal register formula. Disassemble a few of the 176-byte PS to
   confirm.
3. Add a `PGR3_RECOMP` define for what is genuinely per-game:
   - the vertex input location table (`*_VERTEX_INPUT_LOCATIONS`), built from
     the semantics PGR3's VS element tables declare; rr6's `static_assert`
     pins RR6's table, so PGR3 needs its own;
   - spec constants, starting with `R11G11B10_NORMAL` and `ALPHA_TEST`;
   - the Blue Dragon `ShadowTexture` PCF rewrite stays off.

   Prefer generalising the fork (shared 0x0E support plus a per-game table)
   over a diverging `pgr3` branch, so fixes flow both ways.
4. Iterate until DXIL and SPIR-V both report **0 failures**, then review the
   HLSL dump (`pgr3_shader_hlsl_dump`) for memexport, dynamic register
   indexing, vertex fetch outside the declaration, and int constants (the
   recompiler emits `#error` on those).

**Exit:** 309/309 compile for both backends.

**Done.** rr6's recompiler, unchanged, crashed on one shader and DXC
rejected 114 of the rest. After the changes below: 0 failures, 309 cache
entries (581 KB of zstd DXIL, 405 KB of SPIR-V). The fork is a local clone of
rr6's submodule (`origin` = github.com/stuken/reblue-XenosRecomp, branch
`pgr3` off `rr6`), built standalone into `out/xenosrecomp` until Phase 3
wires it into the build.

### Phase 2 findings

Fixes to the fork, each tied to the shaders that needed it:

| Problem | Shaders | Change |
|---|---|---|
| Assert: VS export to a register not in the interpolator table | 1 (`vs_820A7608`, VS export mode 2, point sprites) | Export 63 (`VSPointSizeEdgeFlagKillVertex`) goes to a local `oPts` (x = size). The host must expand sprites itself (Phase 5). |
| `cannot jump from switch statement to this case label` | 67 | rr6's co-issue snapshot `float4 sN = rN;` landed inside a `switch (pc)` case; now `float4 sN; sN = rN;`. |
| SPIR-V: partial `vk::location` assignment | 36 | `PGR3_RECOMP` location table in `shader_common.h`: PGR3's 21 vertex semantics at locations 0-20. A missing semantic now emits a named `#error`. |
| `redefinition of parameter` | 6 (`vs_820C1350` etc.) | The element table has one entry per vfetch, and these shaders fetch Position1-3 (and TexCoord) more than once. Each semantic is declared once, and a `#error` fires if two fetches of it disagree in constant, offset, stride or format (none do). |
| `NORMAL0` interpolator undeclared | 1 pair (`vs_820A6980` -> `ps_820A66D0`) | `NORMAL0` appended to the interpolator list under `PGR3_RECOMP`, after the others so their registers do not move. |
| `getWeights1D`, `_Texture1DDescriptorIndex` undeclared | 2 (`ps_820A66D0`, `ps_820BC970`) | 1D fetches are emitted as 2D at `float2(u, 0.5)`, matching Xenos's 1D layout; the host must bind 1D textures as 2D (Phase 5). |

Also:
- The Blue Dragon `ShadowTexture` rewrite is off for PGR3 (`!PGR3_RECOMP`).
- `cond_call`/`return` control flow, which the recompiler skipped silently,
  now emits `#error`. No PGR3 shader uses them.
- DXC errors and the failure list name the shader (`=== ps_... (dxil)`).

Review of the output:
- Spec constants: every PS uses `ALPHA_TEST`, and 70 of 105 VS use
  `R11G11B10_NORMAL`, the same two as RR6.
- No memexport: an export to any register outside the table would have hit
  the same assert as the point-size shader.
- The swap masks (`g_Swapped*`) are 32-bit per-semantic bitfields, so
  PGR3's higher usage indices fit.
- The two container-layout questions from step 2 needed nothing: the 2099
  containers parse with the 2135 code.
- Regression: with `REBLUE_RECOMP RR6_RECOMP`, RR6's 176 shaders still
  compile. Their HLSL differs only in the snapshot declaration (108 lines),
  an unused `oPts` local per VS, and the embedded PGR3 table.

Carried into Phase 5: point-sprite expansion (`oPts`), binding 1D textures as
2D, the PGR3 vertex location table on the host side, and `NORMAL0` in the host
interpolator layout if anything there depends on it.

Scratch tools: `scratch/recomp_report.py` (runs the recompiler and groups DXC
errors by kind) and `scratch/hlsl_diff.py` (compares two HLSL dumps).

### Phase 3: Build integration

1. Copy rr6's `cmake/` files and its `CMakeLists.txt` layout: the
   `PGR3_NATIVE_GPU` option (OFF = xenos, ON = native), `PGR3_D3D12`,
   `PGR3_VULKAN_EXE`, one `pgr3_recomp` OBJECT library shared by `pgr3.exe` and
   `pgr3_vk.exe`, the shader cache OBJECT library, and `pgr3_prelink`.
2. Add the submodules (plume, XenosRecomp, rexglue-sdk) and miniz/zstd.
3. In `Pgr3App`: `config.graphics = nullptr` under `PGR3_NATIVE_GPU`, bring
   plume up in `OnPreLaunchModule`, and call `Settings::Init` in
   `OnPostInitLogging`, which rr6 initially forgot.

**Exit:** both configurations build; the native exe links the unused cache and
the shader set regenerates when the XEX, the patch, the recompiler or
`shader_common.h` changes.

**Done.**

- **Submodules** (`.gitmodules`), each cloned from rr6's local copy with
  `origin` set to the GitHub fork: `thirdparty/XenosRecomp` (branch `pgr3`,
  `cd59cf1`; **not pushed yet**, so a fresh clone cannot fetch it),
  `thirdparty/plume` (`rr6`, `2f9fb4c`, with its four `contrib/`
  submodules), and `thirdparty/rexglue-sdk` (`main`, `c53c48a`, the commit
  rr6 pins and the installed SDK was built from). The SDK checkout is a pin
  only: the build still uses an installed SDK (`CMAKE_PREFIX_PATH`), and its
  nested submodules are not checked out. miniz and zstd are vendored, and
  `licenses/` carries re:Blue's BSD 3-Clause text for the files ported from it.
- **Build files:** `thirdparty/CMakeLists.txt` builds XenosRecomp with
  `REBLUE_RECOMP PGR3_RECOMP`. `cmake/shader_cache.cmake` has
  `pgr3_shader_cache()` for image dump, extraction, recompile,
  `pgr3_shader_cache_gen` and `pgr3_shader_hlsl_dump`. `cmake/native_gpu.cmake`
  and `cmake/shaders.cmake` are rr6's, renamed; the renderer's own HLSL list
  waits for Phase 5. The top-level `CMakeLists.txt` splits sources into those
  every build shares, the xenos-only ones (census, resolve dump) and the
  native ones (`src/gpu/**`).
- **Shader pipeline in every build:** `pgr3_shader_cache` (the generated
  `shader_cache.cpp`, 309 entries) compiles in the xenos build too, as rr6's
  does. In the native build `pgr3_prelink` DXC-links every spec-constant subset:
  548 variants (204 PS x 2 alpha-test states, 70 VS x 2 normal-format states),
  6.1 MB of DXIL deflated to 2.2 MB, with no failures.
- **App:** under `PGR3_NATIVE_GPU`, `OnPreSetup` sets `config.graphics =
  nullptr`. `OnPreLaunchModule` logs that the renderer is missing, flushes the
  log and exits with code 1. A queued `QuitFromUIThread` is not enough: the
  SDK resumes the guest's main thread as soon as the hook returns. The
  `Settings::Init` and plume bring-up steps move to Phase 5 with the renderer
  they belong to. `src/gpu/present_count_stub.cpp` stands in for the
  renderer's present counter, which `pgr3_input_script` reads.
- **Regeneration checked:** touching `shader_common.h` reruns the recompile;
  touching `default.xexp` reruns the image dump, extraction and recompile.

Building the native configuration (its own build directory, so the xenos
build is kept):

```bat
cmake --preset win-amd64-relwithdebinfo -B out/build/native-relwithdebinfo ^
  -DPGR3_NATIVE_GPU=ON -DCMAKE_PREFIX_PATH=<sdk install>
cmake --build out/build/native-relwithdebinfo
```

### Phase 4: Identify XDK 2099 D3D

Use rr6's method: find functions by behaviour in the codegen output
(`scripts/re/funcdb.py`), not by signature.

1. **Anchors:**
   - kernel imports: `VdSwap` (`D3DDevice_Swap`, then `Present`),
     `VdInitializeEngines`/`VdInitializeRingBuffer` (`Direct3D_CreateDevice`,
     `SetRingBufferParameters`), `VdGlobalDevice`, `VdGetCurrentDisplayGamma`,
     `MmAllocatePhysicalMemoryEx`/`MmFreePhysicalMemory` (`XPhysicalAlloc`/`Free`);
   - the 332 shader container VAs (`shaders.csv`): their references reach the
     loader and `D3DDevice_Create{Vertex,Pixel}Shader`. Find out whether PGR3,
     like RR6, creates everything from one table at boot, or per material as
     content loads;
   - shape matching against rr6's `config/d3d.toml` (77 names). 2099 and 2135
     should share most function bodies.
2. Write `config/d3d.toml` and `config/xapi.toml` and include them from
   `pgr3_manifest.toml`. Re-run codegen and check every name applies without
   warnings.
3. Re-derive the **`D3DDevice` layout** (the header comment in rr6's
   `d3d.toml` is the checklist): allocation size, dirty masks, render- and
   sampler-state dispatch tables, fetch constants, VS/PS float constants,
   register shadows (`RB_DEPTHCONTROL`, `RB_BLENDCONTROL0`, `RB_COLORCONTROL`,
   `PA_SU_SC_MODE_CNTL`, `RB_COLOR_MASK`, `RB_STENCILREFMASK`, `RB_ALPHA_REF`,
   `PA_CL_CLIP_CNTL`, `PA_CL_VTE_CNTL`, `PA_SU_VTX_CNTL`, `PA_SU_POLY_OFFSET_*`,
   window scissor), the viewport, clip planes, streams, textures, render
   targets, depth, and shaders.
4. Re-derive the resource headers (`D3DResource`, VB, IB, texture, surface)
   from the creators, `GetDesc` and the lock functions, then check them against
   live data with a destroy log, as rr6 did.
5. List the exports PGR3 does call that RR6 did not, especially
   `BeginTiling`/`EndTiling` (predicated tiling), multisample surface creation,
   queries, `DrawIndexedVerticesUP` and the gamma ramp. Each becomes a hook or a
   documented gap.

**Exit:** a PGR3 equivalent of rr6's hook table (rr6 plan, Phase 3 findings),
every address named, and `gpu/d3d.h` rewritten for 2099.

**Done.**

### Phase 4 findings

**The hook table.** Everything rr6's renderer hooks or calls has a PGR3
address:

| rr6 | PGR3 |
|---|---|
| `Direct3D_CreateDevice` | `0x823D59F8` (allocates 0x5080, as in 2135) |
| `D3DDevice_Present` | **none**: hook `D3DDevice_Swap` `0x823CADB8`; the game resolves the front buffer itself first |
| `D3DDevice_Clear` (Stencil in r9) / `ClearF` | `0x823D5818` / `0x823D5158`, the same register use |
| `D3DDevice_Resolve` | `0x823CEF68`, the same 416-byte frame and stack arguments (`508(r1)`, `516(r1)`) |
| `Draw{Vertices,IndexedVertices,VerticesUP}`, `Begin/EndVertices` | `0x823D3D40`, `0x823D3F80`, `0x823D3CB8`, `0x823D3A18` / `0x823D3C98` |
| `Create{Vertex,Pixel}Shader`, `CreateVertexDeclaration` | `0x823CA900`, `0x823CA810`, `0x823CA5E0` |
| `Set{Vertex,Pixel}Shader`, `SetVertexDeclaration` | `0x823CA578`, `0x823CA208`, `0x823CA9D8` |
| `Set{RenderTarget,DepthStencilSurface}` | `0x823CE288`, `0x823CE620` |
| `D3DResource_Destroy`, `D3DSurface_GetDesc` | `0x823D4338`, `0x823D37A8` |
| `D3D__MakeSpace`, `D3D__AllocCommandMemory` | `0x823D1198`, `0x823D04E8`; allocator callback at device `+0x3524` (2135: `+0x3534`) |
| `XPhysicalAlloc` / `XPhysicalFree` | `0x823C5370` / `0x823C53F8` |
| `XInputGetState` | `0x823C5600` |
| `VdGlobalDevice` import | `0x820005A0` in the patched image (`0x8200059C` unpatched) |

**How the layout was checked.** `scripts/re/state_setters.py` pairs the 97
render-state and 20 sampler-state setters by state index, through the
`{Get, Set, Default}` tables at `0x825271F0`/`0x82527680` (rr6:
`0x823763E0`/`0x82376870`). The enums are the API's, so index *k* is the same
state in both. Every pair matches at shape ratio 1.00 apart from D3DRS 78-81,
and every device field they touch, `0x2C90`-`0x2F6C`, is at the same offset.
That covers all the register shadows rr6 reads. `scratch/pair_diff.py` did the
same for the named functions. `scripts/re/offset_map.py` (a free shape match
over the whole library) was too noisy to rely on.

**What differs from 2135:**
- Up to the pixel shader (`+0x3290`), nothing.
- `+0x32AC`-`+0x3544`: the tiling and command state. `BeginTiling` keeps its
  tile count at `+0x32AC` and its rects from `+0x32B0`; 2135's `ClearF` and
  `SetRenderTarget` compare more render-target fields there. The command
  fields move by -0x10, and some by -4 or -0x14.
- The vertex shader is at `+0x4FD8` and the behaviour flags at `+0x5050`
  (2135: `+0x4FE8`, `+0x5060`).
- D3DRS 78-81 and `SetRenderTarget` have 2099-only tails behind a global at
  `0x829496EC`, which is zero in the image and written by nothing, so they
  never run.

**Resources.** The creators and accessors (`CreateTexture`, `CreateSurface`,
`GetDesc`, `Destroy`, `AddRef`/`Release`, the locks) match 2135's at 1.00.
`D3DDevice_CreateVertexBuffer`/`IndexBuffer` are not linked: PGR3 builds its
own buffer headers, and `kCommonD3DCreated` is clear on every live one. A
frame trace decodes the live headers with 2135's layout: VB `Common`
`0x40090001` (kind 1, cached memory) with fetch type 3 at `+12`, IB
`0x400A0001` (kind 2, 16-bit indices). PGR3 also overrides `XMemAlloc` and
`XMemFree` (`sub_8219A778`/`sub_8219A170`).

**Render targets in a race frame** (`pgr3_trace_frame`, now decoding
`RB_SURFACE_INFO` and `RB_COLOR_INFO`):

| Surface | EDRAM pitch | MSAA | Format | Use |
|---|---|---|---|---|
| `0x40008000` | 1040 | 2x | 3 `2_10_10_10_FLOAT` (7e3), sometimes 12 (`7e3_AS_16_16_16_16`) | race scene |
| `0x40008050` | 1040 | 2x | `D24FS8` | race depth, at EDRAM tile 975 |
| `0x400E4B60` | 280 | 4x | 12 | environment passes |
| `0x4009F140` | 280 | 4x | 0 (8888) | probably the 256x256 minimap |
| `0x4009F000`-`F0F0` | 80 | 1x | 7 `16_16_16_16_FLOAT` | luminance chain |
| `0x400171A0` | 400 | 1x | 0 | bloom |
| `0x400E7170` | 560 | 1x | `D24FS8` | shadow depth |

- 1040 x 600 x 2 samples x 4 bytes is 975 EDRAM tiles of 5 KB, so colour plus
  depth use 1950 of the 2048 tiles: no tiling needed, as reported. The 64bpp
  FP16 data exists only in the resolve targets.
- **`BeginTiling`/`EndTiling` are not called in races.** They are reached only
  through `sub_821D78A8` and `sub_8229B078` (perhaps photo mode or captures);
  their trace hooks log tile rects when they run.

**What this adds to Phase 5:**
- 2x and 4x MSAA targets with MSAA-to-1x resolves (re:Blue's resolve
  shaders).
- 7e3 EDRAM colour on the host: an `R16G16B16A16_FLOAT` (or
  `R11G11B10`-class) host target, with resolves into FP16 textures converting
  7e3 -> half and resolves into 8-bit or 7e3 textures converting on the way.
- Float depth (`D24FS8`) resolves, which rr6 never had.
- Predicated tiling, once the path that uses it is found.

### Phase 5: Native renderer bring-up

1. Port the files listed under "What ports from rr6", with the rr6-specific
   passes stubbed out until PGR3's equivalents are known.
2. Bring it up in rr6's order, comparing each step against the Phase 0
   references:
   1. `CreateDevice`, `Clear`, `Present`, and the ring and command memory
      turned into discard buffers (RR6's two hangs were the ring's hard end and
      the allocator callback);
   2. shader creation with the hit rate logged (target 309/309);
   3. the first 2D draws (front end);
   4. textures, including XPR-loaded and cube textures, and the Bink planes;
   5. render targets and `Resolve`, including partial resolves;
   6. 3D course geometry;
   7. post effects, HUD, and the display correction curve.
3. Model these PGR3-specific paths when the trace shows them:
   - **MSAA:** multisampled host targets at the sample count the guest asks
     for, MSAA-to-single-sample resolves (restore re:Blue's resolve shaders),
     and how resolution scaling interacts with them;
   - **7e3 render targets:** an `R16G16B16A16_FLOAT` host target, with resolves
     into 7e3 textures converted on the way, as xenia does;
   - **predicated tiling**, if the game uses it: draw the whole frame once
     (there is no EDRAM limit on the host) and treat each tile's resolve as a
     region of one resolve.
4. Carry over rr6's `PGR3_WARN_FIRST` warnings for unmodeled paths (clear
   rects, depth resolves, MRT, lines), and model a path only when a warning
   shows PGR3 reaching it.
5. Lifetime: hook `D3DResource_Destroy` and `XPhysicalAlloc`/`XPhysicalFree`.
   Check a course change in both directions, since streaming makes stale
   mirrors more likely here than in RR6.

**Exit:** the front end, garage, and a race in every city (day and night),
replays and movies match the xenos references at 1x.

### Phase 5 progress

**The port.** rr6's `src/gpu`, `src/core` and `src/platform` came over with
identifiers and the project renamed, and the word "RR6" kept where it names the
game, so rr6's notes stay true as history. Fixes to the port itself:
- **Addresses:** every RR6 address replaced from `gpu/d3d.h` and
  `config/d3d.toml`.
- **`Swap` instead of `Present`:** `D3DDevice_Swap` replaces
  `D3DDevice_Present` and presents the front buffer's resolve target.
- **Physical allocators:** `XPhysicalAlloc`/`XPhysicalFree` replace rr6's
  unnamed functions.
- **RR6- and BD-specific bits disabled:** the RR6-hash-keyed passes (race
  resolve fit, FXAA before grading) match nothing, the precache list starts
  empty, and Blue Dragon's render-pass global reads 0.
- **Location table:** the vertex declaration's `static_assert` pins PGR3's
  table.

**XDK 2099 API that RR6 never used**, now handled natively:
- **Predicated tiling** (`BeginTiling`/`EndTiling`) is used in the front end
  (the run crashed in the guest's tiling ring code). `BeginTiling` becomes the
  first tile's clear, `EndTiling` one whole-surface resolve. The tile count
  stays 0, so the rest of D3D acts untiled.
- **Queries** (`D3DDevice_CreateQuery`, `D3DQuery_Issue`/`GetData`). The
  guest creates the objects; the device's query-slot pool head (`+0x4FDC`)
  is seeded as `D3DDevice_Initialize` leaves it, since a zeroed head handed out
  slots at a null pointer. `Issue` is a no-op, and `GetData` answers at once:
  events done, occlusion 2^20 samples. That turns off occlusion culling;
  real host queries are open.

**Formats PGR3 uses that reblue and rr6 did not:**
- **7e3 back buffer:** `0x1A22ABBF` (`k_2_10_10_10_FLOAT_EDRAM`) is now
  mapped. `ConvertGuestFormat` also falls back to decoding the texture-format
  field.
- **Vertex types:**
  - DEC3N/DEC4N normals: new `SPEC_CONSTANT_DEC3N_NORMAL`, 688 prelinked
    variants.
  - The 11:11:10 texcoord family (UHEND3N/UHEND3 and the signed forms):
    `unpackUhend3n`, with three masks in shared constants `c21.w`/`c29.z`/`c29.w`.
  - **`SHORT4` positions, which are integers**, not normalised. Bound SNORM,
    they had shrunk the whole course and every car onto the scene origin. They
    now bind UINT and sign-extend through bits 16+ of `g_SintTexcoords`.
- **Textures:** `k_16` (as `R16_UNORM`) and `k_5_6_5`, which plume has no
  format for, so it is expanded to RGBA8 on the CPU.

**Bring-up tools added:**
- `pgr3_trace_frame` decodes each draw's declaration and stream 0's header.
- `pgr3_debug_ztest_always` and `pgr3_debug_cull_none` are experiment
  switches.
- Failed pipelines log their description (capped), and repeating warnings are
  capped. The SDK's log rotates at 5 MB into `<name>.1.log`, which had been
  hiding most of each run.
- Test runs use `--pgr3_vsync=false`: with the display occluded (a locked or
  remote session) the vblank clock stops and pacing falls back to a slow
  sleep.

**Vertex pulling** (the instanced fetches). Ten VS compute their fetch index:
`index mod N` for per-vertex data and `index / N` for per-instance data, and
`vs_820C4738` (a fifth of a race frame's indices) also fetches through a
fetched index, so neither the input assembler nor CPU de-instancing can model
them.
- **Shaders:** any vfetch not indexed by `r0.x` marks the shader as pulling
  (exactly those ten). Every fetch goes through a `VFETCH` macro, which pulling
  shaders define as `pullVertex`.
- **`pullVertex`** reads raw dwords from streams 0-3 and decodes them as the
  input assembler would for the format the declaration binds (`PULL_FORMAT_*`),
  so the existing decode wrappers apply unchanged. The streams are root SRVs
  (`t0`-`t3`, `space6`) on D3D12 and device addresses on Vulkan.
- **Shared constants:** a per-location table, built once per declaration, plus
  each stream's size and stride.
- **Vertex index:** every VS now loads the vertex index into `r0.x`, as Xenos
  does. D3D's `SV_VertexID` leaves out the base vertex (Vulkan's includes it),
  so the host passes it in `g_VertexIndexBase`.
- **Fixed along the way:**
  - Relative constant indices are clamped below as well as above. A negative
    `a0` had read before the constant upload and page-faulted the GPU as soon
    as `r0.x` held real indices.
  - Integer TEXCOORDs (`UBYTE4`, `UINT1`) reached the shader as raw bits. Xenos
    converts them to float, so they now pass through `uintTexcoord`; they carry
    the instance-transform index.
  - Pulled `BLENDINDICES` are converted the same way as the `uint4` input.
- **DXC flag:** DXC now gets `-DPGR3_RECOMP` too.
- **Result:** instanced barriers, lamp posts and tyre smoke render, the black
  block in the sky is gone, and the Vulkan exe now reaches the race as well.
- **New switch:** `pgr3_debug_skip_vs` skips draws by VS hash.

**Rect lists** (primitive 8) come only through `BeginVertices`/`DrawVerticesUP`:
two 3-vertex full-screen passes per race frame, with the viewport transform off.
- **The passes:** one on the scene, sampling the scene depth resolve, and the
  bloom downsample to 341x200.
- **Expansion:** each triple is expanded on the CPU to a quad. The fourth
  corner is opposite the right-angle vertex (the vertex off the longest
  edge), computed in input space, which is exact for their pass-through
  vertex shaders. Dwords the three vertices share are copied. The quad then
  takes the quad-list path. Before this, both passes covered only half their
  target.

**Depth resolves** (`D3DRESOLVE_DEPTHSTENCIL`, flags `0x4`/`0x14`) now copy
the depth surface into an `R32_FLOAT` resolve target through the colour-copy
shader. The depth surface is the SRV source: plume views `D32S8`/`D24S8` as
their depth plane.
- **What reads them:** the shaders sample them as `k_24_8_FLOAT`, depth in
  `.x`. A race frame resolves the scene depth (1024x600, read by the
  full-screen rect pass), the cars' 512x512 shadow map, and several 256x256
  targets.
- **Supporting changes:** `R32_FLOAT` joined the render-target whitelist, and
  `pgr3_dump_frames` writes it as grey.

**Missing scenery (fixed).** Against xenos on the same London stretch (the
starting grid and the Embankment), the native race lacked the 3D trees, fences,
the crowd, lane markings and cones, and the buildings beside the road came out
as flat colour blocks. There were three causes:
- **World matrices written into the ring.** `D3DDevice_BeginShaderConstantF4`
  (`sub_823D8E08`, `(device, pixel, start, count)`) reserves a `SET_CONSTANT`
  packet in the ring and returns its payload; the caller writes the float4s
  itself. The device's constant shadow, which the host uploads, never sees
  them. PGR3 sends every scene object's world matrices this way: VS c0-c3
  (`g_mWorld`) from `sub_82249D70`, and c0-c3 plus c64-c67
  (`g_mWeightedWorld`) from `sub_82246730`. `sub_82279B78` sends VS c52+
  and PS c20+ the same way. The shadow kept an identity `g_mWorld`, so the
  scenery VS (`vs_820B25A8`, the roadside walls, and the rest) drew every
  object at the world origin, which the camera saw at NDC (-5.6, -4.1).
  `hooks/constants.cpp` now notes each call, and the next draw copies the
  payloads, still intact in the discard ring, into the shadow.
- **Bool constants.** The host uploaded them as zero, since rr6's shaders had
  none. The device keeps the bool file at `+0x2780` (8 dwords, then 32 loop
  dwords, as the draw-time flush `sub_823DE880` uploads registers
  `0x4900`-`0x4927`). The only writer, `sub_823CA040`, starts at dword
  `0x2790`, so it is `D3DDevice_SetPixelShaderConstantB` (PS b*n* is bit
  128+*n*); nothing sets VS bools. The foliage and building pixel shader
  `ps_820B4100` takes the diffuse map when `g_textureSet_0` (PS b0) is set,
  and otherwise samples the building crop sheet: on the foliage cards that
  gave the grey squares.
- **Alpha to coverage.** The foliage cards have `ALPHA_TO_MASK_ENABLE` set
  (`RB_COLORCONTROL` `0x87000016`) with the alpha test off; on the 2x MSAA
  target that cuts the leaves out. The bit now enables the host's own alpha to
  coverage, which also cuts at one sample. The per-quad dither offsets (bits
  24-31) are not modelled.

Settled along the way:
- **xenos references need `--readback_resolve=full`.** Without it the xenos
  dump tool reads resolve memory that is never written. Use the Phase 4 script
  (`start@1500,a@2100,a@2700..4800,rt@...`): the race loads at about present
  4530 and the countdown runs to about 4900. `pgr3_dump_frames` also decodes
  `k_6_5_5`.
- **The resolve alias lost the crowd impostor atlas (fixed).** The 512x192
  `k_6_5_5` texture `0x40396320` is baked once while the course loads
  (`vs_820C2DB8` with `ps_820BC830`) and decoded by the sprite shader
  `ps_820BC970`. Resolve aliasing skipped the copy, so `pgr3_resolve_alias`
  now defaults to off.
- **The game submits the same scenery in both builds** (`pgr3_scenery_probe`
  on the draw loop `sub_8220D0B8` and its material setup `sub_82208C28`).
  Occlusion queries, fences and CPU reads of depth resolves were ruled out as
  gates.

**Tools:**
- `pgr3_trace_vs` now dumps the records the draw's first indices reach, keyed
  by vertex index (`pgr3_trace_vs_records` indices, for up to
  `pgr3_trace_vs_draws` draws a frame), plus the PS constants and the bool
  file.
- `scratch/vsemu.py` runs flow-control shaders (`switch (pc)`) and reads
  undeclared inputs as the host's zero slot. `scratch/vsemu_survey.py` gives
  each traced draw's NDC extent.
- Other bring-up switches: `pgr3_debug_solid_vs` (a list, cycling every
  `pgr3_debug_solid_cycle` frames), `pgr3_debug_occlusion_samples`,
  `pgr3_debug_fill_depth_resolve`, `scratch/sprites.py`.

**Side-by-side against xenos (London, first lap).** Game time is not tied to
presents, so frames are matched by scene: native present 5800 is xenos 5600
(the Embankment at about 90 mph), native 6280 is xenos 6000 (the chevron
board). `scratch/cmp_sheet.py` builds such sheets, `scratch/tracesum.py`
summarises a frame trace, and `scratch/bmpalpha.py` reads the alpha byte that
both dumpers write (Pillow drops it from a 32-bit BMP). The two builds' pass
lists and resolves agree one for one. Fixed:
- **The minimap** (stack-built `vs_821DA5E0`/`ps_821DAA64`, 256x256) takes a
  colour index and an edge coordinate from SHORT2 `BLENDINDICES`, which had no
  16-bit swap: the road drew black and the player marker lost its colour.
  XenosRecomp's `swapUints` swaps them under bits 16+ of
  `g_SwappedBlendWeights`.
- **Motion blur.** `ps_8200F068`, the full-screen rect pass after the scene and
  depth resolves, reprojects each pixel through `g_blurMatrix` and blurs only
  where the scene's alpha is 1. The opaque scenery masks alpha off
  (`RB_COLOR_MASK` 7), so it keeps the alpha the target starts with. The
  frame-end resolve clears the 7e3 target to (0,0,0,0) (D3D packs alpha 0;
  the xenos trace logs the packed value), but xenos resolves the scene with
  alpha 1 at every pixel, in its EDRAM-exact ROV path too. The shadow and
  environment passes put depth and colour in the scene's EDRAM tiles
  (bases 0 and 224) in between, and depth near 1.0 has the top bits 7e3 reads
  as alpha. The host does not alias EDRAM, so the resolve-clear of a 7e3
  surface clears alpha to 1. The roadside, crowd and buildings now blur as on
  xenos, and the title screen's logo, which blends against destination alpha,
  appears.

Also settled in the comparison:
- **Linear volume textures** (two 64x64x32 `k_8_8_8_8`, one bound as t15 on
  the scenery, and 256x256x4 and 512x512x4 DXT5 on the cars) now mirror: rows
  at the fetch pitch rounded to 32 blocks, slices the height rounded to 32
  block rows apart, as xenia lays out a base level.
- **The garage preview** matches xenos now, and the bloom target is as dark as
  xenos's in daylight (means under 2/255 in both), so neither is a fault.

**MSAA.** Surfaces take the sample count `D3DSurface_GetDesc` reports
(`MultiSampleType` 1 is 2x, 2 is 4x; `pgr3_msaa`, on by default, falls back to
1x where the host lacks the count). In a race: the scene 2x, the environment
passes and the minimap 4x, the post chain 1x. Resolves:
- a colour source goes through a hardware resolve (the sample average) into a
  single-sample scratch texture per size and format, which then feeds the
  existing 1x copies: whole, partial rect, into a cube face, or converted;
- a depth source (the 2x scene depth into its `R32_FLOAT` texture) goes through
  `resolve_msaa_ps`, which reads `Texture2DMS` and takes sample 0.

Alpha to coverage now has the samples to work with on the foliage. Checked on
D3D12 and Vulkan through the menus, garage and race.

**Night races blew out their highlights (fixed).** Las Vegas at night
(`highlight_blowout.rdc`; reached from boot with
`a@2s,a@4s,a@6s,start@11s,a@16s,down@21s,a@23s,a@25s,a@27s,up@29s,a@31s,a@33s,a@35s,start@37s`
plus `rt@45s+200`, since the car fades out without throttle). The scene,
the luminance chain (at the game's 0.1 floor) and the scene alpha (1
everywhere) all matched xenos; the bloom did not:
- **MRT.** At night the bright pass (`ps_8200D0E0`) writes two targets: the
  raw bright pass to RT0 and its compressed glow, `x/(x+k)`, to RT1, which the
  game resolves with `D3DRESOLVE_RENDERTARGET1` (flags `0x1`). The host
  modelled RT0 only, so the bloom took the raw bright pass, the whole car
  body included. `SetRenderTarget` slot 1 is now a second colour attachment
  (no depth beside it), with its own write mask (`RB_COLOR_MASK` bits 4-7)
  and RT0's blend, and resolves pick the target by flags bits 0-1.
- **Resolve destination formats.** A resolve target took its source surface's
  format, so the 7e3 scene resolved into PGR3's `k_8_8_8_8` bloom and front
  buffer textures as FP16, keeping values over 1 that Xenos clamps. Such a
  destination is now `R8G8B8A8_UNORM`, and a partial resolve that converts
  formats runs the copy shader with the source's extent placed under a
  scissor, instead of failing over to a whole-surface copy.

**Open, in rough order of what is visible in a race:**
1. **The other cities and night races,** replays and movies, against xenos.
   These need menu scripts.
2. The title screen's logo rings are dimmer than on xenos.
3. The Vulkan exe has had no validation-layer pass.

The default resolution scale (2x, a 2048x1200 scene) runs with MSAA on and
looks right in a race; the per-pass checks at 2x and 3x are Phase 6 work.

### Phase 6: Hardening and performance

Mostly carry-over work from rr6's Phase 5:

- resolution scale and supersampling, re-checking the post chain at 2x and 3x
  for the banding and edge problems rr6 hit (glow buffer scale, resolve
  pattern, viewport edge snap); the fixes are per pass, so they must be found
  again for PGR3's passes;
- the pipeline precache, captured from scratch (`pgr3_pso_capture`), with
  dynamic depth bias where the runtime has it;
- display-locked frame pacing, perf counters, the Vulkan exe and the Vulkan
  validation pass;
- optional: FXAA before the HUD, 16-bit output, texture replacement and
  dumping, anisotropy override;
- Linux, as rr6 has it (written, not yet built).

### Phase 6 progress

**Per-draw CPU cost.** `pgr3_perf_stats=300` now splits `draw` into its stages
(geometry, framebuffer, flush, and inside the flush the pipeline, constants
and shared constants). A London race at the default 2x scale was CPU-bound:
3,600 draws a frame took 13-16 ms, 8.6 ms of it binding streams and textures,
and the GPU never made the CPU wait. The binding took a lock per vertex stream
slot (all 16, every draw) and four or five per bound texture. Now:
- `Video::SetVertexStreams` and `Video::SetTextures` bind all slots under one
  lock;
- `ResolveOrMirrorGuestTexture` answers repeats from a per-thread cache, keyed
  by the texture's fetch words, the frame and a generation that every mirror
  or resolve target creation, rebuild or eviction bumps on entry and exit;
- `GuestMirrorFrame` is an atomic read.

Draws now cost 6-7 ms a frame (geometry 3 ms), and the race holds 60 fps
with 4-5 ms of the frame spent in the pacing sleep. No pipeline builds happen
mid-race once the course has loaded.

### Phase 7: Geometry Wars (`gw.xex`)

`gw.xex` links its own D3D, so with `config.graphics = nullptr` it would get no
GPU at all.

1. Find out how PGR3 starts it (`XLaunchNewImage` vs `XexLoadImage`) and
   whether the SDK can recompile it as a second module (`dll_targets.cmake`).
2. Options, in order of cost: leave it unsupported in the native build; run it
   in a xenos-plugin build; or identify its D3D (same XDK as the main game, so
   much of `config/d3d.toml` should match by shape) and route it through the
   same renderer with its own hook set and shader cache.

---

## Risks and open questions

1. **MSAA and 7e3 are new ground.** RR6 used neither, so this is where the port
   stops being a port. Phase 4 confirmed both: 2x MSAA 7e3 for the race scene
   and 4x MSAA for the environment passes, plus float depth resolves.
2. **XDK 2099 vs 2135.** Settled by Phase 4 for everything the renderer
   touches; the tiling state is the one region still only partly mapped.
   Originally: container layout, device offsets and register-setter
   ABI quirks (like RR6's `Clear` taking Stencil in r9) all have to be checked.
   Phase 2 step 1 is the cheap early read on how far they differ.
3. **Runtime shader creation from streamed content.** Largely settled by Phase
   1: 309 shaders (305 in the image, 4 stack-built), all seen at runtime, and
   no container in the uncompressed data. Modes not yet run (replays, online,
   other cities) could still create one from a compressed archive; the cache
   miss warning will show it.
4. **The image source.** The build extractor, codegen and runtime must all see
   the patched image. A unit check (extracted count and hashes vs the runtime
   census) should fail the build if they drift.
5. **Scale of the port.** rr6's renderer is about 20k lines, and most of the
   cost was in game-specific findings, not the copy. Phases 0-3 are cheap and
   leave the xenos build untouched, so they should be done before committing
   to Phase 5.
