# pgr3

pgr3 rebuilds Project Gotham Racing 3 (Xbox 360) as a native PC program using static recompilation. The game's PowerPC code is translated to C++ with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) and compiled for the host, so the game runs directly instead of inside an emulated console.

> [!IMPORTANT]
> This repository contains nothing from the game. You need the files from your own retail copy of Project Gotham Racing 3, together with its title update, and both building and running need them. Everything derived from the game is produced on your machine at build time and is never committed:
> - the recompiled C++ that codegen generates from `default.xex` (`generated/default/`, gitignored);
> - the 309 Xenos shaders taken from the patched game image, and the DXIL and SPIR-V that XenosRecomp makes from them (inside the build directory);
> - the window icon, also taken from `default.xex` (`src/icon.generated.*`, gitignored).

## Renderers

pgr3 has two renderers. You pick one when you configure the build.

- **xenos plugin** (`PGR3_NATIVE_GPU=OFF`, the default). This is the SDK's Xenia-derived GPU emulation. It is the reference that the native renderer is checked against.
- **Native renderer** (`PGR3_NATIVE_GPU=ON`). This replaces the game's Direct3D library with a renderer built on [plume](https://github.com/renderbag/plume), ported from [rr6](https://github.com/stuken/rr6) and before that from re:Blue. The game's Xenos shaders are recompiled at build time with [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) and embedded in the executable. On Windows this builds two executables from one tree:
  - `pgr3.exe`: Direct3D 12.
  - `pgr3_vk.exe`: Vulkan.

  It renders every city, day and night, at a raised resolution with the game's own MSAA (2x for the race, 4x for reflections), holds 60 fps in races, and compiles its pipelines in the background from a precache.

`docs/native-renderer-plan.md` tracks how the native renderer was built, what was found along the way, and what is still open.

## Platform status

| Platform | State |
|---|---|
| Windows x64 | Builds and runs, with both renderers and both native backends. |
| Linux x64 | CMake support is carried over from rr6 but has not been built yet. It would use the Vulkan backend. |
| macOS | Not ported. |

Geometry Wars (`gw.xex`, the arcade game in the garage) is a separate executable, and the SDK cannot run a second XEX, so it is not supported.

## Building from scratch (Windows)

The build has three stages:

1. Clone pgr3, which brings the ReXGlue SDK with it, and add your game files.
2. Build and install the SDK.
3. Configure, run codegen, and build.

The first build takes a while. Codegen turns the whole executable into C++, and there is a lot of it to compile.

### 1. Prerequisites

- **Visual Studio 2022** (Community is fine) with these components:
  - Desktop development with C++.
  - C++ Clang Compiler for Windows. pgr3 and the SDK build with clang, not MSVC.
  - C++ CMake tools for Windows. This provides CMake and Ninja; standalone CMake 3.25+ and Ninja also work.
  - A Windows SDK. 10.0.26100 or later is recommended: with older headers the D3D12 backend bakes depth bias into pipelines instead of setting it per draw. That path works, but it compiles more pipelines.
- **Git**.
- **Python 3**. The build scripts use only the standard library.

No Vulkan SDK or DirectX Shader Compiler install is needed. XenosRecomp vendors DXC for Windows and Linux.

Run every command below from an **x64 Native Tools Command Prompt for VS 2022**, with VS's clang on `PATH`:

```bat
set "PATH=%VCINSTALLDIR%Tools\Llvm\x64\bin;%PATH%"
clang --version
```

### 2. Clone pgr3 and add the game files

```bat
git clone --recursive <this repository's URL> pgr3
cd pgr3
```

`--recursive` fetches the submodules: `thirdparty/rexglue-sdk`, `thirdparty/plume` and `thirdparty/XenosRecomp`, along with their own dependencies. The SDK's dependencies make this a large download. If you cloned without it, run:

```bat
git submodule update --init --recursive
```

Extract the contents of your Project Gotham Racing 3 disc into `assets/`, using a tool such as extract-xiso, so that you end up with `assets/default.xex` next to the `Game`, `Materials` and `UI` folders. Then put the title update's `default.xexp` next to `default.xex`. `assets/` is gitignored.

The game always runs patched: the SDK applies `default.xexp` when it loads `default.xex`, for codegen, for the shader extraction and at run time. The function addresses in `pgr3_config.toml` and `config/` are specific to this pair of files:

| | |
|---|---|
| Title ID | `4D5307D1` |
| Title update | 0.0.5.1 |
| `default.xex` MD5 | `00142082517511d2ad810720318182d4` |
| `default.xexp` MD5 | `1b405bbca713bae54755502ecca5defa` |

Check your copy against these. A different `default.xex`, or a missing or different `default.xexp`, will not recompile correctly against these configs.

### 3. Build and install the ReXGlue SDK

pgr3 needs SDK version 0.10.0. It is developed against [stuken/rexglue-sdk](https://github.com/stuken/rexglue-sdk), which adds GPU, kernel and XAM fixes on top of upstream, and pins it as `thirdparty/rexglue-sdk`. From the pgr3 folder:

```bat
cd thirdparty\rexglue-sdk
cmake --preset win-amd64
cmake --build out/build/win-amd64 --config RelWithDebInfo --target install
cd ..\..
```

This installs the SDK to `thirdparty\rexglue-sdk\out\install\win-amd64`, which step 4 points pgr3 at. The SDK uses a multi-config generator. Install each configuration you plan to build pgr3 in: run the build command again with `--config Release` or `--config Debug`.

An SDK built elsewhere works too: point `CMAKE_PREFIX_PATH` at its install instead.

### 4. Configure, run codegen, and build

Set `CMAKE_PREFIX_PATH` to the SDK install from step 3. Add `-DPGR3_NATIVE_GPU=ON` for the native renderer, or leave it off for the xenos plugin. Giving each its own build directory keeps both:

```bat
set REXSDK=%CD%\thirdparty\rexglue-sdk\out\install\win-amd64
set BUILD=out\build\native-relwithdebinfo

cmake --preset win-amd64-relwithdebinfo -B %BUILD% -DCMAKE_PREFIX_PATH=%REXSDK% -DPGR3_NATIVE_GPU=ON
cmake --build %BUILD% --target pgr3_codegen
cmake --preset win-amd64-relwithdebinfo -B %BUILD%
cmake --build %BUILD%
```

The commands do the following:

1. **Configure.**
2. **Codegen.** The SDK's `rexglue codegen` recompiles the patched `default.xex` into `generated/default/`, following `pgr3_manifest.toml`.
3. **Reconfigure.** CMake only picks up the generated sources on a configure that runs after they exist. Without this step the first build fails to link. Later builds re-run codegen by themselves when its inputs change, so you need the reconfigure only once per fresh tree.
4. **Build.** This builds everything, including the shader cache:
   - `pgr3_dump_image` loads the game through the SDK, title update applied, and writes the patched image.
   - `scripts/extract_shaders.py` pulls the 309 Xenos shaders out of it, including four the game builds on the stack.
   - XenosRecomp turns them into DXIL and SPIR-V, and `pgr3_prelink` links every spec-constant variant.
   - The result is compiled into the executable.

Debug and Release presets work the same way (`win-amd64-debug`, `win-amd64-release`). The output goes to the build directory, along with the DLLs it needs from the SDK and DXC:

- With `PGR3_NATIVE_GPU=ON`: `pgr3.exe` (D3D12) and `pgr3_vk.exe` (Vulkan).
- With it off: `pgr3.exe`, using the xenos plugin.

These options apply to the native build:

| Option | Default | Effect |
|---|---|---|
| `PGR3_NATIVE_GPU` | `OFF` | Native renderer instead of the xenos plugin. |
| `PGR3_D3D12` | `ON` on Windows | `pgr3` uses D3D12. With `OFF`, `pgr3` itself uses Vulkan. |
| `PGR3_VULKAN_EXE` | `ON` | Also build `pgr3_vk` (Windows). |

## Running

`pgr3.exe` looks for the game in `assets\default.xex` next to itself. Either copy or link your `assets` folder into the build directory, or point the game at it:

```bat
out\build\native-relwithdebinfo\pgr3.exe --game_data_root="C:\path\to\pgr3\assets"
```

Next to the executable, pgr3 uses:

- `pgr3.toml`: the config file. Any cvar can go there, and the same cvar can be given on the command line as `--name=value`.
- `user\`: saves, the gamer profile, frame dumps and texture dumps.
- `cache\`: with `pgr3_pso_capture`, the captured pipeline list.

### Native renderer settings

| Cvar | Default | Effect |
|---|---|---|
| `pgr3_resolution_scale` | `0` | Integer render scale over the game's 1280x720 canvas. `0` picks one from the output height, `1` is native. |
| `pgr3_supersampling` | `1` | `2` renders at twice the resolution scale and downsamples at present. Restart to apply. |
| `pgr3_msaa` | `true` | Render the game's multisampled targets at the sample count it asks for. |
| `pgr3_fxaa` | `false` | FXAA on the tone-mapped scene, before the HUD is drawn. |
| `pgr3_anisotropy` | `default` | `2`, `4`, `8` or `16` forces that anisotropic filtering level on every linearly filtered sampler. |
| `pgr3_output_16bit` | `true` | Keep the back buffer at 16 bits per channel and dither at present. Restart to apply. |
| `pgr3_vsync` | `true` | Vertical sync. |
| `pgr3_fps_limit` | `60` | Frame rate cap, `0` for none. The game is built for 60 or 30 fps. |
| `pgr3_texture_dump` | `false` | Write each texture the game uses to `user\textures\dump` as DDS, once. |
| `pgr3_texture_replace` | `true` | Use DDS files in `user\textures\replace` (any resolution, named by a dump's leading hash) instead of the game's textures. |
| `pgr3_pso_precache` | `true` | Compile the recorded pipelines on background threads at boot. |

### Bring-up and debugging tools

These are for working on the renderer rather than playing.

| Cvar | Effect |
|---|---|
| `pgr3_input_script` | Scripted pad input, `button@frame[+hold],...`; a trailing `s` counts seconds from the first pad poll. |
| `pgr3_null_audio` | Discard audio output, for machines without an audio device. |
| `pgr3_dump_frames` | Write the listed present frames, and every resolve target, as BMPs to `user\`. In the xenos build add `--readback_resolve=full`. |
| `pgr3_trace_frame`, `pgr3_trace_vs` | Log every draw and resolve of a frame, or one vertex shader's draws with their inputs. |
| `pgr3_perf_stats` | Log frame times and per-stage renderer timings every N presents. |
| `pgr3_pso_capture` | Write the precache list plus every pipeline a draw had to compile to `cache\pipeline_state_cache.h`, ready to replace `src\gpu\pipeline\cache\pipeline_state_cache.h`. |

## Repository layout

| Path | Contents |
|---|---|
| `src/` | The app (`main.cpp`, `pgr3_app.h`), the native renderer (`gpu/`), shared support (`core/`) and test tooling (`platform/`). |
| `config/` | Names for the game's statically linked XDK 2099 Direct3D and XAPI functions, used by codegen and the renderer's hooks. |
| `pgr3_config.toml`, `pgr3_manifest.toml` | Codegen configuration. |
| `cmake/` | The native renderer, shader cache and host shader build. |
| `scripts/` | Shader extraction, the icon generator and reverse-engineering helpers (`scripts/re/`). |
| `tools/` | `pgr3_dump_image` and `pgr3_prelink`. |
| `thirdparty/` | The ReXGlue SDK, plume, XenosRecomp, miniz and zstd. |
| `docs/` | The native renderer plan and findings. |

## Licences

The files ported from re:Blue (by way of rr6) carry its BSD 3-Clause licence; see `licenses/reblue-BSD-3-Clause.txt`. The submodules carry their own licences.
