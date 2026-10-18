// pgr3 - ReXGlue Recompiled Project
//
// Device lifecycle hooks for the native renderer: Direct3D_CreateDevice builds
// the guest D3DDevice while plume owns the real swapchain, Clear and Swap
// drive the host frame, and D3D__MakeSpace turns the guest command ring into a
// discard buffer. Modelled on reblue's gpu/hooks/device.cpp (Copyright (c) 2026
// Tom Clay, BSD 3-Clause) by way of rr6, against XDK 2.0.2099's layout
// (gpu/d3d.h, config/d3d.toml).

#include <atomic>

#include <rex/hook.h>
#include <rex/system/xmemory.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "gpu/d3d.h"
#include "gpu/device.h"
#include "gpu/native_texture_mirror.h"

namespace pgr3::gpu {
void NoteDefaultSurfaces(u32 back_buffer, u32 depth);
void AdvanceGuestBufferFrame();
void LogDrawStats(u32 frame);
bool TraceFrameActive();
} // namespace pgr3::gpu

namespace {

// Nothing consumes the guest's PM4 ring: every guest D3D function still
// running unhooked writes its packets here and D3D__MakeSpace rewinds them.
// The limit leaves half the buffer as headroom for the largest single batch
// written between two limit checks.
constexpr u32 kDiscardRingSize = 4u << 20;
constexpr u32 kDiscardRingLimit = kDiscardRingSize / 2;
u32 g_discard_ring = 0;

// Command memory D3D__AllocCommandMemory hands out (inline vertex data,
// clear and resolve constants), bump-allocated and wrapped. The guest only
// writes it, so reuse after a wrap is harmless while nothing reads it back.
constexpr u32 kDiscardCommandSize = 8u << 20;
u32 g_discard_command = 0;
u32 g_discard_command_used = 0;

REX_IMPORT(__imp__D3D__InitializeStateTables, InitializeStateTables,
           void(u32));
REX_IMPORT(__imp__D3DDevice_CreateSurface, CreateSurface,
           u32(u32, u32, u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetRenderTarget, SetRenderTarget,
           void(u32, u32, u32));
REX_IMPORT(__imp__D3DDevice_SetDepthStencilSurface, SetDepthStencilSurface,
           void(u32, u32));

// D3DPRESENT_PARAMETERS, the fields the device's default surfaces need.
struct PresentParameters {
  be_u32 BackBufferWidth;        // +0x00
  be_u32 BackBufferHeight;       // +0x04
  be_u32 BackBufferFormat;       // +0x08
  be_u32 BackBufferCount;        // +0x0C
  be_u32 MultiSampleType;        // +0x10
  be_u32 MultiSampleQuality;     // +0x14
  be_u32 SwapEffect;             // +0x18
  be_u32 hDeviceWindow;          // +0x1C
  be_u32 Windowed;               // +0x20
  be_u32 EnableAutoDepthStencil; // +0x24
  be_u32 AutoDepthStencilFormat; // +0x28
};

// The real D3DDevice_Initialize binds a back buffer at RT0 (and a depth
// buffer when asked) before returning, and games read RT0 back (RR6 does
// straight away). The guest's own CreateSurface/Set* keep its bookkeeping
// consistent; the device holds the creation references.
void BindDefaultSurfaces(u32 device_va, u32 params_va) {
  const auto *pp = pgr3::mem::at<const PresentParameters>(params_va);
  if (!pp)
    return;
  const u32 width = pp->BackBufferWidth ? u32(pp->BackBufferWidth) : 1280u;
  const u32 height = pp->BackBufferHeight ? u32(pp->BackBufferHeight) : 720u;
  const u32 back = CreateSurface(width, height, pp->BackBufferFormat,
                                 pp->MultiSampleType, 0);
  if (back)
    SetRenderTarget(device_va, 0, back);
  u32 depth = 0;
  if (pp->EnableAutoDepthStencil) {
    depth = CreateSurface(width, height, pp->AutoDepthStencilFormat,
                          pp->MultiSampleType, 0);
    if (depth)
      SetDepthStencilSurface(device_va, depth);
  }
  pgr3::gpu::NoteDefaultSurfaces(back, depth);
  PGR3_INFO("Direct3D_CreateDevice: {}x{} back buffer 0x{:08X} (format "
           "0x{:08X}), depth 0x{:08X} (format 0x{:08X})",
           width, height, back, u32(pp->BackBufferFormat), depth,
           u32(pp->AutoDepthStencilFormat));
}

u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 /*device_type*/,
                               u32 /*focus_window*/, u32 behavior_flags,
                               u32 present_params, mapped_u32 out_device) {
  auto *memory = REX_KERNEL_MEMORY();
  const u32 device_va =
      memory->SystemHeapAlloc(pgr3::gpu::kD3DDeviceAllocSize, 0x80);
  if (!g_discard_ring)
    g_discard_ring = memory->SystemHeapAlloc(kDiscardRingSize, 0x1000);
  if (!g_discard_command)
    g_discard_command = memory->SystemHeapAlloc(kDiscardCommandSize, 0x1000);
  if (!device_va || !g_discard_ring || !g_discard_command) {
    PGR3_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return 0x8007000E; // E_OUTOFMEMORY, as the guest returns
  }
  memory->Zero(device_va, pgr3::gpu::kD3DDeviceAllocSize);

  auto *device = pgr3::mem::at<pgr3::gpu::D3DDevice>(device_va);
  device->m_pRing = g_discard_ring;
  device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  device->m_pRingLimit = g_discard_ring + kDiscardRingLimit;
  device->m_ReferenceCount = 1;
  device->behaviorFlags = behavior_flags;
  // The query-slot pool head as D3DDevice_Initialize leaves it: full, so the
  // guest's own CreateQuery allocates a page for its first query.
  if (auto *pool = pgr3::mem::at<be_u32>(device_va + pgr3::gpu::kDeviceQuerySlotPool)) {
    pool[0] = 0xFFFFFFFFu;
    pool[1] = 0xFFFFFFFFu;
    pool[2] = 0;
    pool[3] = 0;
  }

  if (auto *global = pgr3::mem::at<const be_u32>(pgr3::gpu::kVdGlobalDeviceImport)) {
    if (auto *slot = pgr3::mem::at<be_u32>(global->get()))
      *slot = device_va;
  }

  // The guest's own seeding: copies both dispatch tables in, calls every Set
  // with its default so the register shadows start valid, and clears all
  // texture slots. Its packets land in the discard ring.
  InitializeStateTables(device_va);
  BindDefaultSurfaces(device_va, present_params);

  device->viewport.Width = 1280;
  device->viewport.Height = 720;
  device->viewport.MaxZ = 1.0f;

  if (out_device)
    *out_device = device_va;
  PGR3_INFO("Direct3D_CreateDevice: guest device at 0x{:08X}", device_va);
  return 0;
}

// The Xenon ABI reserves a GPR slot for every float argument, so Z in f1 also
// consumes r8 while SDK marshaling numbers integer args without skipping it.
// The placeholder absorbs r8 so Stencil reads r9 (D3DDevice_Clear, 0x823D5818,
// passes r9 on to ClearF as 2135's does).
u32 D3DDevice_Clear_hook(u32 /*device*/, u32 count, u32 rects, u32 flags,
                         u32 color, f64 z, u32 /*z_gpr_slot*/, u32 stencil) {
  if (pgr3::gpu::TraceFrameActive())
    PGR3_INFO("[trace] Clear flags=0x{:X} color=0x{:08X} z={}", flags, color, z);
  // D3DRECT {x1,y1,x2,y2}. Rects aren't modeled: the whole target clears.
  if (count && rects) {
    if (const auto *r = pgr3::mem::at<const be_i32>(rects))
      PGR3_WARN_FIRST(4, "Clear: {} rect(s) ignored, first {},{}-{},{} "
                     "flags=0x{:X}", count, i32(r[0]), i32(r[1]), i32(r[2]),
                     i32(r[3]), flags);
  }
  pgr3::gpu::Video::Clear(flags, color, static_cast<float>(z), stencil);
  return 0;
}

// The per-frame entry. XDK 2099 has no D3DDevice_Present: the game's frame end
// (sub_821507A0) resolves the back buffer into the front buffer texture itself
// and calls Swap with it, so the host presents that resolve target (or the
// back buffer, when that was drawn last; Video::Present picks). Plume owns the
// swapchain. The guest's Swap would also hand D3D's deferred physical frees
// to MmFreePhysicalMemory; rr6's Present hook skipped them the same way.
std::atomic<u32> g_presents{0};

u32 D3DDevice_Swap_hook(u32 /*device*/, u32 front_buffer) {
  const u32 s_presents = g_presents.fetch_add(1, std::memory_order_relaxed) + 1;
  if (s_presents == 1)
    PGR3_INFO("D3DDevice_Swap: first frame (front buffer 0x{:08X})", front_buffer);
  if (s_presents == 60 || (s_presents % 600) == 0)
    pgr3::gpu::LogDrawStats(s_presents);
  pgr3::gpu::Video::Present(pgr3::gpu::FindResolveTarget(front_buffer));
  pgr3::gpu::AdvanceGuestBufferFrame();
  return 0;
}

// Callers compare the ring pointer against the limit and call this when a
// batch would cross it, then write from the returned pointer.
u32 D3D__MakeSpace_hook(rex::MappedPtr<pgr3::gpu::D3DDevice> device) {
  if (device) {
    device->m_pRing = g_discard_ring;
    device->m_pRingEnd = g_discard_ring + kDiscardRingSize;
  }
  return g_discard_ring;
}

// The real allocator asks the callback D3DDevice_Initialize installs at
// device+kDeviceCommandAllocCallback (+0x3524) for space in the next ring
// segment. With no segments to manage, hand out discard memory with the
// requested alignment instead.
u32 D3D__AllocCommandMemory_hook(u32 /*device*/, u32 dwords, u32 align) {
  const u32 bytes = dwords * 4;
  const u32 alignment = align ? align : 4;
  if (bytes > kDiscardCommandSize / 2) {
    PGR3_ERROR("D3D__AllocCommandMemory: {} bytes exceeds the discard buffer",
              bytes);
    return 0;
  }
  u32 offset = (g_discard_command_used + alignment - 1) & ~(alignment - 1);
  if (offset + bytes > kDiscardCommandSize)
    offset = 0;
  g_discard_command_used = offset + bytes;
  return g_discard_command + offset;
}

} // namespace

namespace pgr3::gpu {

// Frames the guest has presented, for tools that script against frame
// numbers (platform/input_script.cpp).
u32 PresentCount() { return g_presents.load(std::memory_order_relaxed); }

} // namespace pgr3::gpu

REX_HOOK(D3D__AllocCommandMemory, D3D__AllocCommandMemory_hook);
REX_HOOK(Direct3D_CreateDevice, Direct3D_CreateDevice_hook);
REX_HOOK(D3DDevice_Clear, D3DDevice_Clear_hook);
REX_HOOK(D3DDevice_Swap, D3DDevice_Swap_hook);
REX_HOOK(D3D__MakeSpace, D3D__MakeSpace_hook);
