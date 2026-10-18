# XenosRecomp runs on the build host and turns PGR3's Xenos shaders into the
# embedded DXIL/SPIR-V cache. REBLUE_RECOMP selects the shader ABI shared with
# the reblue-derived renderer; PGR3_RECOMP selects PGR3's vertex input
# locations and interpolators and keeps Blue Dragon's shader rewrites out.
# Revision 0x0E containers (XDK 2099 and 2135) are handled unconditionally.
add_subdirectory(XenosRecomp EXCLUDE_FROM_ALL)
target_compile_definitions(XenosRecomp PRIVATE REBLUE_RECOMP PGR3_RECOMP)
