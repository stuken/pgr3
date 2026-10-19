// pgr3 - ReXGlue Recompiled Project
//
// Shader constants the game writes straight into the command buffer.
// D3DDevice_BeginShaderConstantF4 (sub_823D8E08) reserves a SET_CONSTANT
// packet in the ring and returns a pointer to its payload; the caller then
// writes Vector4fCount float4s there itself. The device's constant shadow
// (D3DDevice::vsFloatConstants/psFloatConstants), which the host uploads each
// draw, never sees them. PGR3 sends every scene object's world matrices this
// way (VS c0-c3 and c64-c67, from sub_82249D70 and sub_82246730), and
// instance data (VS c52.., PS c20.., from sub_82279B78).
//
// The ring is a discard buffer here (hooks/device.cpp) that only rewinds when
// D3D crosses its limit, so the payload is still intact later: each call is
// noted, and the payloads are copied into the shadow in call order at the
// next draw, the next BeginShaderConstantF4, or the next
// Set{Vertex,Pixel}ShaderConstantF, whichever comes first. The last keeps
// command order: PGR3 sometimes writes a car's world matrix (VS c0-c3) into
// the ring and then sets c0-c3 back to the identity before drawing scenery.
// Applied at the draw instead, the car's matrix won, and the scenery's fog
// distance, taken from that world matrix, washed parts of Tokyo out.

#include <algorithm>
#include <mutex>
#include <vector>

#include <rex/hook.h>
#include <rex/types.h>

#include "core/memory_helpers.h"
#include "gpu/d3d.h"

REX_EXTERN(__imp__D3DDevice_BeginShaderConstantF4);
REX_EXTERN(__imp__D3DDevice_SetVertexShaderConstantF);
REX_EXTERN(__imp__D3DDevice_SetPixelShaderConstantF);

namespace pgr3::gpu {
void ApplyGuestWrittenConstants(u32 device_guest);
}

namespace {

struct PendingConstants {
  u32 data;  // guest payload, count float4s
  bool pixel;
  u32 start; // first register of the stage
  u32 count;
};

std::mutex g_mutex;
std::vector<PendingConstants> g_pending;

} // namespace

// (device, pixel shader, start register, Vector4fCount) -> payload pointer,
// or 0 when the ring has no room.
REX_HOOK_RAW(D3DDevice_BeginShaderConstantF4) {
  const bool pixel = ctx.r4.u32 != 0;
  const u32 start = ctx.r5.u32;
  const u32 count = ctx.r6.u32;
  // Earlier payloads were written before this call reserves the next one.
  pgr3::gpu::ApplyGuestWrittenConstants(ctx.r3.u32);
  __imp__D3DDevice_BeginShaderConstantF4(ctx, base);
  const u32 data = ctx.r3.u32;
  if (!data || !count || start >= 256)
    return;
  std::lock_guard lock(g_mutex);
  g_pending.push_back({data, pixel, start, std::min(count, 256 - start)});
}

// (device, start register, float4 *, count): pending payloads land first, so
// this Set overrides them as it would on the GPU.
REX_HOOK_RAW(D3DDevice_SetVertexShaderConstantF) {
  pgr3::gpu::ApplyGuestWrittenConstants(ctx.r3.u32);
  __imp__D3DDevice_SetVertexShaderConstantF(ctx, base);
}

REX_HOOK_RAW(D3DDevice_SetPixelShaderConstantF) {
  pgr3::gpu::ApplyGuestWrittenConstants(ctx.r3.u32);
  __imp__D3DDevice_SetPixelShaderConstantF(ctx, base);
}

namespace pgr3::gpu {

// Called by each draw before it uploads the constant shadow, and by the hooks
// above.
void ApplyGuestWrittenConstants(u32 device_guest) {
  std::lock_guard lock(g_mutex);
  if (g_pending.empty())
    return;
  if (auto *dev = pgr3::mem::at<D3DDevice>(device_guest)) {
    for (const auto &p : g_pending) {
      const auto *src = pgr3::mem::try_at<const be_f32>(p.data);
      if (!src)
        continue;
      auto &dst = p.pixel ? dev->psFloatConstants : dev->vsFloatConstants;
      for (u32 i = 0; i < p.count * 4; ++i)
        dst[p.start + i / 4][i % 4] = src[i];
    }
  }
  g_pending.clear();
}

} // namespace pgr3::gpu
