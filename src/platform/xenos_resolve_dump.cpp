// pgr3 - ReXGlue Recompiled Project
//
// xenos plugin build only: pgr3_dump_frames="N,..." writes the destination
// texture of every D3DDevice_Resolve the guest issues during present frame N
// as pgr3_frame_N_r<S>_<VA>_<W>x<H>.bmp, S being the resolve's sequence number
// in the frame, so the two builds' intermediate surfaces can be compared
// directly. PGR3 resolves several steps of a chain into one aliased target
// (its luminance and bloom chains share bases), so each resolve is dumped as
// soon as it lands: the hook inserts a fence after it and blocks on it, which
// with readback on means the texels are in guest memory. That stalls the GPU,
// so only the dumped frames pay for it. 32bpp unorm, k_16_16_FLOAT and
// k_16_16_16_16_FLOAT 2D targets are handled; the texel values written (alpha
// included, in the BMP's fourth byte) are what a shader would sample through
// the fetch swizzle. The plugin keeps resolve output on the GPU unless told
// otherwise, so run with
//   --readback_resolve=full --d3d12_readback_resolve=true

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hook.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_STRING(pgr3_dump_frames, "", kCvarGroup,
                      "Comma-separated present frame numbers whose resolve "
                      "targets are written as BMPs in the user data folder");

REXCVAR_DEFINE_INT32(pgr3_trace_frame, 0, kCvarGroup,
                     "Log the D3D calls of this present frame and the next two "
                     "(render targets, draws with their textures, resolves)");

REXCVAR_DEFINE_BOOL(pgr3_dump_raw, false, kCvarGroup,
                    "With pgr3_dump_frames, also write each resolve target's "
                    "memory (from 64 KiB before its base) as a .bin");

REX_EXTERN(__imp__D3DDevice_Resolve);
REX_EXTERN(__imp__D3DDevice_SetRenderTarget);
REX_EXTERN(__imp__D3DDevice_SetDepthStencilSurface);
REX_EXTERN(__imp__D3DDevice_DrawVertices);
REX_EXTERN(__imp__D3DDevice_DrawIndexedVertices);
REX_EXTERN(__imp__D3DDevice_BeginVertices);
REX_EXTERN(__imp__D3DDevice_Clear);
REX_EXTERN(__imp__D3DDevice_InsertFence);
REX_EXTERN(__imp__D3DDevice_BeginTiling);
REX_EXTERN(__imp__D3DDevice_EndTiling);
REX_EXTERN(__imp__D3DDevice_BlockOnFence);

namespace {

namespace tu = rex::graphics::texture_util;

// D3DBaseTexture: the fetch constant sits at +16. XDK 2099's CreateTexture and
// SetTexture are the same code as 2135's, where rr6 verified the layout.
constexpr u32 kTextureFetchOffset = 16;

std::mutex g_mutex;
std::vector<u32> g_wanted;
bool g_parsed = false;
u32 g_frame = 0;    // presents completed
u32 g_resolves = 0; // resolves so far in the current present

bool Tracing() {
  const i32 wanted = REXCVAR_GET(pgr3_trace_frame);
  // Three presents: the plugin build's frames do not always hold a whole
  // scene render between two presents.
  return wanted > 0 && g_frame + 1 >= static_cast<u32>(wanted) &&
         g_frame + 1 < static_cast<u32>(wanted) + 3;
}

// D3DDevice: render targets at +0x30F8, textures at +0x31A0 (config/d3d.toml).
void TraceCall(const char *what, PPCContext &ctx, const std::string &extra) {
  const u32 dev = ctx.r3.u32;
  const auto *rt = pgr3::mem::at<const be_u32>(dev + 0x30F8);
  const auto *tex = pgr3::mem::at<const be_u32>(dev + 0x31A0);
  std::string t;
  for (int i = 0; tex && i < 4; ++i)
    if (u32(tex[i]))
      t += std::format(" t{}=0x{:08X}", i, u32(tex[i]));
  PGR3_INFO("[xtrace] {}{} rt=0x{:08X}{} lr=0x{:08X}", what, extra,
           rt ? u32(rt[0]) : 0, t, u32(ctx.lr));
}

// The resource headers a draw reads, decoded as XDK 2135 lays them out
// (rr6's gpu/d3d.h), to check that 2099's agree: Common (kind in bits
// 16-18: 1 VB, 2 IB, 4 surface), then a VB's fetch words at +12/+16, or an
// IB's base and size there (INDEX32 in Common bit 31).
std::string DescribeBuffers(u32 dev) {
  std::string out;
  const auto *streams = pgr3::mem::at<const be_u32>(dev + 0x310C); // {offset, vb}[16]
  for (int i = 0; streams && i < 4; ++i) {
    const u32 vb = u32(streams[i * 2 + 1]);
    if (!vb)
      continue;
    const auto *h = pgr3::mem::at<const be_u32>(vb);
    if (h)
      out += std::format(" s{}=0x{:08X}{{c={:08X} f0={:08X} f1={:08X} off={}}}", i, vb,
                         u32(h[0]), u32(h[3]), u32(h[4]), u32(streams[i * 2]));
  }
  const auto *ib = pgr3::mem::at<const be_u32>(dev + 0x30F4);
  if (ib && u32(*ib)) {
    const auto *h = pgr3::mem::at<const be_u32>(u32(*ib));
    if (h)
      out += std::format(" ib=0x{:08X}{{c={:08X} base={:08X} size={}}}", u32(*ib), u32(h[0]),
                         u32(h[3]), u32(h[4]));
  }
  return out;
}

// A render target or depth surface: Common, RB_SURFACE_INFO (+0x30: pitch
// bits 0-13, MSAA bits 16-17) and RB_COLOR_INFO / RB_DEPTH_INFO (+0x34: EDRAM
// base bits 0-11, format bits 16-19).
std::string DescribeSurface(u32 surface) {
  const auto *h = surface ? pgr3::mem::at<const be_u32>(surface) : nullptr;
  if (!h)
    return std::string();
  const u32 info = u32(h[12]), edram = u32(h[13]);
  return std::format(" {{c={:08X} pitch={} msaa={} base={} fmt={}}}", u32(h[0]), info & 0x3FFF,
                     (info >> 16) & 3, edram & 0xFFF, (edram >> 16) & 0xF);
}

void ParseWanted() {
  if (g_parsed)
    return;
  g_parsed = true;
  const std::string text = REXCVAR_GET(pgr3_dump_frames);
  size_t pos = 0;
  while (pos < text.size()) {
    size_t end = text.find(',', pos);
    if (end == std::string::npos)
      end = text.size();
    if (end > pos)
      g_wanted.push_back(static_cast<u32>(std::stoul(text.substr(pos, end - pos))));
    pos = end + 1;
  }
}

bool WriteBmp(const std::filesystem::path &path, u32 width, u32 height,
              const std::vector<u8> &bgra) {
  FILE *f = std::fopen(path.string().c_str(), "wb");
  if (!f)
    return false;
  const u32 image = width * height * 4;
  u8 header[54] = {'B', 'M'};
  auto put32 = [&](int off, u32 v) { std::memcpy(header + off, &v, 4); };
  auto put16 = [&](int off, u16 v) { std::memcpy(header + off, &v, 2); };
  put32(2, 54 + image);
  put32(10, 54);
  put32(14, 40);
  put32(18, width);
  put32(22, static_cast<u32>(-static_cast<i32>(height))); // top-down
  put16(26, 1);
  put16(28, 32);
  put32(34, image);
  std::fwrite(header, 1, sizeof(header), f);
  std::fwrite(bgra.data(), 1, image, f);
  std::fclose(f);
  return true;
}

void DumpTexture(u32 frame, u32 seq, u32 va, const std::filesystem::path &dir) {
  const auto *words = pgr3::mem::at<const be_u32>(va + kTextureFetchOffset);
  auto *memory = REX_KERNEL_MEMORY();
  if (!words || !memory)
    return;
  u32 fc[6];
  for (int i = 0; i < 6; ++i)
    fc[i] = u32(words[i]);
  const u32 format = fc[1] & 0x3F;
  const u32 endian = (fc[1] >> 6) & 3;
  const u32 base = fc[1] & 0xFFFFF000;
  const bool tiled = (fc[0] >> 31) & 1;
  const u32 pitch = ((fc[0] >> 22) & 0x1FF) * 32;
  const u32 width = (fc[2] & 0x1FFF) + 1;
  const u32 height = ((fc[2] >> 13) & 0x1FFF) + 1;
  const u32 swizzle = (fc[3] >> 1) & 0xFFF;
  // 32bpp: k_8_8_8_8, k_2_10_10_10, k_8_8_8_8_A, k_16_16_FLOAT.
  // 64bpp: k_16_16_16_16_FLOAT, PGR3's HDR scene format. Floats are clamped to
  // 0..1 for the BMP and the largest component is logged.
  // 16bpp: k_6_5_5 (PGR3's tree impostor atlas), x in bits 0-4, y 5-9,
  // z 10-15.
  const bool half = format == 31 || format == 32;
  const bool f655 = format == 5;
  const u32 bpp = format == 32 ? 8 : f655 ? 2 : 4;
  if (format != 6 && format != 7 && format != 14 && !half && !f655) {
    PGR3_INFO("pgr3_dump_frames: skip 0x{:08X} format {}", va, format);
    return;
  }
  // With readback on, tiled resolve output lands 4 KiB past the fetch
  // constant's base. rr6 took that for one 32x32 tile at 32bpp; PGR3's 64bpp
  // targets show it is 4 KiB at either size (fitted from pgr3_dump_raw).
  const u8 *src = memory->TranslatePhysical(base) + (tiled ? 4096u : 0u);
  const u32 aligned_h = (height + 31) & ~31u;
  if (REXCVAR_GET(pgr3_dump_raw)) {
    // Raw bytes for working out layouts offline: the window starts 64 KiB
    // below the base and runs 64 KiB past the aligned extent.
    constexpr u32 kSlack = 64 * 1024;
    const u32 lo = base >= kSlack ? base - kSlack : 0;
    const size_t size = size_t(base - lo) + size_t(pitch) * aligned_h * bpp + kSlack;
    const auto raw = dir / std::format("pgr3_frame_{}_r{:02}_{:08X}_{}x{}_fmt{}_e{}_p{}_t{}_base{:08X}_lo{:08X}.bin",
                                       frame, seq, va, width, height, format, endian,
                                       pitch, u32(tiled), base, lo);
    if (FILE *f = std::fopen(raw.string().c_str(), "wb")) {
      std::fwrite(memory->TranslatePhysical(lo), 1, size, f);
      std::fclose(f);
    }
  }
  auto swap = [endian](const u8 *b) -> u32 {
    switch (endian) {
    case 2: // 8in32
      return u32(b[0]) << 24 | u32(b[1]) << 16 | u32(b[2]) << 8 | b[3];
    case 1: // 8in16 (rr6 had this and 16in32 exchanged; it only met 8in32)
      return u32(b[2]) << 24 | u32(b[3]) << 16 | u32(b[0]) << 8 | b[1];
    case 3: // 16in32
      return u32(b[1]) << 24 | u32(b[0]) << 16 | u32(b[3]) << 8 | b[2];
    default:
      return u32(b[3]) << 24 | u32(b[2]) << 16 | u32(b[1]) << 8 | b[0];
    }
  };
  auto half_to_float = [](u32 h) {
    const u32 e = (h >> 10) & 0x1F, m = h & 0x3FF;
    const float mag = e == 0    ? std::ldexp(float(m), -24)
                      : e == 31 ? 65504.0f
                                : std::ldexp(float(m | 0x400), int(e) - 25);
    return (h & 0x8000) ? -mag : mag;
  };
  float peak = 0.0f;
  std::vector<u8> bgra(size_t(width) * height * 4);
  for (u32 y = 0; y < height; ++y) {
    for (u32 x = 0; x < width; ++x) {
      const u32 off = tiled ? u32(tu::GetTiledOffset2D(i32(x), i32(y), pitch,
                                                       bpp == 8 ? 3 : bpp == 2 ? 1 : 2))
                            : (y * pitch + x) * bpp;
      if (off + bpp > pitch * aligned_h * bpp)
        continue;
      // A 16bpp texel is swapped within its own two bytes (8in16).
      const u32 v = f655 ? (endian == 1 || endian == 2
                                ? u32(src[off]) << 8 | src[off + 1]
                                : u32(src[off + 1]) << 8 | src[off])
                         : swap(src + off);
      u8 comp[6] = {u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24), 0, 255};
      if (f655) {
        comp[0] = u8((v & 0x1F) * 255 / 31);
        comp[1] = u8(((v >> 5) & 0x1F) * 255 / 31);
        comp[2] = u8(((v >> 10) & 0x3F) * 255 / 63);
        comp[3] = 255;
      } else if (format == 7) { // 2_10_10_10: keep the top 8 bits of each channel
        comp[0] = u8((v >> 2) & 0xFF);
        comp[1] = u8((v >> 12) & 0xFF);
        comp[2] = u8((v >> 22) & 0xFF);
        comp[3] = u8(((v >> 30) & 3) * 85);
      } else if (half) {
        const u32 v1 = bpp == 8 ? swap(src + off + 4) : 0x3C000000u; // B 0, A 1
        const u32 h[4] = {v & 0xFFFF, v >> 16, v1 & 0xFFFF, v1 >> 16};
        for (int c = 0; c < 4; ++c) {
          const float f = half_to_float(h[c]);
          peak = std::max(peak, f);
          comp[c] = u8(std::clamp(f, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
      }
      auto pick = [&](u32 sel) { return comp[std::min<u32>(sel, 5)]; };
      u8 *out = bgra.data() + (size_t(y) * width + x) * 4;
      out[2] = pick(swizzle & 7);
      out[1] = pick((swizzle >> 3) & 7);
      out[0] = pick((swizzle >> 6) & 7);
      out[3] = pick((swizzle >> 9) & 7);
    }
  }
  const auto path = dir / std::format("pgr3_frame_{}_r{:02}_{:08X}_{}x{}.bmp", frame,
                                      seq, va, width, height);
  if (WriteBmp(path, width, height, bgra))
    PGR3_INFO("pgr3_dump_frames: wrote {} fmt={} endian={} tiled={} swz={:03X}{}",
             path.string(), format, endian, u32(tiled), swizzle,
             half ? std::format(" peak={:.3f}", peak) : std::string());
}

} // namespace

REX_HOOK_RAW(D3DDevice_SetRenderTarget) {
  if (Tracing())
    PGR3_INFO("[xtrace] SetRenderTarget {} 0x{:08X}{} lr=0x{:08X} frame={}",
             ctx.r4.u32, ctx.r5.u32, DescribeSurface(ctx.r5.u32), u32(ctx.lr), g_frame + 1);
  __imp__D3DDevice_SetRenderTarget(ctx, base);
}

// (device, surface).
REX_HOOK_RAW(D3DDevice_SetDepthStencilSurface) {
  if (Tracing())
    PGR3_INFO("[xtrace] SetDepthStencilSurface 0x{:08X}{} lr=0x{:08X} frame={}",
             ctx.r4.u32, DescribeSurface(ctx.r4.u32), u32(ctx.lr), g_frame + 1);
  __imp__D3DDevice_SetDepthStencilSurface(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawVertices) {
  if (Tracing())
    TraceCall("Draw", ctx, std::format(" prim={} n={}{}", ctx.r4.u32, ctx.r6.u32,
                                       DescribeBuffers(ctx.r3.u32)));
  __imp__D3DDevice_DrawVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_DrawIndexedVertices) {
  if (Tracing())
    TraceCall("DrawIndexed", ctx,
              std::format(" prim={} n={}{}", ctx.r4.u32, ctx.r7.u32,
                          DescribeBuffers(ctx.r3.u32)));
  __imp__D3DDevice_DrawIndexedVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_BeginVertices) {
  if (Tracing())
    TraceCall("BeginVertices", ctx,
              std::format(" prim={} n={}", ctx.r4.u32, ctx.r5.u32));
  __imp__D3DDevice_BeginVertices(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Clear) {
  if (Tracing())
    TraceCall("Clear", ctx, std::format(" flags=0x{:X}", ctx.r6.u32));
  __imp__D3DDevice_Clear(ctx, base);
}

// Predicated tiling, which RR6 never used. (device, flags, tile count, tile
// D3DRECTs, clear colour, z in f1, stencil in r9).
REX_HOOK_RAW(D3DDevice_BeginTiling) {
  if (Tracing()) {
    std::string rects;
    const auto *r = pgr3::mem::at<const be_i32>(ctx.r6.u32);
    for (u32 i = 0; r && i < std::min<u32>(ctx.r5.u32, 8); ++i)
      rects += std::format(" ({},{})-({},{})", i32(r[i * 4]), i32(r[i * 4 + 1]),
                           i32(r[i * 4 + 2]), i32(r[i * 4 + 3]));
    TraceCall("BeginTiling", ctx,
              std::format(" flags=0x{:X} tiles={}{}", ctx.r4.u32, ctx.r5.u32, rects));
  }
  __imp__D3DDevice_BeginTiling(ctx, base);
}

REX_HOOK_RAW(D3DDevice_EndTiling) {
  if (Tracing())
    TraceCall("EndTiling", ctx,
              std::format(" r4=0x{:X} r5=0x{:X} r6=0x{:08X} r7=0x{:08X}", ctx.r4.u32,
                          ctx.r5.u32, ctx.r6.u32, ctx.r7.u32));
  __imp__D3DDevice_EndTiling(ctx, base);
}

REX_HOOK_RAW(D3DDevice_Resolve) {
  if (Tracing())
    TraceCall("Resolve", ctx,
              std::format(" flags=0x{:X} dest=0x{:08X}", ctx.r4.u32, ctx.r6.u32));
  const u32 device = ctx.r3.u32;
  const u32 dest = ctx.r6.u32;
  bool wanted;
  u32 frame, seq;
  {
    std::lock_guard lock(g_mutex);
    ParseWanted();
    frame = g_frame + 1;
    seq = g_resolves++;
    wanted = dest && std::find(g_wanted.begin(), g_wanted.end(), frame) != g_wanted.end();
  }
  __imp__D3DDevice_Resolve(ctx, base);
  if (!wanted)
    return;
  // Resolve returns nothing, so the volatile registers are free to reuse.
  ctx.r3.u64 = device;
  __imp__D3DDevice_InsertFence(ctx, base);
  __imp__D3DDevice_BlockOnFence(ctx, base); // takes the fence InsertFence returned in r3
  std::filesystem::path dir = rex::Runtime::instance()
                                  ? rex::Runtime::instance()->user_data_root()
                                  : std::filesystem::path(".");
  std::error_code ec;
  std::filesystem::create_directories(dir, ec);
  DumpTexture(frame, seq, dest, dir);
}

// Called by input_script.cpp's Swap wrapper once per present.
void Pgr3XenosDumpOnPresent() {
  std::lock_guard lock(g_mutex);
  ++g_frame;
  g_resolves = 0;
}
