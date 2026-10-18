// pgr3 - ReXGlue Recompiled Project

#pragma once

namespace pgr3::gpu {

struct VideoState;
struct GuestTexture;

// Called once per presented frame with the frame's source render target, after
// its command list was submitted. Dumps it when pgr3_dump_frames names this
// frame. Caller holds s.mutex.
void MaybeDumpFrame(VideoState &s, GuestTexture *rt);

} // namespace pgr3::gpu
