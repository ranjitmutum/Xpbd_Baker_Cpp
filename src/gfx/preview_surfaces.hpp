#pragma once

#include "xpbd/gfx/preview_scene.hpp"

namespace xpbd::gfx::detail {

// FastNoiseLite-backed static terrain. The frozen upstream source and license
// are recorded in third_party/PREVIEW_SCENE_UPSTREAMS.md.
void appendOpenSourceDesertSurface(ViewportGpuScene &out, float scene_seed);

// CPU port of osgw's Gerstner displacement/analytic-normal equations. Stable
// topology keeps the existing Vulkan RT dynamic-refit path valid.
void appendOpenSourceOceanSurface(ViewportGpuScene &out, float time_sec,
                                  bool dynamic, float scene_seed);

} // namespace xpbd::gfx::detail
