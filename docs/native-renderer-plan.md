# Native renderer plan for pgr3

Template: `D:\rr6` (Ridge Racer 6), whose native renderer is itself a port of
re:Blue's. rr6's `docs/xenosrecomp-integration-plan.md` records how each piece
was found and fixed; this plan follows the same phases and points at it rather
than repeating it. SDK: rexglue 0.10.0.

---

## Status (2026-10-18)

| Phase | State |
|---|---|
| 0. Baseline and tooling | **In progress.** The xenos build boots into a race under a scripted pad. Ported from rr6: `pgr3_input_script`, `pgr3_null_audio`, `pgr3_dump_frames`, `pgr3_trace_frame`, the abort logger, `scripts/re/funcdb.py`; added `pgr3_dump_raw` and `scripts/re/match_names.py`. See Phase 0 findings. |
| 1. Shader extraction | **Done.** The `pgr3_shaders` build target extracts **309** shaders from the patched image (`pgr3_dump_image` + `scripts/extract_shaders.py`). The runtime census covers all 309, byte for byte, with nothing missing. See Phase 1 findings. |
| 2. XenosRecomp | **Done.** All 309 shaders compile to DXIL and SPIR-V with `REBLUE_RECOMP PGR3_RECOMP`, on branch `pgr3` of the fork in `thirdparty/XenosRecomp` (uncommitted). RR6's 176 still compile, and their HLSL changes only as intended. See Phase 2 findings. |
| 3. Build integration | Not started. |
| 4. D3D identification (XDK 2099) | **Started early** (Phase 0 needed it): `config/d3d.toml` names 70 functions matched against rr6's, `config/xapi.toml` the XInput pair. The device layout is checked only where the dump and trace tools read it. |
| 5. Native renderer bring-up | Not started. |
| 6. Hardening and performance | Not started. |
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
- **Render setup:** PGR3 is widely reported to render at 1024x600 with 2x MSAA
  (sized to fit EDRAM without predicated tiling) and to use HDR. The Phase 0
  trace confirms 1024x600 and HDR (FP16 resolves, below); MSAA and the EDRAM
  colour format still need the render-target registers traced. If MSAA is
  used, restore re:Blue's MSAA resolves (rr6 removed them in `bb8e5ea`;
  restore from `bb8e5ea^`). If the EDRAM format is `k_2_10_10_10_FLOAT` (7e3),
  it has no host equivalent.
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
   stops being a port. Confirm both from the first trace before planning the
   work in detail.
2. **XDK 2099 vs 2135.** Container layout, device offsets and register-setter
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
