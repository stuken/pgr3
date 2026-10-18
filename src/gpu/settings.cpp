/**
 * @file    gpu/settings.cpp
 * @license BSD 3-Clause, see LICENSE
 */
#include "gpu/settings.h"

#include <charconv>
#include <string>
#include <string_view>

#include <rex/cvar.h>

#include "core/logging.h"
#include "core/settings.h" // kCvarGroup
#include "gpu/output.h"

REXCVAR_DECLARE(bool, pgr3_geometry_gpu_upload);
REXCVAR_DECLARE(bool, pgr3_dred);
REXCVAR_DECLARE(std::string, pgr3_anisotropy);
REXCVAR_DECLARE(i32, pgr3_supersampling);
REXCVAR_DECLARE(bool, pgr3_vsync);

REXCVAR_DEFINE_BOOL(pgr3_geometry_gpu_upload, true, kCvarGroup,
                    "Place static geometry in the GPU_UPLOAD heap when the "
                    "device supports it. Off uses UPLOAD instead, costing the "
                    "write-combine win on AMD. Requires restart.");

REXCVAR_DEFINE_BOOL(pgr3_dred, true, kCvarGroup,
                    "Record D3D12 auto-breadcrumbs and page-fault allocations "
                    "so a lost device names the op and resource it died on. "
                    "Costs a little GPU time per op. Requires restart.");

REXCVAR_DEFINE_STRING(pgr3_anisotropy, "default", kCvarGroup,
                      "Anisotropic texture filtering. default = what the "
                      "game's sampler states ask for; 2, 4, 8 or 16 forces "
                      "that level on every sampler the game sets to linear "
                      "filtering.")
    .allowed({"default", "2", "4", "8", "16"});

REXCVAR_DEFINE_INT32(pgr3_supersampling, 1, kCvarGroup,
                     "Render at twice the resolution scale and downsample "
                     "(pgr3_resolution_scale). 1 = off, 2 = on. Requires "
                     "restart.")
    .range(1, 2)
    .validator([](std::string_view v) {
      int n = 0;
      auto r = std::from_chars(v.data(), v.data() + v.size(), n);
      return r.ec == std::errc() && (n == 1 || n == 2);
    });

REXCVAR_DEFINE_BOOL(pgr3_vsync, true, kCvarGroup, "Vertical sync.");

namespace pgr3::gpu {
namespace {

std::string FormatCvar(i32 v) { return std::to_string(v); }
std::string FormatCvar(bool v) { return v ? "true" : "false"; }

// 0 for "default". The cvar's allowed list only guards config and console
// writes: the SDK applies command line and environment values unvalidated, so
// anything other than 2, 4, 8 or 16 falls back to default here too.
i32 ParseAnisotropy(std::string_view v) {
  i32 n = 0;
  auto r = std::from_chars(v.data(), v.data() + v.size(), n);
  if (r.ec == std::errc() && r.ptr == v.data() + v.size() &&
      (n == 2 || n == 4 || n == 8 || n == 16))
    return n;
  if (v != "default")
    PGR3_WARN("pgr3_anisotropy: '{}' is not default, 2, 4, 8 or 16; using "
             "default",
             v);
  return 0;
}

} // namespace

Settings &Settings::Get() {
  static Settings s;
  return s;
}

void Settings::AdoptAnisotropy() {
  anisotropy_ = ParseAnisotropy(REXCVAR_GET(pgr3_anisotropy));
}
void Settings::AdoptVsync() { vsync_ = REXCVAR_GET(pgr3_vsync); }
void Settings::AdoptGeometryGPUUpload() {
  geometryGPUUpload_ = REXCVAR_GET(pgr3_geometry_gpu_upload);
}
void Settings::AdoptDRED() { dred_ = REXCVAR_GET(pgr3_dred); }
void Settings::AdoptSuperSampling() {
  superSampling_ = REXCVAR_GET(pgr3_supersampling);
}

bool Settings::SetAnisotropy(i32 v) {
  return rex::cvar::SetFlagByName("pgr3_anisotropy",
                                  v == 0 ? "default" : FormatCvar(v));
}

bool Settings::SetVsync(bool v) {
  return rex::cvar::SetFlagByName("pgr3_vsync", FormatCvar(v));
}

bool Settings::SetSuperSampling(i32 v) {
  return rex::cvar::SetFlagByName("pgr3_supersampling", FormatCvar(v));
}

void Settings::AdoptCvars() {
  AdoptAnisotropy();
  AdoptVsync();
  AdoptGeometryGPUUpload();
  AdoptDRED();
  AdoptSuperSampling();
}

void Settings::Init() {
  AdoptCvars();

  auto reg = [](const char *name, void (Settings::*adopt)()) {
    rex::cvar::RegisterChangeCallback(
        name, [adopt](std::string_view, std::string_view) {
          (Settings::Get().*adopt)();
        });
  };
  reg("pgr3_anisotropy", &Settings::AdoptAnisotropy);
  reg("pgr3_vsync", &Settings::AdoptVsync);
  reg("pgr3_geometry_gpu_upload", &Settings::AdoptGeometryGPUUpload);
  reg("pgr3_dred", &Settings::AdoptDRED);
  reg("pgr3_supersampling", &Settings::AdoptSuperSampling);

  rex::cvar::RegisterChangeCallback(
      "resolution", [](std::string_view, std::string_view) {
        Output::Recompute();
      });
}

} // namespace pgr3::gpu
