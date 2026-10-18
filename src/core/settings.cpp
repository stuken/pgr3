// pgr3 - ReXGlue Recompiled Project

#include "core/settings.h"

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(pgr3_devmode, false, kCvarGroup,
                    "Extra renderer diagnostics and developer logging");
REXCVAR_DEFINE_INT32(pgr3_shutdown_timeout_ms, 5000, kCvarGroup,
                     "Milliseconds a quit may take before it is forced");

namespace pgr3 {

Settings &Settings::Get() {
  static Settings settings;
  return settings;
}

bool Settings::Devmode() const { return REXCVAR_GET(pgr3_devmode); }

i32 Settings::ShutdownTimeoutMs() const {
  return REXCVAR_GET(pgr3_shutdown_timeout_ms);
}

} // namespace pgr3
