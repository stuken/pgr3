// pgr3 - ReXGlue Recompiled Project
//
// pgr3_scenery_probe: bring-up diagnostic for the scenery the native renderer
// is missing. sub_8220D0B8 draws one batch of scenery objects: for list
// (r4 * 2 + {0, 1}) of the table at obj+2276 (12-byte entries: u16 first in
// the low half of +0, last at +4, count at +8), each object record at
// obj+2260 (12 bytes) is drawn when its byte 0 is set. The probe counts the
// records walked and those set, in both builds, so the two can be compared.

#include <algorithm>
#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/types.h>

#include "core/logging.h"
#include "core/memory_helpers.h"
#include "core/settings.h"

REXCVAR_DEFINE_BOOL(pgr3_scenery_probe, false, kCvarGroup,
                    "Log how many scenery objects sub_8220D0B8 walks and draws");

REX_EXTERN(__imp__sub_8220D0B8);
REX_EXTERN(__imp__sub_82208C28);

namespace {
std::atomic<u64> g_calls{0}, g_records{0}, g_set{0};
std::atomic<u64> g_material_calls{0}, g_material_ok{0};
} // namespace

REX_HOOK_RAW(sub_8220D0B8) {
  if (REXCVAR_GET(pgr3_scenery_probe)) {
    const u32 obj = ctx.r3.u32;
    const u32 lists = pgr3::mem::try_load<u32>(obj + 2276);
    const u32 records = pgr3::mem::try_load<u32>(obj + 2260);
    const u32 base_list = (ctx.r4.u32 << 1) & 0x1FE;
    u32 walked = 0, set = 0;
    for (u32 k = 0; k < 2 && lists && records; ++k) {
      const u32 entry = lists + (base_list + k) * 12;
      if (pgr3::mem::try_load<u32>(entry + 8) == 0)
        continue;
      const u32 first = pgr3::mem::try_load<u32>(entry) & 0xFFFF;
      const u32 last = pgr3::mem::try_load<u32>(entry + 4);
      for (u32 i = first; i <= last && i - first < 100000; ++i) {
        ++walked;
        if (pgr3::mem::try_load<u8>(records + i * 12) != 0)
          ++set;
      }
    }
    g_records += walked;
    g_set += set;
    const u64 n = ++g_calls;
    if (n % 2000 == 0)
      PGR3_INFO("scenery probe: {} calls, {} records walked, {} set ({:.1f}%); material "
                "setups {}, {} succeeded",
                n, g_records.load(), g_set.load(),
                100.0 * double(g_set.load()) / double(std::max<u64>(g_records.load(), 1)),
                g_material_calls.load(), g_material_ok.load());
  }
  __imp__sub_8220D0B8(ctx, base);
}

// The material setup the draw loop calls when a record's material changes:
// its result gates every draw until the next change.
REX_HOOK_RAW(sub_82208C28) {
  __imp__sub_82208C28(ctx, base);
  if (REXCVAR_GET(pgr3_scenery_probe)) {
    ++g_material_calls;
    if (ctx.r3.u32 & 0xFF)
      ++g_material_ok;
  }
}
