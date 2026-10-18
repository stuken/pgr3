// pgr3 - ReXGlue Recompiled Project
//
// pgr3_shader_census: checks that the build's shader set (extracted from the
// patched image, generated/shaders) is every shader the game creates. Each
// distinct container passed to D3DDevice_Create{Vertex,Pixel}Shader is written
// once to <user>/shader_census/ under the extractor's names
// (vs_<VA>.vso / ps_<VA>.pso), so scripts/check_shader_census.py can compare
// the two directories byte for byte. A container from outside the XEX image
// (one loaded from game data at runtime) is logged as a warning; none is
// expected, since all 332 references sit in the image.

#include <cstdio>
#include <filesystem>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <utility>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_BOOL(pgr3_shader_census, false, kCvarGroup,
                    "Write every shader container the game creates to "
                    "shader_census/ in the user data folder");

REX_EXTERN(__imp__D3DDevice_CreateVertexShader);
REX_EXTERN(__imp__D3DDevice_CreatePixelShader);

namespace {

std::mutex g_mutex;
std::set<std::pair<u32, std::string>> g_seen; // (VA, bytes)
u32 g_counts[2] = {};                         // distinct ps, vs
u32 g_outside = 0;

// Return addresses up the guest stack: each frame's back chain is at 0(r1)
// and the saved LR at -8 from the caller's frame (stw r12,-8(r1) prologues).
std::string CallChain(u32 sp) {
  std::string out;
  for (int i = 0; i < 8 && sp; ++i) {
    // Checked reads: the outermost frame's back chain is the stack's top,
    // which is not mapped.
    const u32 next = pgr3::mem::try_load<u32>(sp);
    if (next <= sp || next - sp > 0x10000)
      break;
    const u32 saved = pgr3::mem::try_load<u32>(next - 8);
    if (!saved)
      break;
    out += std::format(" 0x{:08X}", saved);
    sp = next;
  }
  return out;
}

std::pair<u32, u32> ImageRange() {
  auto *ks = rex::system::kernel_state();
  auto module = ks ? ks->GetExecutableModule() : nullptr;
  auto *xex = module ? module->xex_module() : nullptr;
  if (!xex)
    return {0, 0};
  return {xex->base_address(), xex->base_address() + xex->image_size()};
}

void Record(u32 va, bool vertex, u32 caller, u32 sp) {
  if (!REXCVAR_GET(pgr3_shader_census))
    return;
  const auto *header = pgr3::mem::at<const be_u32>(va);
  if (!header)
    return;
  const u32 size = u32(header[1]) + u32(header[2]); // virtualSize + physicalSize
  const auto *bytes = pgr3::mem::at<const char>(va);
  if (!bytes || size == 0 || size > 0x40000)
    return;
  std::string blob(bytes, size);

  std::lock_guard lock(g_mutex);
  if (!g_seen.emplace(va, blob).second)
    return;
  const u32 n = ++g_counts[vertex];
  static const auto range = ImageRange();
  const bool outside = va < range.first || va >= range.second;
  if (outside) {
    ++g_outside;
    PGR3_WARN("pgr3_shader_census: {} at 0x{:08X} ({} bytes) is outside the image, "
              "created from lr=0x{:08X}, chain{}",
              vertex ? "VS" : "PS", va, size, caller, CallChain(sp));
  }
  auto dir = rex::Runtime::instance()->user_data_root() / "shader_census";
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  // A second container at a VA already used (only possible outside the
  // image) gets a suffix rather than overwriting the first.
  std::string name = std::format("{}_{:08X}", vertex ? "vs" : "ps", va);
  if (std::filesystem::exists(dir / (name + (vertex ? ".vso" : ".pso"))))
    name += std::format("_{}", n);
  if (FILE *f = std::fopen((dir / (name + (vertex ? ".vso" : ".pso"))).string().c_str(), "wb")) {
    std::fwrite(blob.data(), 1, blob.size(), f);
    std::fclose(f);
  }
  PGR3_DEBUG("pgr3_shader_census: {} 0x{:08X}", vertex ? "VS" : "PS", va);
  if ((g_counts[0] + g_counts[1]) % 50 == 0)
    PGR3_INFO("pgr3_shader_census: {} VS, {} PS distinct so far, {} outside the image",
              g_counts[1], g_counts[0], g_outside);
}

} // namespace

// (pFunction) -> D3DVertexShader*.
REX_HOOK_RAW(D3DDevice_CreateVertexShader) {
  const u32 container = ctx.r3.u32;
  const u32 caller = u32(ctx.lr), sp = ctx.r1.u32;
  __imp__D3DDevice_CreateVertexShader(ctx, base);
  Record(container, true, caller, sp);
}

// (pFunction) -> D3DPixelShader*.
REX_HOOK_RAW(D3DDevice_CreatePixelShader) {
  const u32 container = ctx.r3.u32;
  const u32 caller = u32(ctx.lr), sp = ctx.r1.u32;
  __imp__D3DDevice_CreatePixelShader(ctx, base);
  Record(container, false, caller, sp);
}
