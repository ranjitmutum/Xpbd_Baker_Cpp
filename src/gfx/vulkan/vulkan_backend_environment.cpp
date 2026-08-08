#include "vulkan/vulkan_backend_internal.hpp"
#include "xpbd/gfx/rt_scene_records.hpp"
#include "xpbd/log.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <future>
#include <limits>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace xpbd::gfx::detail {

float halfToFloat(std::uint16_t value) noexcept {
  const bool negative = (value & 0x8000u) != 0u;
  const std::uint32_t exponent = (value >> 10u) & 0x1fu;
  const std::uint32_t mantissa = value & 0x03ffu;
  double decoded = 0.0;
  if (exponent == 0u) {
    decoded = std::ldexp(static_cast<double>(mantissa), -24);
  } else if (exponent == 0x1fu) {
    decoded = mantissa == 0u
                  ? std::numeric_limits<double>::infinity()
                  : std::numeric_limits<double>::quiet_NaN();
  } else {
    decoded = std::ldexp(static_cast<double>(1024u + mantissa),
                         static_cast<int>(exponent) - 25);
  }
  return static_cast<float>(negative ? -decoded : decoded);
}

static const uint32_t kSpvAtmosphereTransmittanceComp[] = {
#include "spirv/atmosphere_transmittance.comp.spv.inc"
};

static const uint32_t kSpvAtmosphereDirectIrradianceComp[] = {
#include "spirv/atmosphere_direct_irradiance.comp.spv.inc"
};

static const uint32_t kSpvAtmosphereSingleScatteringComp[] = {
#include "spirv/atmosphere_single_scattering.comp.spv.inc"
};

static const uint32_t kSpvAtmosphereScatteringDensityComp[] = {
#include "spirv/atmosphere_scattering_density.comp.spv.inc"
};

static const uint32_t kSpvAtmosphereIndirectIrradianceComp[] = {
#include "spirv/atmosphere_indirect_irradiance.comp.spv.inc"
};

static const uint32_t kSpvAtmosphereMultipleScatteringComp[] = {
#include "spirv/atmosphere_multiple_scattering.comp.spv.inc"
};

static const uint32_t kSpvAtmosphereEnvironmentCacheComp[] = {
#include "spirv/atmosphere_environment_cache.comp.spv.inc"
};

[[nodiscard]] VulkanBackend::DynamicSkyCpuResult VulkanBackend::buildDynamicSkyDistribution(
      const DynamicSkyCpuInput &input, VkDevice device, VkFence fence,
      Clock::time_point submitted_at,
      std::shared_ptr<RenderThreadControl> control) {
    DynamicSkyCpuResult result;
    for (;;) {
      if (control && control->fatalQuarantineRequested()) {
        const VkResult status = vkGetFenceStatus(device, fence);
        result.fence_result =
            status == VK_SUCCESS ? VK_SUCCESS : VK_TIMEOUT;
        break;
      }
      result.fence_result = vkWaitForFences(
          device, 1, &fence, VK_TRUE,
          control ? 25'000'000ull : UINT64_MAX);
      if (result.fence_result != VK_TIMEOUT) {
        break;
      }
    }
    result.cache_compute_ms =
        std::chrono::duration<double, std::milli>(Clock::now() - submitted_at)
            .count();
    if (result.fence_result != VK_SUCCESS) {
      result.error = "dynamic-sky GPU fence wait failed";
      return result;
    }
    if (input.readback == nullptr || input.distribution_mapped == nullptr ||
        input.width == 0u || input.height == 0u) {
      result.error = "dynamic-sky readback/output mapping is unavailable";
      return result;
    }

    try {
      const auto readback_begin = Clock::now();
      const std::uint64_t pixel_count =
          static_cast<std::uint64_t>(input.width) * input.height;
      const VkDeviceSize distribution_bytes =
          sizeof(WorldEnvironmentGpuHeader) +
          static_cast<VkDeviceSize>(pixel_count) *
              sizeof(WorldEnvironmentGpuAlias);
      if (distribution_bytes > input.distribution_capacity) {
        result.error = "dynamic-sky distribution buffer is undersized";
        return result;
      }

      FloatEnvironmentImage radiance;
      radiance.width = input.width;
      radiance.height = input.height;
      radiance.rgba.resize(static_cast<std::size_t>(pixel_count) * 4u);
      std::uint64_t positive_rgb = 0u;
      float brightest_luminance = 0.0f;
      std::uint32_t brightest_x = 0u;
      std::uint32_t brightest_y = 0u;
      double moon_integrated_luminance = 0.0;
      bool valid_radiance = true;
      constexpr double kPi = 3.14159265358979323846;
      constexpr double kTwoPi = 2.0 * kPi;
      for (std::uint64_t pixel = 0;
           valid_radiance && pixel < pixel_count; ++pixel) {
        for (std::uint32_t channel = 0; channel < 3u; ++channel) {
          const float value =
              halfToFloat(input.readback[pixel * 4u + channel]);
          if (!std::isfinite(value) || value < 0.0f) {
            valid_radiance = false;
            break;
          }
          radiance.rgba[pixel * 4u + channel] = value;
          positive_rgb += value > 0.0f ? 1u : 0u;
        }
        const float moon_luminance =
            halfToFloat(input.readback[pixel * 4u + 3u]);
        radiance.rgba[pixel * 4u + 3u] = moon_luminance;
        valid_radiance = valid_radiance && std::isfinite(moon_luminance) &&
                         moon_luminance >= 0.0f;
        if (!valid_radiance) {
          break;
        }
        const float luminance =
            0.2126f * radiance.rgba[pixel * 4u] +
            0.7152f * radiance.rgba[pixel * 4u + 1u] +
            0.0722f * radiance.rgba[pixel * 4u + 2u];
        const std::uint32_t x =
            static_cast<std::uint32_t>(pixel % input.width);
        const std::uint32_t y =
            static_cast<std::uint32_t>(pixel / input.width);
        const double theta0 =
            kPi * static_cast<double>(y) / static_cast<double>(input.height);
        const double theta1 = kPi * static_cast<double>(y + 1u) /
                              static_cast<double>(input.height);
        const double solid_angle =
            (kTwoPi / static_cast<double>(input.width)) *
            (std::cos(theta0) - std::cos(theta1));
        moon_integrated_luminance +=
            static_cast<double>(moon_luminance) * solid_angle;
        if (luminance > brightest_luminance) {
          brightest_luminance = luminance;
          brightest_x = x;
          brightest_y = y;
        }
      }
      if (!valid_radiance || positive_rgb == 0u) {
        result.error = "dynamic-sky readback contains invalid radiance";
        return result;
      }

      for (std::uint64_t pixel = 0; pixel < pixel_count; ++pixel) {
        const float moon_luminance = radiance.rgba[pixel * 4u + 3u];
        const float lighting_moon_luminance =
            input.sun_moon_lighting && input.moon.enabled
                ? moon_luminance
                : 0.0f;
        for (std::uint32_t channel = 0; channel < 3u; ++channel) {
          // SunDisk is a separate Light Registry family. Keeping it out of
          // this alias data prevents Environment and Sun from counting it
          // twice while preserving the disk in background evaluation.
          radiance.rgba[pixel * 4u + channel] += lighting_moon_luminance;
        }
        radiance.rgba[pixel * 4u + 3u] = 1.0f;
      }
      result.readback_ms =
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    readback_begin)
              .count();

      EnvironmentDistribution distribution;
      const auto distribution_begin = Clock::now();
      if (!distribution.build(radiance)) {
        result.error = "dynamic-sky importance distribution build failed";
        return result;
      }
      result.environment_power_estimate = estimateEnvironmentPower(
          radiance, input.environment_strength);
      result.distribution_build_ms =
          std::chrono::duration<double, std::milli>(Clock::now() -
                                                    distribution_begin)
              .count();

      const double moon_u_unwrapped =
          std::atan2(input.celestial.moon.direction[0],
                     input.celestial.moon.direction[2]) /
          kTwoPi;
      const double moon_u =
          moon_u_unwrapped < 0.0 ? moon_u_unwrapped + 1.0
                                 : moon_u_unwrapped;
      const double moon_v =
          std::acos(std::clamp(input.celestial.moon.direction[1], -1.0,
                               1.0)) /
          kPi;
      const std::int32_t moon_center_x = static_cast<std::int32_t>(
          std::clamp(moon_u * input.width, 0.0,
                     static_cast<double>(input.width - 1u)));
      const std::int32_t moon_center_y = static_cast<std::int32_t>(
          std::clamp(moon_v * input.height, 0.0,
                     static_cast<double>(input.height - 1u)));
      const double moon_angular_radius =
          std::clamp(static_cast<double>(
                         input.moon.angular_diameter_degrees),
                     0.05, 5.0) *
          0.5 * (kPi / 180.0);
      const double moon_solid_angle =
          kTwoPi * (1.0 - std::cos(moon_angular_radius));
      const float moon_mean_luminance =
          moon_solid_angle > 0.0
              ? static_cast<float>(moon_integrated_luminance /
                                   moon_solid_angle)
              : 0.0f;
      double moon_probability = 0.0;
      float moon_peak_luminance = 0.0f;
      for (std::int32_t offset_y = -1; offset_y <= 1; ++offset_y) {
        const std::uint32_t y = static_cast<std::uint32_t>(std::clamp(
            moon_center_y + offset_y, 0,
            static_cast<std::int32_t>(input.height - 1u)));
        for (std::int32_t offset_x = -1; offset_x <= 1; ++offset_x) {
          const std::int32_t wrapped_x =
              (moon_center_x + offset_x +
               static_cast<std::int32_t>(input.width)) %
              static_cast<std::int32_t>(input.width);
          const std::uint32_t x = static_cast<std::uint32_t>(wrapped_x);
          moon_probability += distribution.texelProbability(x, y);
          moon_peak_luminance = (std::max)(
              moon_peak_luminance,
              halfToFloat(input.readback[
                  (static_cast<std::size_t>(y) * input.width + x) * 4u +
                  3u]));
        }
      }

      constexpr std::uint32_t kValidEnvironment = 1u << 0u;
      constexpr std::uint32_t kBackgroundVisible = 1u << 1u;
      constexpr std::uint32_t kLightingEnabled = 1u << 2u;
      constexpr std::uint32_t kProceduralFiniteMoon = 1u << 3u;
      constexpr std::uint32_t kSunBackgroundVisible = 1u << 4u;
      constexpr std::uint32_t kMoonBackgroundVisible = 1u << 5u;
      constexpr std::uint32_t kSunLightingEnabled = 1u << 6u;
      constexpr std::uint32_t kMoonLightingEnabled = 1u << 7u;
      constexpr std::uint32_t kSunCastsShadows = 1u << 8u;
      constexpr std::uint32_t kMoonCastsShadows = 1u << 9u;
      constexpr std::uint32_t kMoonSurfaceDetail = 1u << 10u;
      constexpr std::uint32_t kMoonManualPhase = 1u << 11u;
      const double sun_angular_radius = input.sun.angular_radius;
      const std::array<float, 3> &sun_color = input.sun.color;
      WorldEnvironmentGpuHeader header;
      header.flags =
          kValidEnvironment |
          (input.background_visible ? kBackgroundVisible : 0u) |
          (input.environment_lighting ? kLightingEnabled : 0u) |
          (input.moon.enabled ? kProceduralFiniteMoon : 0u) |
          (input.sun.disk_visible ? kSunBackgroundVisible : 0u) |
          (input.moon.enabled && input.moon.disk_visible
               ? kMoonBackgroundVisible
               : 0u) |
          (input.sun.lighting_enabled ? kSunLightingEnabled : 0u) |
          (input.sun_moon_lighting && input.moon.enabled
               ? kMoonLightingEnabled
               : 0u) |
          (input.sun.casts_shadow ? kSunCastsShadows : 0u) |
          (input.moon.cast_shadows ? kMoonCastsShadows : 0u) |
          (input.moon.surface_detail > 0.0f ? kMoonSurfaceDetail : 0u) |
          (input.moon.phase_mode == MoonPhaseMode::Manual
               ? kMoonManualPhase
               : 0u);
      header.width = input.width;
      header.height = input.height;
      header.entry_count = static_cast<std::uint32_t>(pixel_count);
      header.lighting_strength = input.environment_strength;
      header.background_multiplier = input.background_multiplier;
      header.rotation_radians = input.rotation_radians;
      header.padding = static_cast<float>(sun_angular_radius);
      header.sun_direction_moon_mean = {
          static_cast<float>(input.celestial.sun.direction[0]),
          static_cast<float>(input.celestial.sun.direction[1]),
          static_cast<float>(input.celestial.sun.direction[2]),
          moon_mean_luminance};
      header.moon_direction_angular_radius = {
          static_cast<float>(input.celestial.moon.direction[0]),
          static_cast<float>(input.celestial.moon.direction[1]),
          static_cast<float>(input.celestial.moon.direction[2]),
          static_cast<float>(moon_angular_radius)};
      header.moon_phase_libration = {
          input.moon_fraction, input.moon_phase_radians,
          static_cast<float>(
              input.celestial.moon_libration_latitude_degrees *
              (kPi / 180.0)),
          static_cast<float>(
              input.celestial.moon_libration_longitude_degrees *
              (kPi / 180.0))};
      header.sun_color_strength = {
          sun_color[0], sun_color[1], sun_color[2],
          std::clamp(input.sun.strength, 0.0f, 32.0f)};
      header.light_power = {result.environment_power_estimate,
                            input.sun.power_estimate, 0.0f, 0.0f};
      std::memcpy(input.distribution_mapped, &header, sizeof(header));
      auto *gpu_alias = reinterpret_cast<WorldEnvironmentGpuAlias *>(
          static_cast<std::byte *>(input.distribution_mapped) +
          sizeof(WorldEnvironmentGpuHeader));
      for (std::uint32_t y = 0; y < input.height; ++y) {
        for (std::uint32_t x = 0; x < input.width; ++x) {
          const std::uint32_t index = y * input.width + x;
          gpu_alias[index].acceptance = static_cast<float>(
              std::clamp(distribution.aliasAcceptance(x, y), 0.0, 1.0));
          gpu_alias[index].alias_index = distribution.aliasIndex(x, y);
          gpu_alias[index].probability = static_cast<float>(
              (std::max)(distribution.texelProbability(x, y), 0.0));
        }
      }
      result.success = true;
      result.positive_rgb = positive_rgb;
      result.brightest_luminance = brightest_luminance;
      result.brightest_x = brightest_x;
      result.brightest_y = brightest_y;
      result.moon_probability = moon_probability;
      result.moon_peak_luminance = moon_peak_luminance;
    } catch (const std::exception &exception) {
      result.error = std::string("dynamic-sky CPU build exception: ") +
                     exception.what();
    } catch (...) {
      result.error = "dynamic-sky CPU build failed with unknown exception";
    }
    return result;
  }

bool VulkanBackend::pollDynamicSkyEnvironmentCache(bool wait_for_completion) {
    DynamicSkyPending &pending = atmosphere_environment_pending_;
    if (!pending.active()) {
      return false;
    }
    if (!pending.completion.valid()) {
      if (!wait_for_completion) {
        return false;
      }
      DynamicSkyCpuResult fallback;
      const ControlledWaitResult wait = waitForFenceControlled(
          pending.fence, "vkWaitForFences.dynamic_sky_fallback",
          static_cast<std::uint32_t>(frame_index_), VK_NULL_HANDLE,
          pending.command, true, true);
      fallback.fence_result = wait.result;
      fallback.error = "dynamic-sky worker launch failed";
      std::promise<DynamicSkyCpuResult> promise;
      pending.completion = promise.get_future();
      promise.set_value(std::move(fallback));
    } else if (!wait_for_completion &&
               pending.completion.wait_for(std::chrono::milliseconds(0)) !=
                   std::future_status::ready) {
      return false;
    } else if (wait_for_completion) {
      while (pending.completion.wait_for(std::chrono::milliseconds(25)) !=
             std::future_status::ready) {
        if (render_thread_control_ &&
            render_thread_control_->fatalQuarantineRequested()) {
          markGpuCompletionUnproven("dynamic_sky_cpu_completion");
          return true;
        }
      }
    }

    DynamicSkyCpuResult cpu_result;
    try {
      cpu_result = pending.completion.get();
    } catch (const std::exception &exception) {
      // A broken future does not prove that the submitted command buffer has
      // completed. Establish completion independently before reclaiming any
      // Vulkan object owned by the pending transaction.
      const ControlledWaitResult wait = waitForFenceControlled(
          pending.fence, "vkWaitForFences.dynamic_sky_exception",
          static_cast<std::uint32_t>(frame_index_), VK_NULL_HANDLE,
          pending.command, true, true);
      cpu_result.fence_result = wait.result;
      cpu_result.error = std::string("dynamic-sky worker completion failed: ") +
                         exception.what();
    } catch (...) {
      const ControlledWaitResult wait = waitForFenceControlled(
          pending.fence, "vkWaitForFences.dynamic_sky_exception",
          static_cast<std::uint32_t>(frame_index_), VK_NULL_HANDLE,
          pending.command, true, true);
      cpu_result.fence_result = wait.result;
      cpu_result.error =
          "dynamic-sky worker completion failed with unknown exception";
    }
    if (cpu_result.fence_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Dynamic sky completion could not be established: "
          "API=vkWaitForFences VkResult=%s(%d) frame_slot=%u "
          "still_job_id=%llu; quarantining submitted resources until device "
          "teardown",
          vkResultName(cpu_result.fence_result),
          static_cast<int>(cpu_result.fence_result), frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      markGpuCompletionUnproven("vkWaitForFences(dynamic_sky)");
      // Do not destroy the fence, free the command buffer, or release any
      // image/buffer here: a non-successful wait does not prove completion.
      return true;
    }
    if (pending.fence != VK_NULL_HANDLE) {
      vkDestroyFence(device_, pending.fence, nullptr);
      pending.fence = VK_NULL_HANDLE;
    }
    if (pending.command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, cmd_pool_, 1, &pending.command);
      pending.command = VK_NULL_HANDLE;
    }
    if (!cpu_result.success) {
      xpbd::log::warnf(
          "Dynamic sky asynchronous update failed: VkResult=%s(%d) %s; "
          "retaining previous radiance/PDF pair",
          vkResultName(cpu_result.fence_result),
          static_cast<int>(cpu_result.fence_result),
          cpu_result.error.c_str());
      atmosphere_environment_failed_key_ = pending.environment_key;
      destroyImage(atmosphere_environment_spare_cache_);
      atmosphere_environment_spare_cache_ = pending.cache;
      pending.cache = {};
      destroyImage(atmosphere_cloud_history_spare_);
      atmosphere_cloud_history_spare_ = pending.cloud_history;
      pending.cloud_history = {};
      destroyBuffer(atmosphere_environment_distribution_spare_);
      atmosphere_environment_distribution_spare_ = pending.distribution;
      pending.distribution = {};
      destroyBuffer(atmosphere_environment_readback_);
      atmosphere_environment_readback_ = pending.readback;
      pending.readback = {};
      destroyImage(pending.cache);
      destroyImage(pending.cloud_history);
      destroyBuffer(pending.distribution);
      destroyBuffer(pending.readback);
      atmosphere_environment_pending_ = {};
      atmosphere_environment_last_update_ = Clock::now();
      return true;
    }

    const bool retiring_shared_front =
        atmosphere_environment_cache_.image != VK_NULL_HANDLE ||
        atmosphere_environment_distribution_.buffer != VK_NULL_HANDLE;
    if (retiring_shared_front) {
      const std::size_t other_slot =
          (frame_index_ + 1u) % frames_.size();
      atmosphere_environment_spare_retirement_fence_ =
          frames_[other_slot].fence;
    } else {
      atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
    }
    destroyImage(atmosphere_environment_spare_cache_);
    atmosphere_environment_spare_cache_ = atmosphere_environment_cache_;
    atmosphere_environment_cache_ = pending.cache;
    pending.cache = {};
    destroyImage(atmosphere_cloud_history_spare_);
    atmosphere_cloud_history_spare_ = atmosphere_cloud_history_;
    atmosphere_cloud_history_ = pending.cloud_history;
    pending.cloud_history = {};
    destroyBuffer(atmosphere_environment_distribution_spare_);
    atmosphere_environment_distribution_spare_ =
        atmosphere_environment_distribution_;
    atmosphere_environment_distribution_ = pending.distribution;
    pending.distribution = {};
    destroyBuffer(atmosphere_environment_readback_);
    atmosphere_environment_readback_ = pending.readback;
    pending.readback = {};
    atmosphere_environment_distribution_bytes_ = pending.distribution_bytes;
    atmosphere_environment_power_estimate_ =
        cpu_result.environment_power_estimate;
    atmosphere_environment_key_ = pending.environment_key;
    atmosphere_cloud_history_compatibility_key_ =
        pending.cloud_compatibility_key;
    atmosphere_cloud_history_weather_offset_ = pending.weather_offset;
    atmosphere_cloud_history_frame_ = pending.cloud_frame;
    atmosphere_environment_failed_key_.clear();
    atmosphere_environment_ready_ = true;
    atmosphere_environment_last_update_ = Clock::now();
    if (pending.cloud_enabled) {
      xpbd::log::infof(
          "Dynamic cloud temporal cache: %ux%u history=%s frame=%u "
          "previous_frame=%u weather_delta=%.6f/%.6f weight=%.3f "
          "shadow_resolution=%u",
          atmosphere_environment_cache_.width,
          atmosphere_environment_cache_.height,
          pending.cloud_history_valid ? "reprojected" : "reset",
          pending.cloud_frame, pending.previous_cloud_frame,
          pending.cloud_history_parameters[0],
          pending.cloud_history_parameters[1],
          pending.cloud_history_weight, pending.cloud_shadow_resolution);
    }
    xpbd::log::infof(
        "Dynamic sky asynchronous cache ready: %ux%u positive=%llu "
        "table=%llu brightest=%.7g@%u,%u moon_pmf=%.9g "
        "moon_peak=%.7g",
        atmosphere_environment_cache_.width,
        atmosphere_environment_cache_.height,
        static_cast<unsigned long long>(cpu_result.positive_rgb),
        static_cast<unsigned long long>(pending.distribution_bytes),
        cpu_result.brightest_luminance, cpu_result.brightest_x,
        cpu_result.brightest_y, cpu_result.moon_probability,
        cpu_result.moon_peak_luminance);
    xpbd::log::infof(
        "Dynamic sky update perf: cache_compute_ms=%.3f readback_ms=%.3f "
        "distribution_build_ms=%.3f queue_idle_count=0 "
        "cache_realloc_count=%llu",
        cpu_result.cache_compute_ms, cpu_result.readback_ms,
        cpu_result.distribution_build_ms,
        static_cast<unsigned long long>(
            atmosphere_environment_cache_reallocations_));
    atmosphere_environment_pending_ = {};
    return true;
  }

void VulkanBackend::discardDynamicSkyPending() {
    if (!atmosphere_environment_pending_.active()) {
      atmosphere_environment_pending_ = {};
      return;
    }
    (void)pollDynamicSkyEnvironmentCache(true);
    if (!atmosphere_environment_pending_.active()) {
      return;
    }
    // poll(true) only leaves the transaction active when its fence completion
    // could not be established. Intentionally lose the child handles here;
    // vkDestroyDevice owns the final reclamation, while freeing them directly
    // could race work that is still in flight.
    xpbd::log::error(
        "Dynamic sky pending resources quarantined for device teardown");
    atmosphere_environment_pending_ = {};
  }

void VulkanBackend::clearDynamicSkyEnvironmentCache() {
    discardDynamicSkyPending();
    if (quarantine_required_) {
      return;
    }
    destroyImage(atmosphere_environment_cache_);
    destroyImage(atmosphere_cloud_history_);
    destroyImage(atmosphere_environment_spare_cache_);
    destroyImage(atmosphere_cloud_history_spare_);
    destroyBuffer(atmosphere_environment_readback_);
    destroyBuffer(atmosphere_environment_distribution_);
    destroyBuffer(atmosphere_environment_distribution_spare_);
    atmosphere_environment_distribution_bytes_ = 0;
    atmosphere_environment_power_estimate_ = 0.0f;
    atmosphere_environment_key_.clear();
    atmosphere_cloud_history_compatibility_key_.clear();
    atmosphere_cloud_history_weather_offset_ = {};
    atmosphere_cloud_history_frame_ = 0u;
    atmosphere_environment_last_update_ = {};
    atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
    atmosphere_environment_ready_ = false;
  }

void VulkanBackend::clearProceduralAtmosphereImage() {
    clearDynamicSkyEnvironmentCache();
    if (quarantine_required_) {
      return;
    }
    destroyImage(atmosphere_transmittance_);
    destroyImage(atmosphere_scattering_);
    destroyImage(atmosphere_irradiance_);
    atmosphere_resource_key_.clear();
    atmosphere_ready_ = false;
  }

void VulkanBackend::destroyProceduralAtmosphereGpu() {
    clearProceduralAtmosphereImage();
    if (quarantine_required_) {
      return;
    }
    if (atmosphere_environment_cache_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_environment_cache_pipeline_,
                        nullptr);
      atmosphere_environment_cache_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_multiple_scattering_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_multiple_scattering_pipeline_,
                        nullptr);
      atmosphere_multiple_scattering_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_indirect_irradiance_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_indirect_irradiance_pipeline_,
                        nullptr);
      atmosphere_indirect_irradiance_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_scattering_density_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_scattering_density_pipeline_,
                        nullptr);
      atmosphere_scattering_density_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_single_scattering_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_single_scattering_pipeline_,
                        nullptr);
      atmosphere_single_scattering_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_direct_irradiance_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_direct_irradiance_pipeline_,
                        nullptr);
      atmosphere_direct_irradiance_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_transmittance_pipeline_ != VK_NULL_HANDLE) {
      vkDestroyPipeline(device_, atmosphere_transmittance_pipeline_, nullptr);
      atmosphere_transmittance_pipeline_ = VK_NULL_HANDLE;
    }
    if (atmosphere_pipeline_layout_ != VK_NULL_HANDLE) {
      vkDestroyPipelineLayout(device_, atmosphere_pipeline_layout_, nullptr);
      atmosphere_pipeline_layout_ = VK_NULL_HANDLE;
    }
    if (atmosphere_desc_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(device_, atmosphere_desc_pool_, nullptr);
      atmosphere_desc_pool_ = VK_NULL_HANDLE;
      atmosphere_desc_set_ = VK_NULL_HANDLE;
    }
    if (atmosphere_desc_layout_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(device_, atmosphere_desc_layout_, nullptr);
      atmosphere_desc_layout_ = VK_NULL_HANDLE;
    }
    if (atmosphere_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, atmosphere_sampler_, nullptr);
      atmosphere_sampler_ = VK_NULL_HANDLE;
    }
    atmosphere_failed_key_.clear();
    atmosphere_environment_failed_key_.clear();
  }

bool VulkanBackend::ensureProceduralAtmospherePipeline() {
    if (atmosphere_transmittance_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_direct_irradiance_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_single_scattering_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_scattering_density_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_indirect_irradiance_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_multiple_scattering_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_environment_cache_pipeline_ != VK_NULL_HANDLE &&
        atmosphere_pipeline_layout_ != VK_NULL_HANDLE &&
        atmosphere_desc_set_ != VK_NULL_HANDLE &&
        atmosphere_sampler_ != VK_NULL_HANDLE) {
      return true;
    }
    destroyProceduralAtmosphereGpu();

    constexpr std::array<VkDescriptorType, 18> descriptor_types{
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
    };
    std::array<VkDescriptorSetLayoutBinding, descriptor_types.size()>
        bindings{};
    for (std::uint32_t index = 0; index < bindings.size(); ++index) {
      bindings[index].binding = index;
      bindings[index].descriptorType = descriptor_types[index];
      bindings[index].descriptorCount = 1;
      bindings[index].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo descriptor_layout_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    descriptor_layout_info.bindingCount =
        static_cast<std::uint32_t>(bindings.size());
    descriptor_layout_info.pBindings = bindings.data();
    if (vkCreateDescriptorSetLayout(device_, &descriptor_layout_info, nullptr,
                                    &atmosphere_desc_layout_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }

    VkPushConstantRange push_range{};
    push_range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    push_range.offset = 0;
    push_range.size = sizeof(AtmosphereEnvironmentPush);
    VkPipelineLayoutCreateInfo pipeline_layout_info{
        VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pipeline_layout_info.setLayoutCount = 1;
    pipeline_layout_info.pSetLayouts = &atmosphere_desc_layout_;
    pipeline_layout_info.pushConstantRangeCount = 1;
    pipeline_layout_info.pPushConstantRanges = &push_range;
    if (vkCreatePipelineLayout(device_, &pipeline_layout_info, nullptr,
                               &atmosphere_pipeline_layout_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }

    auto create_pipeline = [&](const std::uint32_t *words,
                               std::size_t word_count,
                               VkPipeline &pipeline) {
      VkShaderModule module = makeModule(words, word_count);
      if (module == VK_NULL_HANDLE) {
        return false;
      }
      VkPipelineShaderStageCreateInfo shader_stage{
          VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
      shader_stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
      shader_stage.module = module;
      shader_stage.pName = "main";
      VkComputePipelineCreateInfo pipeline_info{
          VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
      pipeline_info.stage = shader_stage;
      pipeline_info.layout = atmosphere_pipeline_layout_;
      const VkResult result =
          vkCreateComputePipelines(device_, VK_NULL_HANDLE, 1, &pipeline_info,
                                   nullptr, &pipeline);
      vkDestroyShaderModule(device_, module, nullptr);
      return result == VK_SUCCESS;
    };
    if (!create_pipeline(
            kSpvAtmosphereTransmittanceComp,
            sizeof(kSpvAtmosphereTransmittanceComp) / sizeof(std::uint32_t),
            atmosphere_transmittance_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereDirectIrradianceComp,
            sizeof(kSpvAtmosphereDirectIrradianceComp) /
                sizeof(std::uint32_t),
            atmosphere_direct_irradiance_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereSingleScatteringComp,
            sizeof(kSpvAtmosphereSingleScatteringComp) /
                sizeof(std::uint32_t),
            atmosphere_single_scattering_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereScatteringDensityComp,
            sizeof(kSpvAtmosphereScatteringDensityComp) /
                sizeof(std::uint32_t),
            atmosphere_scattering_density_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereIndirectIrradianceComp,
            sizeof(kSpvAtmosphereIndirectIrradianceComp) /
                sizeof(std::uint32_t),
            atmosphere_indirect_irradiance_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereMultipleScatteringComp,
            sizeof(kSpvAtmosphereMultipleScatteringComp) /
                sizeof(std::uint32_t),
            atmosphere_multiple_scattering_pipeline_) ||
        !create_pipeline(
            kSpvAtmosphereEnvironmentCacheComp,
            sizeof(kSpvAtmosphereEnvironmentCacheComp) /
                sizeof(std::uint32_t),
            atmosphere_environment_cache_pipeline_)) {
      destroyProceduralAtmosphereGpu();
      return false;
    }

    std::array<VkDescriptorPoolSize, 2> pool_sizes{{
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 10},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8},
    }};
    VkDescriptorPoolCreateInfo pool_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pool_info.maxSets = 1;
    pool_info.poolSizeCount =
        static_cast<std::uint32_t>(pool_sizes.size());
    pool_info.pPoolSizes = pool_sizes.data();
    if (vkCreateDescriptorPool(device_, &pool_info, nullptr,
                               &atmosphere_desc_pool_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }
    VkDescriptorSetAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    allocate_info.descriptorPool = atmosphere_desc_pool_;
    allocate_info.descriptorSetCount = 1;
    allocate_info.pSetLayouts = &atmosphere_desc_layout_;
    if (vkAllocateDescriptorSets(device_, &allocate_info,
                                 &atmosphere_desc_set_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.maxLod = 0.0f;
    if (vkCreateSampler(device_, &sampler_info, nullptr,
                        &atmosphere_sampler_) != VK_SUCCESS) {
      destroyProceduralAtmosphereGpu();
      return false;
    }
    return true;
  }

bool VulkanBackend::createAtmosphereImage(std::uint32_t width, std::uint32_t height,
                             std::uint32_t depth, ImageResource &out) {
    if (!storage_image_extended_formats_enabled_) {
      xpbd::log::warn(
          "Procedural atmosphere requires "
          "shaderStorageImageExtendedFormats");
      return false;
    }
    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(phys_, VK_FORMAT_R16G16B16A16_SFLOAT,
                                        &format_properties);
    constexpr VkFormatFeatureFlags required_features =
        VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
        VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    constexpr VkImageUsageFlags kUsage =
        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    const VkDeviceSize estimated_bytes =
        static_cast<VkDeviceSize>(width) * height * depth * 8u;
    if ((format_properties.optimalTilingFeatures & required_features) !=
        required_features) {
      logImageResourceError(
          "vkGetPhysicalDeviceFormatProperties",
          VK_ERROR_FORMAT_NOT_SUPPORTED, "procedural-atmosphere-rgba16f",
          VK_FORMAT_R16G16B16A16_SFLOAT, width, height, depth, kUsage,
          estimated_bytes, (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }

    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType =
        depth > 1u ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    image_info.extent = {width, height, depth};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = kUsage;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    const VkResult create_result =
        vkCreateImage(device_, &image_info, nullptr, &out.image);
    if (create_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImage", create_result, "procedural-atmosphere-rgba16f",
          image_info.format, width, height, depth, image_info.usage,
          estimated_bytes, (std::numeric_limits<std::uint32_t>::max)());
      return false;
    }

    VkMemoryRequirements requirements{};
    vkGetImageMemoryRequirements(device_, out.image, &requirements);
    const auto memory_type = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (!memory_type) {
      logImageResourceError(
          "findMemoryType", VK_ERROR_FEATURE_NOT_PRESENT,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size,
          (std::numeric_limits<std::uint32_t>::max)());
      destroyImage(out);
      return false;
    }
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex = *memory_type;
    const VkResult allocation_result =
        vkAllocateMemory(device_, &allocation, nullptr, &out.memory);
    if (allocation_result != VK_SUCCESS) {
      logImageResourceError(
          "vkAllocateMemory", allocation_result,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size, *memory_type);
      destroyImage(out);
      return false;
    }
    const VkResult bind_result =
        vkBindImageMemory(device_, out.image, out.memory, 0);
    if (bind_result != VK_SUCCESS) {
      logImageResourceError(
          "vkBindImageMemory", bind_result,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size, *memory_type);
      destroyImage(out);
      return false;
    }

    VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view_info.image = out.image;
    view_info.viewType =
        depth > 1u ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    const VkResult view_result =
        vkCreateImageView(device_, &view_info, nullptr, &out.view);
    if (view_result != VK_SUCCESS) {
      logImageResourceError(
          "vkCreateImageView", view_result,
          "procedural-atmosphere-rgba16f", image_info.format, width, height,
          depth, image_info.usage, requirements.size, *memory_type);
      destroyImage(out);
      return false;
    }
    out.width = width;
    out.height = height;
    out.depth = depth;
    return true;
  }

bool VulkanBackend::buildProceduralAtmosphereLuts(
      const ResolvedWorldEnvironment &resolved,
      const std::string &resource_key) {
    if (resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
        resolved.atmosphere == nullptr ||
        !resolved.atmosphere->valid() || resource_key.empty()) {
      return false;
    }
    const BrunetonAtmosphereConfig &config = *resolved.atmosphere;
    const AtmosphereLutDimensions &dimensions = config.dimensions;
    const AtmosphereLutDimensions frozen_dimensions{};
    if (config.format != AtmosphereLutFormat::Rgba16Float ||
        dimensions.transmittance_width !=
            frozen_dimensions.transmittance_width ||
        dimensions.transmittance_height !=
            frozen_dimensions.transmittance_height ||
        dimensions.scattering_radial !=
            frozen_dimensions.scattering_radial ||
        dimensions.scattering_view_cosine !=
            frozen_dimensions.scattering_view_cosine ||
        dimensions.scattering_sun_cosine !=
            frozen_dimensions.scattering_sun_cosine ||
        dimensions.scattering_relative_azimuth !=
            frozen_dimensions.scattering_relative_azimuth ||
        dimensions.irradiance_width != frozen_dimensions.irradiance_width ||
        dimensions.irradiance_height != frozen_dimensions.irradiance_height) {
      xpbd::log::warn(
          "Procedural atmosphere rejected: shader/LUT dimensions differ");
      return false;
    }
    if (!ensureProceduralAtmospherePipeline()) {
      xpbd::log::warn("Procedural atmosphere compute pipeline creation failed");
      return false;
    }
    const std::uint32_t scattering_width = dimensions.scatteringWidth();
    const std::uint64_t transmittance_pixels =
        static_cast<std::uint64_t>(dimensions.transmittance_width) *
        dimensions.transmittance_height;
    const std::uint64_t irradiance_pixels =
        static_cast<std::uint64_t>(dimensions.irradiance_width) *
        dimensions.irradiance_height;
    const std::uint64_t scattering_pixels =
        static_cast<std::uint64_t>(scattering_width) *
        dimensions.scattering_view_cosine *
        dimensions.scattering_radial;
    const VkDeviceSize transmittance_bytes =
        static_cast<VkDeviceSize>(transmittance_pixels) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize irradiance_bytes =
        static_cast<VkDeviceSize>(irradiance_pixels) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize scattering_bytes =
        static_cast<VkDeviceSize>(scattering_pixels) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize transmittance_offset = 0;
    const VkDeviceSize irradiance_offset = transmittance_bytes;
    const VkDeviceSize scattering_offset =
        irradiance_offset + irradiance_bytes;
    const VkDeviceSize readback_bytes = scattering_offset + scattering_bytes;
    constexpr VkDeviceSize kMaximumAtmosphereBytes =
        VkDeviceSize{128} * 1024u * 1024u;
    if (readback_bytes == 0u || readback_bytes > kMaximumAtmosphereBytes) {
      xpbd::log::warn("Procedural atmosphere LUT budget exceeded");
      return false;
    }

    ImageResource candidate_transmittance{};
    ImageResource candidate_irradiance{};
    ImageResource candidate_scattering{};
    ImageResource delta_irradiance{};
    ImageResource delta_rayleigh{};
    ImageResource delta_mie{};
    ImageResource scattering_density{};
    ImageResource delta_multiple{};
    Buffer readback{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (gpu_completion_unproven_) {
        return;
      }
      if (command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(readback);
      destroyImage(candidate_transmittance);
      destroyImage(candidate_irradiance);
      destroyImage(candidate_scattering);
      destroyImage(delta_irradiance);
      destroyImage(delta_rayleigh);
      destroyImage(delta_mie);
      destroyImage(scattering_density);
      destroyImage(delta_multiple);
    };
    if (!createAtmosphereImage(dimensions.transmittance_width,
                               dimensions.transmittance_height, 1u,
                               candidate_transmittance) ||
        !createAtmosphereImage(dimensions.irradiance_width,
                               dimensions.irradiance_height, 1u,
                               candidate_irradiance) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               candidate_scattering) ||
        !createAtmosphereImage(dimensions.irradiance_width,
                               dimensions.irradiance_height, 1u,
                               delta_irradiance) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               delta_rayleigh) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial, delta_mie) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               scattering_density) ||
        !createAtmosphereImage(scattering_width,
                               dimensions.scattering_view_cosine,
                               dimensions.scattering_radial,
                               delta_multiple) ||
        !createBuffer(readback_bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      readback)) {
      cleanup();
      return false;
    }

    const std::array<VkImageView, 14> descriptor_views{
        candidate_transmittance.view,
        candidate_transmittance.view,
        delta_irradiance.view,
        candidate_irradiance.view,
        delta_rayleigh.view,
        delta_mie.view,
        candidate_scattering.view,
        delta_rayleigh.view,
        delta_mie.view,
        delta_multiple.view,
        delta_irradiance.view,
        scattering_density.view,
        scattering_density.view,
        delta_multiple.view,
    };
    const auto is_sampled_binding = [](std::uint32_t binding) {
      return binding == 1u || binding == 7u || binding == 8u ||
             binding == 9u || binding == 10u || binding == 12u;
    };
    std::array<VkDescriptorImageInfo, descriptor_views.size()>
        descriptor_images{};
    std::array<VkWriteDescriptorSet, descriptor_views.size()>
        descriptor_writes{};
    for (std::uint32_t binding = 0; binding < descriptor_views.size();
         ++binding) {
      descriptor_images[binding].sampler =
          is_sampled_binding(binding) ? atmosphere_sampler_ : VK_NULL_HANDLE;
      descriptor_images[binding].imageView = descriptor_views[binding];
      descriptor_images[binding].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
      descriptor_writes[binding].sType =
          VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptor_writes[binding].dstSet = atmosphere_desc_set_;
      descriptor_writes[binding].dstBinding = binding;
      descriptor_writes[binding].descriptorType =
          is_sampled_binding(binding)
              ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
              : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
      descriptor_writes[binding].descriptorCount = 1;
      descriptor_writes[binding].pImageInfo =
          &descriptor_images[binding];
    }
    vkUpdateDescriptorSets(
        device_, static_cast<std::uint32_t>(descriptor_writes.size()),
        descriptor_writes.data(), 0, nullptr);

    VkCommandBufferAllocateInfo command_allocate{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    command_allocate.commandPool = cmd_pool_;
    command_allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    command_allocate.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &command_allocate, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }

    const std::array<ImageResource *, 8> all_images{
        &candidate_transmittance, &candidate_irradiance,
        &candidate_scattering,   &delta_irradiance,
        &delta_rayleigh,         &delta_mie,
        &scattering_density,     &delta_multiple,
    };
    std::array<VkImageMemoryBarrier, all_images.size()> initial_barriers{};
    for (std::size_t index = 0; index < all_images.size(); ++index) {
      auto &barrier = initial_barriers[index];
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = all_images[index]->image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.dstAccessMask =
          VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    }
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr,
                         static_cast<std::uint32_t>(initial_barriers.size()),
                         initial_barriers.data());
    VkMemoryBarrier compute_barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    compute_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    compute_barrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    auto separate_compute_passes = [&] {
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1,
                           &compute_barrier, 0, nullptr, 0, nullptr);
    };
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            atmosphere_pipeline_layout_, 0, 1,
                            &atmosphere_desc_set_, 0, nullptr);

    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_transmittance_pipeline_);
    vkCmdDispatch(command, (dimensions.transmittance_width + 7u) / 8u,
                  (dimensions.transmittance_height + 7u) / 8u, 1u);
    separate_compute_passes();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_direct_irradiance_pipeline_);
    vkCmdDispatch(command, (dimensions.irradiance_width + 7u) / 8u,
                  (dimensions.irradiance_height + 7u) / 8u, 1u);
    separate_compute_passes();
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_single_scattering_pipeline_);
    vkCmdDispatch(command, (scattering_width + 3u) / 4u,
                  (dimensions.scattering_view_cosine + 3u) / 4u,
                  (dimensions.scattering_radial + 3u) / 4u);
    separate_compute_passes();
    for (std::uint32_t order = 2u; order <= config.scattering_orders;
         ++order) {
      const std::int32_t density_order = static_cast<std::int32_t>(order);
      vkCmdPushConstants(command, atmosphere_pipeline_layout_,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(density_order), &density_order);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        atmosphere_scattering_density_pipeline_);
      vkCmdDispatch(command, (scattering_width + 3u) / 4u,
                    (dimensions.scattering_view_cosine + 3u) / 4u,
                    (dimensions.scattering_radial + 3u) / 4u);
      separate_compute_passes();

      const std::int32_t irradiance_order =
          static_cast<std::int32_t>(order - 1u);
      vkCmdPushConstants(command, atmosphere_pipeline_layout_,
                         VK_SHADER_STAGE_COMPUTE_BIT, 0,
                         sizeof(irradiance_order), &irradiance_order);
      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        atmosphere_indirect_irradiance_pipeline_);
      vkCmdDispatch(command, (dimensions.irradiance_width + 7u) / 8u,
                    (dimensions.irradiance_height + 7u) / 8u, 1u);
      separate_compute_passes();

      vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                        atmosphere_multiple_scattering_pipeline_);
      vkCmdDispatch(command, (scattering_width + 3u) / 4u,
                    (dimensions.scattering_view_cosine + 3u) / 4u,
                    (dimensions.scattering_radial + 3u) / 4u);
      separate_compute_passes();
    }

    const std::array<ImageResource *, 3> persistent_images{
        &candidate_transmittance, &candidate_irradiance,
        &candidate_scattering};
    std::array<VkImageMemoryBarrier, persistent_images.size()>
        transfer_barriers{};
    for (std::size_t index = 0; index < persistent_images.size(); ++index) {
      auto &barrier = transfer_barriers[index];
      barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      barrier.image = persistent_images[index]->image;
      barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(transfer_barriers.size()),
        transfer_barriers.data());
    std::array<VkBufferImageCopy, 3> copies{};
    copies[0].bufferOffset = transmittance_offset;
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[0].imageExtent = {dimensions.transmittance_width,
                             dimensions.transmittance_height, 1u};
    copies[1].bufferOffset = irradiance_offset;
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[1].imageExtent = {dimensions.irradiance_width,
                             dimensions.irradiance_height, 1u};
    copies[2].bufferOffset = scattering_offset;
    copies[2].imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copies[2].imageExtent = {scattering_width,
                             dimensions.scattering_view_cosine,
                             dimensions.scattering_radial};
    for (std::size_t index = 0; index < persistent_images.size(); ++index) {
      vkCmdCopyImageToBuffer(command, persistent_images[index]->image,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             readback.buffer, 1, &copies[index]);
      transfer_barriers[index].oldLayout =
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      transfer_barriers[index].newLayout =
          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      transfer_barriers[index].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      transfer_barriers[index].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    }
    vkCmdPipelineBarrier(
        command, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr,
        static_cast<std::uint32_t>(transfer_barriers.size()),
        transfer_barriers.data());
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    if (!submitGraphicsTransactionAndWait(command, "atmosphere-luts")) {
      cleanup();
      return false;
    }
    vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
    command = VK_NULL_HANDLE;

    struct HalfValidation {
      bool valid = false;
      std::uint64_t finite_rgb = 0;
      std::uint64_t positive_rgb = 0;
      std::uint64_t intermediate_rgb = 0;
    };
    const auto *half =
        static_cast<const std::uint16_t *>(readback.mapped);
    auto validate_half_image =
        [&](VkDeviceSize byte_offset, std::uint64_t pixel_count,
            bool unit_bounded, bool opaque_alpha) {
          HalfValidation validation;
          if (half == nullptr || (byte_offset % sizeof(std::uint16_t)) != 0u) {
            return validation;
          }
          const std::uint64_t half_offset =
              byte_offset / sizeof(std::uint16_t);
          validation.valid = true;
          for (std::uint64_t pixel = 0;
               validation.valid && pixel < pixel_count; ++pixel) {
            for (std::uint32_t channel = 0; channel < 3u; ++channel) {
              const std::uint16_t value =
                  half[half_offset + pixel * 4u + channel];
              const std::uint16_t magnitude = value & 0x7fffu;
              const bool finite = (magnitude & 0x7c00u) != 0x7c00u;
              const bool nonnegative =
                  (value & 0x8000u) == 0u || magnitude == 0u;
              const bool bounded = !unit_bounded || magnitude <= 0x3c00u;
              if (!finite || !nonnegative || !bounded) {
                validation.valid = false;
                break;
              }
              ++validation.finite_rgb;
              if (magnitude > 0u) {
                ++validation.positive_rgb;
              }
              if (magnitude > 0u && magnitude < 0x3c00u) {
                ++validation.intermediate_rgb;
              }
            }
            const std::uint16_t alpha =
                half[half_offset + pixel * 4u + 3u];
            const std::uint16_t alpha_magnitude = alpha & 0x7fffu;
            const bool alpha_finite =
                (alpha_magnitude & 0x7c00u) != 0x7c00u;
            const bool alpha_nonnegative =
                (alpha & 0x8000u) == 0u || alpha_magnitude == 0u;
            if (!alpha_finite || !alpha_nonnegative ||
                (opaque_alpha && alpha != 0x3c00u)) {
              validation.valid = false;
            }
          }
          validation.valid =
              validation.valid &&
              validation.finite_rgb == pixel_count * 3u &&
              validation.positive_rgb > 0u;
          return validation;
        };
    const HalfValidation transmittance_validation =
        validate_half_image(transmittance_offset, transmittance_pixels,
                            true, true);
    const HalfValidation irradiance_validation =
        validate_half_image(irradiance_offset, irradiance_pixels,
                            false, true);
    const HalfValidation scattering_validation =
        validate_half_image(scattering_offset, scattering_pixels,
                            false, false);
    const bool valid_output =
        transmittance_validation.valid &&
        transmittance_validation.intermediate_rgb > 0u &&
        irradiance_validation.valid && scattering_validation.valid;
    if (!valid_output) {
      xpbd::log::warn(
          "Procedural atmosphere LUT readback validation failed");
      cleanup();
      return false;
    }

    clearProceduralAtmosphereImage();
    atmosphere_transmittance_ = candidate_transmittance;
    candidate_transmittance = {};
    atmosphere_irradiance_ = candidate_irradiance;
    candidate_irradiance = {};
    atmosphere_scattering_ = candidate_scattering;
    candidate_scattering = {};
    atmosphere_resource_key_ = resource_key;
    atmosphere_failed_key_.clear();
    atmosphere_ready_ = true;
    destroyBuffer(readback);
    destroyImage(delta_irradiance);
    destroyImage(delta_rayleigh);
    destroyImage(delta_mie);
    destroyImage(scattering_density);
    destroyImage(delta_multiple);
    xpbd::log::infof(
        "Procedural atmosphere LUTs ready: transmittance=%ux%u "
        "scattering=%ux%ux%u irradiance=%ux%u orders=%u "
        "positive=%llu/%llu/%llu",
        dimensions.transmittance_width, dimensions.transmittance_height,
        scattering_width, dimensions.scattering_view_cosine,
        dimensions.scattering_radial, dimensions.irradiance_width,
        dimensions.irradiance_height, config.scattering_orders,
        static_cast<unsigned long long>(
            transmittance_validation.positive_rgb),
        static_cast<unsigned long long>(scattering_validation.positive_rgb),
        static_cast<unsigned long long>(irradiance_validation.positive_rgb));
    return true;
  }std::string VulkanBackend::proceduralEnvironmentResourceKey(
      const ResolvedWorldEnvironment &resolved) const {
    if (!atmosphere_ready_ || atmosphere_resource_key_.empty() ||
        resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
        resolved.celestial == nullptr || !resolved.celestial->valid) {
      return {};
    }
    std::uint64_t hash = 14695981039346656037ull;
    appendPathTraceHistoryBytes(hash, atmosphere_resource_key_.data(),
                                atmosphere_resource_key_.size());
    appendPathTraceHistoryValue(hash, resolved.rotation_radians);
    appendPathTraceHistoryValue(hash, resolved.background_visible);
    appendPathTraceHistoryValue(hash, resolved.environment_lighting);
    appendPathTraceHistoryValue(hash, resolved.environment_strength);
    appendPathTraceHistoryValue(hash, resolved.background_multiplier);
    appendPathTraceHistoryValue(hash, resolved.sun_moon_lighting);
    appendPathTraceHistoryValue(hash, resolved.celestial->sun.direction);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->sun.angular_diameter_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->sun.geometric_altitude_degrees);
    appendPathTraceHistoryValue(hash, resolved.celestial->moon.direction);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon.angular_diameter_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_phase_angle_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_illuminated_fraction);
    appendPathTraceHistoryValue(hash, resolved.celestial->moon_magnitude);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_libration_latitude_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->moon_libration_longitude_degrees);
    appendPathTraceHistoryValue(hash, resolved.celestial->sidereal_time_hours);
    appendPathTraceHistoryValue(
        hash, static_cast<std::uint8_t>(resolved.celestial->twilight));
    appendPathTraceHistoryValue(
        hash, resolved.celestial->observer.latitude_degrees);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->observer.elevation_meters);
    appendPathTraceHistoryValue(
        hash, resolved.celestial->observer.north_offset_degrees);
    if (resolved.sun == nullptr || resolved.moon == nullptr ||
        resolved.atmosphere_controls == nullptr ||
        resolved.night == nullptr) {
      return {};
    }
    appendPathTraceHistoryValue(hash, resolved.sun->enabled);
    appendPathTraceHistoryValue(hash, resolved.sun->strength);
    appendPathTraceHistoryValue(hash, resolved.sun->direction_mode);
    appendPathTraceHistoryValue(hash,
                                resolved.sun->color_temperature_kelvin);
    appendPathTraceHistoryValue(hash,
                                resolved.sun->angular_diameter_degrees);
    appendPathTraceHistoryValue(hash, resolved.sun->disk_visible);
    appendPathTraceHistoryValue(hash, resolved.sun->cast_shadows);
    appendPathTraceHistoryValue(hash, resolved.moon->enabled);
    appendPathTraceHistoryValue(hash, resolved.moon->strength);
    appendPathTraceHistoryValue(hash, resolved.moon->phase_mode);
    appendPathTraceHistoryValue(
        hash, resolved.moon->manual_illuminated_fraction);
    appendPathTraceHistoryValue(hash, resolved.moon->direction_mode);
    appendPathTraceHistoryValue(hash,
                                resolved.moon->angular_diameter_degrees);
    appendPathTraceHistoryValue(hash, resolved.moon->surface_detail);
    appendPathTraceHistoryValue(hash, resolved.moon->disk_visible);
    appendPathTraceHistoryValue(hash, resolved.moon->cast_shadows);
    appendPathTraceHistoryValue(
        hash, resolved.atmosphere_controls->sky_relative_strength);
    appendPathTraceHistoryValue(hash,
                                resolved.atmosphere_controls->turbidity);
    appendPathTraceHistoryValue(hash, resolved.atmosphere_controls->ozone);
    appendPathTraceHistoryValue(
        hash, resolved.atmosphere_controls->lut_quality);
    appendPathTraceHistoryValue(hash, resolved.night->stars_enabled);
    appendPathTraceHistoryValue(hash, resolved.night->star_intensity);
    appendPathTraceHistoryValue(hash,
                                resolved.night->milky_way_enabled);
    appendPathTraceHistoryValue(hash,
                                resolved.night->milky_way_intensity);
    appendPathTraceHistoryValue(hash, resolved.night->light_pollution);
    appendPathTraceHistoryValue(hash,
                                resolved.night->star_rotation_degrees);
    appendPathTraceHistoryValue(hash, resolved.night->night_fill);
    const VolumetricCloudState disabled_clouds;
    const std::string cloud_key = volumetricCloudCacheKey(
        resolved.clouds != nullptr ? *resolved.clouds : disabled_clouds);
    if (cloud_key.empty()) {
      return {};
    }
    appendPathTraceHistoryBytes(hash, cloud_key.data(), cloud_key.size());
    return std::to_string(hash);
  }

bool VulkanBackend::buildDynamicSkyEnvironmentCache(
      const ResolvedWorldEnvironment &resolved,
      const std::string &environment_key) {
    const float render_ratio =
        resolved.clouds != nullptr
            ? std::clamp(resolved.clouds->render_ratio, 0.25f, 1.0f)
            : 1.0f;
    const std::uint32_t kCacheWidth = static_cast<std::uint32_t>(
        std::lround(2048.0f * render_ratio));
    const std::uint32_t kCacheHeight = static_cast<std::uint32_t>(
        std::lround(1024.0f * render_ratio));
    const std::uint64_t kPixelCount =
        static_cast<std::uint64_t>(kCacheWidth) * kCacheHeight;
    const VkDeviceSize kReadbackBytes =
        static_cast<VkDeviceSize>(kPixelCount) * 4u *
        sizeof(std::uint16_t);
    const VkDeviceSize kDistributionBytes =
        sizeof(WorldEnvironmentGpuHeader) +
        static_cast<VkDeviceSize>(kPixelCount) *
            sizeof(WorldEnvironmentGpuAlias);
    if (!atmosphere_ready_ ||
        atmosphere_environment_cache_pipeline_ == VK_NULL_HANDLE ||
        atmosphere_transmittance_.image == VK_NULL_HANDLE ||
        atmosphere_scattering_.image == VK_NULL_HANDLE ||
        resolved.celestial == nullptr || !resolved.celestial->valid ||
        environment_key.empty() || !ensureWorldEnvironmentSampler()) {
      return false;
    }
    if (atmosphere_environment_pending_.active()) {
      return true;
    }
    if (atmosphere_environment_spare_retirement_fence_ != VK_NULL_HANDLE) {
      const VkResult retirement_result = vkGetFenceStatus(
          device_, atmosphere_environment_spare_retirement_fence_);
      if (retirement_result == VK_NOT_READY) {
        // The old front bundle is still referenced by the other frame slot.
        // Defer without blocking; that slot is waited at the start of its next
        // frame before this function is called again.
        return true;
      }
      if (retirement_result != VK_SUCCESS) {
        xpbd::log::errorf(
            "Dynamic sky retirement fence failed: API=vkGetFenceStatus "
            "VkResult=%s(%d) frame_slot=%u still_job_id=%llu",
            vkResultName(retirement_result),
            static_cast<int>(retirement_result), frame_index_,
            static_cast<unsigned long long>(still_active_job_id_));
        recordFatalVulkanError("vkGetFenceStatus(dynamic_sky_retirement)",
                               retirement_result);
        return false;
      }
      atmosphere_environment_spare_retirement_fence_ = VK_NULL_HANDLE;
    }

    ImageResource candidate_cache{};
    ImageResource candidate_cloud_history{};
    Buffer readback{};
    Buffer pending_distribution{};
    bool candidate_cache_reused = false;
    bool candidate_cloud_history_reused = false;
    bool readback_reused = false;
    bool pending_distribution_reused = false;
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence update_fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (gpu_completion_unproven_) {
        return;
      }
      if (update_fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, update_fence, nullptr);
        update_fence = VK_NULL_HANDLE;
      }
      if (command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      if (readback_reused &&
          atmosphere_environment_readback_.buffer == VK_NULL_HANDLE) {
        atmosphere_environment_readback_ = readback;
        readback = {};
      } else {
        destroyBuffer(readback);
      }
      if (candidate_cache_reused &&
          atmosphere_environment_spare_cache_.image == VK_NULL_HANDLE) {
        atmosphere_environment_spare_cache_ = candidate_cache;
        candidate_cache = {};
      } else {
        destroyImage(candidate_cache);
      }
      if (candidate_cloud_history_reused &&
          atmosphere_cloud_history_spare_.image == VK_NULL_HANDLE) {
        atmosphere_cloud_history_spare_ = candidate_cloud_history;
        candidate_cloud_history = {};
      } else {
        destroyImage(candidate_cloud_history);
      }
      if (pending_distribution_reused &&
          atmosphere_environment_distribution_spare_.buffer ==
              VK_NULL_HANDLE) {
        atmosphere_environment_distribution_spare_ = pending_distribution;
        pending_distribution = {};
      } else {
        destroyBuffer(pending_distribution);
      }
    };
    const auto acquire_image = [&](ImageResource &spare,
                                   ImageResource &candidate,
                                   bool &reused) {
      if (spare.image != VK_NULL_HANDLE && spare.width == kCacheWidth &&
          spare.height == kCacheHeight && spare.depth == 1u) {
        candidate = spare;
        spare = {};
        reused = true;
        return true;
      }
      destroyImage(spare);
      if (!createAtmosphereImage(kCacheWidth, kCacheHeight, 1u, candidate)) {
        return false;
      }
      ++atmosphere_environment_cache_reallocations_;
      return true;
    };
    if (!acquire_image(atmosphere_environment_spare_cache_, candidate_cache,
                       candidate_cache_reused) ||
        !acquire_image(atmosphere_cloud_history_spare_,
                       candidate_cloud_history,
                       candidate_cloud_history_reused)) {
      cleanup();
      return false;
    }
    if (atmosphere_environment_readback_.buffer != VK_NULL_HANDLE &&
        atmosphere_environment_readback_.capacity >= kReadbackBytes) {
      readback = atmosphere_environment_readback_;
      atmosphere_environment_readback_ = {};
      readback_reused = true;
    } else {
      destroyBuffer(atmosphere_environment_readback_);
      if (!createBuffer(kReadbackBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        readback, "dynamic-sky-readback")) {
        cleanup();
        return false;
      }
      ++atmosphere_environment_cache_reallocations_;
    }
    if (atmosphere_environment_distribution_spare_.buffer !=
            VK_NULL_HANDLE &&
        atmosphere_environment_distribution_spare_.capacity >=
            kDistributionBytes) {
      pending_distribution = atmosphere_environment_distribution_spare_;
      atmosphere_environment_distribution_spare_ = {};
      pending_distribution_reused = true;
    } else {
      destroyBuffer(atmosphere_environment_distribution_spare_);
      if (!createBuffer(kDistributionBytes,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                        pending_distribution,
                        "dynamic-sky-environment-distribution")) {
        cleanup();
        return false;
      }
      ++atmosphere_environment_cache_reallocations_;
    }

    VolumetricCloudState cloud_compatibility;
    std::string cloud_compatibility_key;
    std::array<float, 2> current_weather_offset{};
    const std::uint32_t previous_cloud_frame =
        atmosphere_cloud_history_frame_;
    bool cloud_history_valid = false;
    if (resolved.clouds != nullptr) {
      cloud_compatibility = *resolved.clouds;
      cloud_compatibility.time_seconds = 0.0f;
      cloud_compatibility.temporal_frame = 0u;
      cloud_compatibility.generation = 0u;
      cloud_compatibility_key =
          volumetricCloudCacheKey(cloud_compatibility);
      const float advection_hours =
          resolved.clouds->time_seconds / 3600.0f;
      current_weather_offset = {
          resolved.clouds->weather_offset_km[0] +
              resolved.clouds->wind_direction[0] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours,
          resolved.clouds->weather_offset_km[1] +
              resolved.clouds->wind_direction[1] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours};
      cloud_history_valid =
          resolved.clouds->reprojection &&
          atmosphere_cloud_history_.view != VK_NULL_HANDLE &&
          atmosphere_cloud_history_.width == kCacheWidth &&
          atmosphere_cloud_history_.height == kCacheHeight &&
          !cloud_compatibility_key.empty() &&
          cloud_compatibility_key ==
              atmosphere_cloud_history_compatibility_key_;
    }

    std::array<VkDescriptorImageInfo, 5> descriptor_images{};
    descriptor_images[0].sampler = atmosphere_sampler_;
    descriptor_images[0].imageView = atmosphere_transmittance_.view;
    descriptor_images[0].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_images[1].sampler = atmosphere_sampler_;
    descriptor_images[1].imageView = atmosphere_scattering_.view;
    descriptor_images[1].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_images[2].imageView = candidate_cache.view;
    descriptor_images[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    descriptor_images[3].sampler = atmosphere_sampler_;
    descriptor_images[3].imageView =
        cloud_history_valid ? atmosphere_cloud_history_.view
                            : atmosphere_transmittance_.view;
    descriptor_images[3].imageLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    descriptor_images[4].imageView = candidate_cloud_history.view;
    descriptor_images[4].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    constexpr std::array<std::uint32_t, 5> kBindings{
        1u, 14u, 15u, 16u, 17u};
    constexpr std::array<VkDescriptorType, 5> kTypes{
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE};
    std::array<VkWriteDescriptorSet, 5> writes{};
    for (std::size_t index = 0; index < writes.size(); ++index) {
      writes[index].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      writes[index].dstSet = atmosphere_desc_set_;
      writes[index].dstBinding = kBindings[index];
      writes[index].descriptorType = kTypes[index];
      writes[index].descriptorCount = 1;
      writes[index].pImageInfo = &descriptor_images[index];
    }
    vkUpdateDescriptorSets(device_, static_cast<std::uint32_t>(writes.size()),
                           writes.data(), 0, nullptr);

    VkCommandBufferAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = cmd_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate_info, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }

    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = candidate_cache_reused
                            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = candidate_cache.image;
    barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    barrier.srcAccessMask = candidate_cache_reused
                                ? VK_ACCESS_SHADER_READ_BIT
                                : 0u;
    barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    VkImageMemoryBarrier cloud_history_barrier = barrier;
    cloud_history_barrier.image = candidate_cloud_history.image;
    cloud_history_barrier.oldLayout =
        candidate_cloud_history_reused
            ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
    cloud_history_barrier.srcAccessMask =
        candidate_cloud_history_reused ? VK_ACCESS_SHADER_READ_BIT : 0u;
    const std::array<VkImageMemoryBarrier, 2> output_barriers{
        barrier, cloud_history_barrier};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr,
                         static_cast<std::uint32_t>(output_barriers.size()),
                         output_barriers.data());
    vkCmdBindPipeline(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                      atmosphere_environment_cache_pipeline_);
    vkCmdBindDescriptorSets(command, VK_PIPELINE_BIND_POINT_COMPUTE,
                            atmosphere_pipeline_layout_, 0, 1,
                            &atmosphere_desc_set_, 0, nullptr);
    constexpr double kRadiansPerDegree =
        3.14159265358979323846 / 180.0;
    if (resolved.sun == nullptr || resolved.moon == nullptr ||
        resolved.atmosphere_controls == nullptr ||
        resolved.night == nullptr) {
      cleanup();
      return false;
    }
    const SunControls &sun_controls = *resolved.sun;
    const MoonControls &moon_controls = *resolved.moon;
    const AtmosphereControls &atmosphere_controls =
        *resolved.atmosphere_controls;
    const NightSkyControls &night_controls = *resolved.night;
    AtmosphereEnvironmentPush environment_push;
    environment_push.sun_direction_observer_height = {
        static_cast<float>(resolved.celestial->sun.direction[0]),
        static_cast<float>(resolved.celestial->sun.direction[1]),
        static_cast<float>(resolved.celestial->sun.direction[2]),
        static_cast<float>((std::max)(
            resolved.celestial->observer.elevation_meters / 1000.0, 0.001))};
    environment_push.moon_direction_angular_radius = {
        static_cast<float>(resolved.celestial->moon.direction[0]),
        static_cast<float>(resolved.celestial->moon.direction[1]),
        static_cast<float>(resolved.celestial->moon.direction[2]),
        std::clamp(moon_controls.angular_diameter_degrees, 0.05f, 5.0f) *
            0.5f * static_cast<float>(kRadiansPerDegree)};
    const float moon_fraction =
        moon_controls.phase_mode == MoonPhaseMode::Manual
            ? std::clamp(moon_controls.manual_illuminated_fraction,
                         0.0f, 1.0f)
            : static_cast<float>(
                  resolved.celestial->moon_illuminated_fraction);
    const float moon_phase_radians =
        moon_controls.phase_mode == MoonPhaseMode::Manual
            ? std::acos(std::clamp(2.0f * moon_fraction - 1.0f,
                                   -1.0f, 1.0f))
            : static_cast<float>(
                  resolved.celestial->moon_phase_angle_degrees *
                  kRadiansPerDegree);
    environment_push.moon_phase_libration = {
        moon_fraction, moon_phase_radians,
        static_cast<float>(
            resolved.celestial->moon_libration_latitude_degrees *
            kRadiansPerDegree),
        static_cast<float>(
            resolved.celestial->moon_libration_longitude_degrees *
            kRadiansPerDegree)};
    environment_push.observer_sidereal_twilight = {
        static_cast<float>(resolved.celestial->observer.latitude_degrees *
                           kRadiansPerDegree),
        static_cast<float>(resolved.celestial->observer.north_offset_degrees *
                           kRadiansPerDegree),
        static_cast<float>(
            (resolved.celestial->sidereal_time_hours * 15.0 +
             night_controls.star_rotation_degrees) *
            kRadiansPerDegree),
        static_cast<float>(
            resolved.celestial->sun.geometric_altitude_degrees)};
    const float light_pollution_attenuation =
        std::exp2(-std::clamp(night_controls.light_pollution, 0.0f, 16.0f));
    environment_push.night_parameters = {
        std::clamp(sun_controls.angular_diameter_degrees, 0.05f, 5.0f) *
            0.5f * static_cast<float>(kRadiansPerDegree),
        static_cast<float>(resolved.celestial->moon_magnitude), 1.0f, 1.0f};
    environment_push.night_parameters[2] =
        night_controls.stars_enabled
            ? std::clamp(night_controls.star_intensity, 0.0f, 32.0f) *
                  light_pollution_attenuation
            : 0.0f;
    environment_push.night_parameters[3] =
        night_controls.milky_way_enabled
            ? std::clamp(night_controls.milky_way_intensity, 0.0f, 32.0f) *
                  light_pollution_attenuation
            : 0.0f;
    environment_push.sky_energy = {
        std::clamp(atmosphere_controls.sky_relative_strength, 0.0f, 8.0f),
        std::clamp(sun_controls.strength, 0.0f, 32.0f),
        std::clamp(moon_controls.strength, 0.0f, 32.0f),
        std::clamp(night_controls.night_fill, 0.0f, 4.0f)};
    std::uint32_t sky_flags = 0u;
    sky_flags |= sun_controls.enabled ? 1u : 0u;
    sky_flags |= moon_controls.enabled ? 2u : 0u;
    sky_flags |=
        moon_controls.phase_mode == MoonPhaseMode::Manual ? 4u : 0u;
    environment_push.sky_flags = {
        sky_flags,
        std::bit_cast<std::uint32_t>(
            std::clamp(moon_controls.surface_detail, 0.0f, 1.0f)),
        static_cast<std::uint32_t>(resolved.debug_view), 0u};
    if (resolved.clouds != nullptr) {
      const float advection_hours = resolved.clouds->time_seconds / 3600.0f;
      environment_push.cloud_layer = {
          1.0f, resolved.clouds->coverage, resolved.clouds->density,
          resolved.clouds->base_altitude_km};
      environment_push.cloud_weather = {
          resolved.clouds->thickness_km,
          resolved.clouds->weather_offset_km[0] +
              resolved.clouds->wind_direction[0] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours,
          resolved.clouds->weather_offset_km[1] +
              resolved.clouds->wind_direction[1] *
                  resolved.clouds->wind_speed_km_per_hour *
                  advection_hours,
          resolved.clouds->time_seconds};
      environment_push.cloud_quality = {
          resolved.clouds->seed, resolved.clouds->ray_steps,
          resolved.clouds->light_steps, resolved.clouds->temporal_frame};
      environment_push.cloud_optics = {
          resolved.clouds->weather_scale,
          resolved.clouds->base_shape_scale,
          resolved.clouds->detail_scale, resolved.clouds->erosion};
      environment_push.cloud_lighting = {
          resolved.clouds->forward_scattering,
          resolved.clouds->silver_lining, resolved.clouds->absorption,
          resolved.clouds->multiple_scattering};
      environment_push.cloud_post = {
          resolved.clouds->shadow_strength,
          resolved.clouds->lighting_strength,
          resolved.clouds->render_ratio,
          resolved.clouds->history_weight};
      environment_push.cloud_history = {
          current_weather_offset[0] -
              atmosphere_cloud_history_weather_offset_[0],
          current_weather_offset[1] -
              atmosphere_cloud_history_weather_offset_[1],
          cloud_history_valid ? 1.0f : 0.0f,
          static_cast<float>(resolved.clouds->shadow_resolution)};
    }
    vkCmdPushConstants(command, atmosphere_pipeline_layout_,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(environment_push), &environment_push);
    vkCmdDispatch(command, (kCacheWidth + 7u) / 8u,
                  (kCacheHeight + 7u) / 8u, 1u);

    barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    cloud_history_barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    cloud_history_barrier.newLayout =
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cloud_history_barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    cloud_history_barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const std::array<VkImageMemoryBarrier, 2> post_compute_barriers{
        barrier, cloud_history_barrier};
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT |
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr,
                         static_cast<std::uint32_t>(
                             post_compute_barriers.size()),
                         post_compute_barriers.data());
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {kCacheWidth, kCacheHeight, 1u};
    vkCmdCopyImageToBuffer(command, candidate_cache.image,
                           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buffer, 1, &copy);
    VkBufferMemoryBarrier readback_barrier{
        VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    readback_barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    readback_barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    readback_barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    readback_barrier.buffer = readback.buffer;
    readback_barrier.offset = 0u;
    readback_barrier.size = kReadbackBytes;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_HOST_BIT |
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 1, &readback_barrier, 1, &barrier);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &command;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    if (vkCreateFence(device_, &fence_info, nullptr, &update_fence) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    const auto cache_compute_begin = Clock::now();
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1, &submit, update_fence);
    if (submit_result != VK_SUCCESS) {
      xpbd::log::warnf(
          "Dynamic sky cache submit failed: API=vkQueueSubmit "
          "VkResult=%s(%d) resource=dynamic-sky-cache extent=%ux%u "
          "frame_slot=%u still_job_id=%llu",
          vkResultName(submit_result), static_cast<int>(submit_result),
          kCacheWidth, kCacheHeight, frame_index_,
          static_cast<unsigned long long>(still_active_job_id_));
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueSubmit(dynamic_sky)", submit_result);
      }
      cleanup();
      return false;
    }

    DynamicSkyCpuInput cpu_input;
    cpu_input.readback =
        static_cast<const std::uint16_t *>(readback.mapped);
    cpu_input.distribution_mapped = pending_distribution.mapped;
    cpu_input.distribution_capacity = pending_distribution.capacity;
    cpu_input.width = kCacheWidth;
    cpu_input.height = kCacheHeight;
    cpu_input.celestial = *resolved.celestial;
    cpu_input.sun = resolveSunLight(resolved);
    cpu_input.moon = moon_controls;
    cpu_input.background_visible = resolved.background_visible;
    cpu_input.environment_lighting = resolved.environment_lighting;
    cpu_input.sun_moon_lighting = resolved.sun_moon_lighting;
    cpu_input.environment_strength = resolved.environment_strength;
    cpu_input.background_multiplier = resolved.background_multiplier;
    cpu_input.rotation_radians = resolved.rotation_radians;
    cpu_input.moon_fraction = moon_fraction;
    cpu_input.moon_phase_radians = moon_phase_radians;

    DynamicSkyPending pending;
    pending.cache = candidate_cache;
    candidate_cache = {};
    pending.cloud_history = candidate_cloud_history;
    candidate_cloud_history = {};
    pending.readback = readback;
    readback = {};
    pending.distribution = pending_distribution;
    pending_distribution = {};
    pending.command = command;
    command = VK_NULL_HANDLE;
    pending.fence = update_fence;
    update_fence = VK_NULL_HANDLE;
    pending.environment_key = environment_key;
    pending.cloud_compatibility_key = std::move(cloud_compatibility_key);
    pending.weather_offset = current_weather_offset;
    pending.cloud_history_parameters = environment_push.cloud_history;
    pending.cloud_frame =
        resolved.clouds != nullptr ? resolved.clouds->temporal_frame : 0u;
    pending.previous_cloud_frame = previous_cloud_frame;
    pending.cloud_history_weight =
        resolved.clouds != nullptr ? resolved.clouds->history_weight : 0.0f;
    pending.cloud_shadow_resolution =
        resolved.clouds != nullptr ? resolved.clouds->shadow_resolution : 0u;
    pending.cloud_enabled = resolved.clouds != nullptr;
    pending.cloud_history_valid = cloud_history_valid;
    pending.distribution_bytes = kDistributionBytes;
    pending.submitted_at = cache_compute_begin;
    atmosphere_environment_pending_ = std::move(pending);
    try {
      const VkDevice worker_device = device_;
      const VkFence worker_fence = atmosphere_environment_pending_.fence;
      const std::shared_ptr<RenderThreadControl> worker_control =
          render_thread_control_;
      atmosphere_environment_pending_.completion = std::async(
          std::launch::async,
          [cpu_input, worker_device, worker_fence,
           cache_compute_begin, worker_control]() mutable {
            return buildDynamicSkyDistribution(
                cpu_input, worker_device, worker_fence,
                cache_compute_begin, worker_control);
          });
    } catch (const std::exception &exception) {
      xpbd::log::errorf(
          "Dynamic sky worker launch failed: %s; waiting only to reclaim "
          "submitted resources safely",
          exception.what());
      (void)pollDynamicSkyEnvironmentCache(true);
      return false;
    }
    atmosphere_environment_last_update_ = cache_compute_begin;
    xpbd::log::infof(
        "Dynamic sky asynchronous update queued: %ux%u "
        "queue_idle_count=0 cache_realloc_count=%llu",
        kCacheWidth, kCacheHeight,
        static_cast<unsigned long long>(
            atmosphere_environment_cache_reallocations_));
    return true;
  }

bool VulkanBackend::ensureProceduralAtmosphereResources(
      const ResolvedWorldEnvironment &resolved) {
    if (resolved.sky_rendering != SkyRendering::ProceduralDayNight ||
        resolved.atmosphere == nullptr) {
      if (atmosphere_transmittance_.image != VK_NULL_HANDLE ||
          atmosphere_transmittance_pipeline_ != VK_NULL_HANDLE) {
        if (submitGraphicsTransactionAndWait(
                VK_NULL_HANDLE, "procedural-atmosphere-clear")) {
          destroyProceduralAtmosphereGpu();
        } else {
          return false;
        }
      }
      atmosphere_failed_key_.clear();
      return false;
    }
    (void)pollDynamicSkyEnvironmentCache(false);
    if (fatal_error_) {
      return false;
    }
    const std::string resource_key =
        brunetonAtmosphereCacheKey(*resolved.atmosphere);
    if (resource_key.empty()) {
      return false;
    }
    if (atmosphere_ready_ && atmosphere_resource_key_ == resource_key) {
      // Reuse the static physical LUTs and update only the dynamic sky cache.
    } else {
      // Static LUT rebuild updates the shared atmosphere descriptor set. It is
      // rare and must not race an in-flight dynamic-cache dispatch.
      if (atmosphere_environment_pending_.active()) {
        (void)pollDynamicSkyEnvironmentCache(true);
        if (fatal_error_ || atmosphere_environment_pending_.active()) {
          return false;
        }
      }
      if (atmosphere_failed_key_ == resource_key) {
        return false;
      }
      if (!buildProceduralAtmosphereLuts(resolved, resource_key)) {
        atmosphere_failed_key_ = resource_key;
        xpbd::log::warn(
            "Procedural atmosphere GPU precomputation failed; resolving Off");
        return false;
      }
    }
    const std::string environment_key =
        proceduralEnvironmentResourceKey(resolved);
    if (environment_key.empty()) {
      return false;
    }
    if (atmosphere_environment_ready_ &&
        atmosphere_environment_key_ == environment_key) {
      return true;
    }
    if (atmosphere_environment_pending_.active()) {
      return atmosphere_environment_ready_;
    }
    const float requested_render_ratio =
        resolved.clouds != nullptr
            ? std::clamp(resolved.clouds->render_ratio, 0.25f, 1.0f)
            : 1.0f;
    const std::uint32_t requested_width = static_cast<std::uint32_t>(
        std::lround(2048.0f * requested_render_ratio));
    const std::uint32_t requested_height = static_cast<std::uint32_t>(
        std::lround(1024.0f * requested_render_ratio));
    constexpr auto kMinimumDynamicSkyUpdateInterval =
        std::chrono::milliseconds(100);
    if (atmosphere_environment_ready_ &&
        atmosphere_environment_cache_.width == requested_width &&
        atmosphere_environment_cache_.height == requested_height &&
        atmosphere_environment_last_update_ != Clock::time_point{} &&
        Clock::now() - atmosphere_environment_last_update_ <
            kMinimumDynamicSkyUpdateInterval) {
      return true;
    }
    if (atmosphere_environment_failed_key_ == environment_key) {
      return atmosphere_environment_ready_;
    }
    if (!buildDynamicSkyEnvironmentCache(resolved, environment_key)) {
      if (fatal_error_) {
        return false;
      }
      atmosphere_environment_failed_key_ = environment_key;
      xpbd::log::warn(
          "Dynamic sky environment cache failed; retaining previous cache");
      return atmosphere_environment_ready_;
    }
    return atmosphere_environment_ready_;
  }

void VulkanBackend::clearWorldEnvironmentResources() {
    destroyImage(world_environment_texture_);
    destroyBuffer(world_environment_distribution_);
    world_environment_distribution_bytes_ = 0;
    world_environment_power_estimate_ = 0.0f;
    world_environment_resource_key_ = 0;
    world_environment_ready_ = false;
    world_environment_runtime_asset_ = {};
    world_environment_published_ = {};
  }

void VulkanBackend::destroyWorldEnvironmentRetired() noexcept {
    WorldEnvironmentRetired &retired = world_environment_retired_;
    if (retired.completion_fence != VK_NULL_HANDLE) {
      return;
    }
    destroyImage(retired.texture);
    destroyBuffer(retired.distribution);
    if (diagnostics_enabled_ && retired.resource_key != 0u) {
      xpbd::log::infof(
          "VKDIAG environment_retirement_reclaimed generation=%llu "
          "elapsed_ms=%.4f",
          static_cast<unsigned long long>(retired.generation),
          std::chrono::duration<double, std::milli>(
              Clock::now() - retired.submitted_at)
              .count());
    }
    retired = {};
  }

bool VulkanBackend::pollWorldEnvironmentRetirement(
    bool wait_for_completion, bool &complete) {
    complete = !world_environment_retired_.active();
    if (complete) {
      return true;
    }
    if (gpu_completion_unproven_ || quarantine_required_) {
      return false;
    }
    VkResult status = VK_NOT_READY;
    if (wait_for_completion) {
      const ControlledWaitResult wait = waitForFenceControlled(
          world_environment_retired_.completion_fence,
          "vkWaitForFences.environment_retirement", UINT32_MAX,
          VK_NULL_HANDLE, VK_NULL_HANDLE, true, true);
      if (!wait.completed()) {
        return false;
      }
      status = VK_SUCCESS;
    } else {
      status = vkGetFenceStatus(
          device_, world_environment_retired_.completion_fence);
      if (status == VK_NOT_READY) {
        return true;
      }
    }
    if (status != VK_SUCCESS) {
      markGpuCompletionUnproven(
          "vkGetFenceStatus.environment_retirement");
      return false;
    }
    vkDestroyFence(device_,
                   world_environment_retired_.completion_fence, nullptr);
    world_environment_retired_.completion_fence = VK_NULL_HANDLE;
    destroyWorldEnvironmentRetired();
    complete = true;
    return true;
  }

void VulkanBackend::discardWorldEnvironmentPending(
    const char *reason) noexcept {
    WorldEnvironmentPending &pending = world_environment_pending_;
    if (!pending.active || gpu_completion_unproven_) {
      return;
    }
    if (pending.upload_fence != VK_NULL_HANDLE) {
      const ControlledWaitResult wait = waitForFenceControlled(
          pending.upload_fence,
          "vkWaitForFences.environment_pending_discard", UINT32_MAX,
          VK_NULL_HANDLE, pending.upload_command, true, true);
      if (!wait.completed()) {
        markGpuCompletionUnproven(
            "vkWaitForFences.environment_pending_discard");
        return;
      }
      vkDestroyFence(device_, pending.upload_fence, nullptr);
      pending.upload_fence = VK_NULL_HANDLE;
    }
    if (pending.upload_command != VK_NULL_HANDLE) {
      vkFreeCommandBuffers(device_, cmd_pool_, 1,
                           &pending.upload_command);
      pending.upload_command = VK_NULL_HANDLE;
    }
    destroyBuffer(pending.staging);
    if (pending.retirement_fence != VK_NULL_HANDLE) {
      const ControlledWaitResult wait = waitForFenceControlled(
          pending.retirement_fence,
          "vkWaitForFences.environment_pending_retirement_discard",
          UINT32_MAX, VK_NULL_HANDLE, VK_NULL_HANDLE, true, true);
      if (!wait.completed()) {
        markGpuCompletionUnproven(
            "vkWaitForFences.environment_pending_retirement_discard");
        return;
      }
      vkDestroyFence(device_, pending.retirement_fence, nullptr);
      pending.retirement_fence = VK_NULL_HANDLE;
    }
    destroyImage(pending.texture);
    destroyBuffer(pending.distribution);
    if (diagnostics_enabled_) {
      xpbd::log::infof(
          "VKDIAG environment_pending_discard generation=%llu reason=%s",
          static_cast<unsigned long long>(pending.generation),
          reason != nullptr ? reason : "unspecified");
    }
    pending = {};
  }

bool VulkanBackend::pollWorldEnvironmentPending(
    bool wait_for_completion, bool &ready, bool &superseded) {
    ready = false;
    superseded = false;
    WorldEnvironmentPending &pending = world_environment_pending_;
    if (!pending.active) {
      ready = true;
      return true;
    }
    if (gpu_completion_unproven_ || quarantine_required_) {
      return false;
    }
    if (pending.upload_fence != VK_NULL_HANDLE) {
      if (wait_for_completion) {
        const ControlledWaitResult wait = waitForFenceControlled(
            pending.upload_fence, "vkWaitForFences.environment_pending",
            UINT32_MAX, VK_NULL_HANDLE, pending.upload_command, true, true);
        if (!wait.completed()) {
          return false;
        }
      } else {
        const VkResult status =
            vkGetFenceStatus(device_, pending.upload_fence);
        if (status == VK_NOT_READY) {
          return true;
        }
        if (status != VK_SUCCESS) {
          markGpuCompletionUnproven(
              "vkGetFenceStatus.environment_pending");
          return false;
        }
      }
      vkDestroyFence(device_, pending.upload_fence, nullptr);
      pending.upload_fence = VK_NULL_HANDLE;
      if (pending.upload_command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1,
                             &pending.upload_command);
        pending.upload_command = VK_NULL_HANDLE;
      }
      destroyBuffer(pending.staging);
    }
    if (pending.superseded) {
      discardWorldEnvironmentPending("environment-candidate-superseded");
      if (gpu_completion_unproven_ || world_environment_pending_.active) {
        return false;
      }
      ready = true;
      superseded = true;
      return true;
    }
    ready = true;
    return true;
  }

bool VulkanBackend::beginWorldEnvironmentRetirement() {
    WorldEnvironmentPending &pending = world_environment_pending_;
    const bool have_published_owner =
        world_environment_texture_.image != VK_NULL_HANDLE ||
        world_environment_distribution_.buffer != VK_NULL_HANDLE;
    if (!have_published_owner) {
      return true;
    }
    if (world_environment_retired_.active()) {
      writeLog("Vulkan environment retirement slot is still active");
      return false;
    }
    if (pending.retirement_fence != VK_NULL_HANDLE) {
      return true;
    }
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence fence = VK_NULL_HANDLE;
    const VkResult create_result =
        vkCreateFence(device_, &fence_info, nullptr, &fence);
    if (create_result != VK_SUCCESS) {
      if (create_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError(
            "vkCreateFence.environment_retirement", create_result);
      } else {
        writeLog("Vulkan environment retirement fence creation failed");
      }
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1u, &submit, fence);
    if (submit_result != VK_SUCCESS) {
      vkDestroyFence(device_, fence, nullptr);
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError(
            "vkQueueSubmit.environment_retirement", submit_result);
      } else {
        writeLog("Vulkan environment retirement marker submission failed");
      }
      return false;
    }
    pending.retirement_fence = fence;
    if (diagnostics_enabled_) {
      xpbd::log::infof(
          "VKDIAG environment_retirement_submitted generation=%llu "
          "fence=0x%llx",
          static_cast<unsigned long long>(
              world_environment_published_.generation),
          static_cast<unsigned long long>(
              reinterpret_cast<std::uintptr_t>(fence)));
    }
    return true;
  }

bool VulkanBackend::commitWorldEnvironmentPending(
    std::uint64_t &uploaded_bytes) {
    uploaded_bytes = 0u;
    WorldEnvironmentPending &pending = world_environment_pending_;
    if (!pending.active || pending.upload_fence != VK_NULL_HANDLE ||
        pending.upload_command != VK_NULL_HANDLE) {
      return false;
    }
    const bool have_published_owner =
        world_environment_texture_.image != VK_NULL_HANDLE ||
        world_environment_distribution_.buffer != VK_NULL_HANDLE;
    if (have_published_owner &&
        pending.retirement_fence == VK_NULL_HANDLE) {
      return false;
    }
    if (have_published_owner) {
      WorldEnvironmentRetired &retired = world_environment_retired_;
      retired.texture = world_environment_texture_;
      retired.distribution = world_environment_distribution_;
      retired.completion_fence = pending.retirement_fence;
      retired.runtime_asset = std::move(world_environment_runtime_asset_);
      retired.published = std::move(world_environment_published_);
      retired.published.hdr = retired.runtime_asset.valid()
                                  ? &retired.runtime_asset
                                  : nullptr;
      retired.resource_key = world_environment_resource_key_;
      retired.generation = retired.published.generation;
      retired.submitted_at = Clock::now();
      pending.retirement_fence = VK_NULL_HANDLE;
      world_environment_texture_ = {};
      world_environment_distribution_ = {};
    } else {
      clearWorldEnvironmentResources();
    }

    world_environment_texture_ = pending.texture;
    world_environment_distribution_ = pending.distribution;
    pending.texture = {};
    pending.distribution = {};
    world_environment_distribution_bytes_ = pending.distribution_bytes;
    world_environment_power_estimate_ = pending.power_estimate;
    world_environment_runtime_asset_ = std::move(pending.runtime_asset);
    world_environment_published_ = std::move(pending.published);
    world_environment_published_.hdr = &world_environment_runtime_asset_;
    world_environment_resource_key_ = pending.resource_key;
    world_environment_failed_key_ = 0u;
    world_environment_ready_ = true;
    uploaded_bytes = pending.uploaded_bytes;
    xpbd::log::infof(
        "World HDRI GPU ready: source=%ux%u requested=%u resolved=%ux%u "
        "mips=%u image=%llu table=%llu peak_budget=%llu generation=%llu",
        pending.source_width, pending.source_height,
        world_environment_published_.requested_hdri_runtime_width,
        pending.runtime_width, pending.runtime_height,
        pending.mip_levels,
        static_cast<unsigned long long>(pending.gpu_image_bytes),
        static_cast<unsigned long long>(pending.distribution_bytes),
        static_cast<unsigned long long>(pending.budget.total_bytes),
        static_cast<unsigned long long>(pending.generation));
    if (diagnostics_enabled_) {
      xpbd::log::infof(
          "VKDIAG environment_pending_commit generation=%llu bytes=%llu "
          "elapsed_ms=%.4f",
          static_cast<unsigned long long>(pending.generation),
          static_cast<unsigned long long>(uploaded_bytes),
          std::chrono::duration<double, std::milli>(
              Clock::now() - pending.submitted_at)
              .count());
    }
    pending = {};
    return true;
  }

void VulkanBackend::destroyWorldEnvironmentGpu() {
    discardWorldEnvironmentPending("environment-resource-destroy");
    if (gpu_completion_unproven_ || world_environment_pending_.active) {
      return;
    }
    bool retirement_complete = false;
    if (!pollWorldEnvironmentRetirement(true, retirement_complete) ||
        !retirement_complete) {
      return;
    }
    clearWorldEnvironmentResources();
    if (world_environment_sampler_ != VK_NULL_HANDLE) {
      vkDestroySampler(device_, world_environment_sampler_, nullptr);
      world_environment_sampler_ = VK_NULL_HANDLE;
    }
    world_environment_failed_key_ = 0;
  }

bool VulkanBackend::submitGraphicsTransactionAndWait(
      VkCommandBuffer command, const char *resource) {
    const char *label = resource != nullptr ? resource : "graphics-transaction";
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkResult fence_result =
        vkCreateFence(device_, &fence_info, nullptr, &fence);
    if (fence_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Vulkan transaction fence creation failed: resource=%s "
          "VkResult=%s(%d)",
          label, vkResultName(fence_result), static_cast<int>(fence_result));
      if (fence_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkCreateFence(graphics_transaction)",
                               fence_result);
      }
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    if (command != VK_NULL_HANDLE) {
      submit.commandBufferCount = 1u;
      submit.pCommandBuffers = &command;
    }
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1u, &submit, fence);
    if (submit_result != VK_SUCCESS) {
      xpbd::log::errorf(
          "Vulkan transaction submit failed: resource=%s "
          "VkResult=%s(%d)",
          label, vkResultName(submit_result),
          static_cast<int>(submit_result));
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError("vkQueueSubmit(graphics_transaction)",
                               submit_result);
      }
      vkDestroyFence(device_, fence, nullptr);
      return false;
    }
    const std::string wait_stage =
        std::string("vkWaitForFences.") + label;
    const ControlledWaitResult wait = waitForFenceControlled(
        fence, wait_stage.c_str(), UINT32_MAX, VK_NULL_HANDLE, command, true,
        true);
    if (!wait.completed()) {
      xpbd::log::errorf(
          "Vulkan transaction fence wait failed: resource=%s "
          "VkResult=%s(%d)",
          label, vkResultName(wait.result), static_cast<int>(wait.result));
      // A non-successful fence wait does not prove completion. The fence and
      // every submitted transaction object remain owned by the quarantined
      // backend and must not be destroyed here.
      return false;
    }
    vkDestroyFence(device_, fence, nullptr);
    return true;
  }

std::uint64_t VulkanBackend::worldEnvironmentResourceKey(
      const ResolvedWorldEnvironment &resolved) const {
    std::uint64_t key = 14695981039346656037ull;
    appendPathTraceHistoryValue(
        key, static_cast<std::uint32_t>(resolved.sky_rendering));
    appendPathTraceHistoryValue(key, resolved.background_visible);
    appendPathTraceHistoryValue(key, resolved.environment_lighting);
    appendPathTraceHistoryValue(key, resolved.environment_strength);
    appendPathTraceHistoryValue(key, resolved.background_exposure);
    appendPathTraceHistoryValue(key, resolved.background_multiplier);
    appendPathTraceHistoryValue(key, resolved.rotation_radians);
    appendPathTraceHistoryValue(key, resolved.hdri_runtime_generation);
    appendPathTraceHistoryValue(key,
                                resolved.requested_hdri_runtime_width);
    appendPathTraceHistoryValue(key,
                                resolved.resolved_hdri_runtime_width);
    appendPathTraceHistoryValue(key,
                                resolved.resolved_hdri_runtime_height);
    appendPathTraceHistoryValue(
        key, resolved.hdri_runtime_budget.distribution_width);
    appendPathTraceHistoryValue(
        key, resolved.hdri_runtime_budget.distribution_height);
    if (resolved.hdr != nullptr) {
      appendPathTraceHistoryBytes(key, resolved.hdr->checksum.data(),
                                  resolved.hdr->checksum.size());
      appendPathTraceHistoryValue(key, resolved.hdr->generation);
      appendPathTraceHistoryValue(key, resolved.hdr->radiance.width);
      appendPathTraceHistoryValue(key, resolved.hdr->radiance.height);
    }
    return key;
  }

bool VulkanBackend::ensureWorldEnvironmentSampler() {
    if (world_environment_sampler_ != VK_NULL_HANDLE) {
      return true;
    }
    VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = 0.0f;
    sampler_info.maxLod = VK_LOD_CLAMP_NONE;
    return vkCreateSampler(device_, &sampler_info, nullptr,
                           &world_environment_sampler_) == VK_SUCCESS;
  }

bool VulkanBackend::uploadWorldEnvironment(
      ResolvedWorldEnvironment &resolved, bool defer_commit) {
    static_assert(
        std::is_nothrow_move_assignable_v<HdrEnvironmentAsset> &&
        std::is_nothrow_move_assignable_v<ResolvedWorldEnvironment>);
    constexpr VkDeviceSize kMaximumGpuBytes =
        VkDeviceSize{512} * 1024u * 1024u;
    if (resolved.sky_rendering != SkyRendering::UserHdri ||
        resolved.hdr == nullptr || !resolved.hdr->valid()) {
      return false;
    }
    if (world_environment_pending_.active) {
      writeLog("Vulkan world-environment Candidate is already pending");
      return false;
    }
    const std::uint64_t requested_resource_key =
        worldEnvironmentResourceKey(resolved);
    const HdrEnvironmentAsset &source_asset = *resolved.hdr;
    const std::uint32_t source_asset_width = source_asset.radiance.width;
    const std::uint32_t source_asset_height = source_asset.radiance.height;
    HdrEnvironmentRuntimeCandidate runtime_candidate;
    std::string candidate_error;
    if (!buildHdrEnvironmentRuntimeCandidate(
            source_asset, resolved.requested_hdri_runtime_width,
            resolved.hdri_runtime_generation, runtime_candidate,
            &candidate_error)) {
      xpbd::log::warnf(
          "World HDRI runtime candidate rejected: %s",
          candidate_error.empty() ? "unknown candidate failure"
                                  : candidate_error.c_str());
      return false;
    }
    if (runtime_candidate.budget.resolved_width !=
            resolved.resolved_hdri_runtime_width ||
        runtime_candidate.budget.resolved_height !=
            resolved.resolved_hdri_runtime_height) {
      xpbd::log::warn(
          "World HDRI runtime candidate disagrees with the resolved key");
      return false;
    }
    HdrEnvironmentAsset published_asset;
    ResolvedWorldEnvironment published_resolved;
    try {
      published_asset.source_identity = source_asset.source_identity;
      published_asset.checksum = source_asset.checksum;
      published_resolved = resolved;
    } catch (const std::bad_alloc &) {
      xpbd::log::warn(
          "World HDRI publication Candidate allocation failed");
      return false;
    } catch (...) {
      xpbd::log::warn(
          "World HDRI publication Candidate construction raised an exception");
      return false;
    }
    published_asset.generation = runtime_candidate.content_generation;
    published_resolved.hdri_runtime_budget = runtime_candidate.budget;
    published_resolved.resolved_hdri_runtime_width =
        runtime_candidate.budget.resolved_width;
    published_resolved.resolved_hdri_runtime_height =
        runtime_candidate.budget.resolved_height;
    const FloatEnvironmentImage &radiance = runtime_candidate.radiance;
    const std::uint32_t runtime_width = radiance.width;
    const std::uint32_t runtime_height = radiance.height;
    const EnvironmentDistribution &distribution =
        runtime_candidate.distribution;
    if (!radiance.valid() || !distribution.valid() ||
        radiance.width != distribution.width() ||
        radiance.height != distribution.height()) {
      return false;
    }
    const std::uint64_t entry_count64 =
        static_cast<std::uint64_t>(radiance.width) * radiance.height;
    if (entry_count64 == 0u ||
        entry_count64 >
            (std::numeric_limits<std::uint32_t>::max)()) {
      return false;
    }
    const VkDeviceSize entry_count =
        static_cast<VkDeviceSize>(entry_count64);
    const VkDeviceSize image_bytes =
        entry_count * VkDeviceSize{4u * sizeof(float)};
    const VkDeviceSize table_bytes =
        sizeof(WorldEnvironmentGpuHeader) +
        entry_count * sizeof(WorldEnvironmentGpuAlias);

    VkFormatProperties format_properties{};
    vkGetPhysicalDeviceFormatProperties(
        phys_, VK_FORMAT_R32G32B32A32_SFLOAT, &format_properties);
    if ((format_properties.optimalTilingFeatures &
         VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) == 0u) {
      xpbd::log::warn(
          "World HDRI GPU upload rejected: RGBA32F sampling unsupported");
      return false;
    }
    constexpr VkFormatFeatureFlags kMipBlitFeatures =
        VK_FORMAT_FEATURE_BLIT_SRC_BIT |
        VK_FORMAT_FEATURE_BLIT_DST_BIT |
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    const bool mip_blit_supported =
        (format_properties.optimalTilingFeatures & kMipBlitFeatures) ==
        kMipBlitFeatures;
    std::uint32_t environment_mip_levels = 1u;
    std::uint64_t mip_texel_count = entry_count64;
    if (mip_blit_supported) {
      std::uint32_t mip_width = radiance.width;
      std::uint32_t mip_height = radiance.height;
      while (mip_width > 1u || mip_height > 1u) {
        mip_width = std::max(mip_width >> 1u, 1u);
        mip_height = std::max(mip_height >> 1u, 1u);
        mip_texel_count +=
            static_cast<std::uint64_t>(mip_width) * mip_height;
        ++environment_mip_levels;
      }
    } else {
      xpbd::log::warn(
          "World HDRI RGBA32F linear blit unsupported; ray-cone LOD "
          "conservatively clamps to the base level");
    }
    const VkDeviceSize gpu_image_bytes =
        static_cast<VkDeviceSize>(mip_texel_count) *
        VkDeviceSize{4u * sizeof(float)};
    if (gpu_image_bytes > kMaximumGpuBytes ||
        table_bytes > kMaximumGpuBytes ||
        gpu_image_bytes > kMaximumGpuBytes - table_bytes) {
      xpbd::log::warnf(
          "World HDRI GPU upload rejected: image=%llu table=%llu "
          "combined limit=%llu",
          static_cast<unsigned long long>(gpu_image_bytes),
          static_cast<unsigned long long>(table_bytes),
          static_cast<unsigned long long>(kMaximumGpuBytes));
      return false;
    }
    if (!ensureWorldEnvironmentSampler()) {
      xpbd::log::warn("World HDRI sampler creation failed");
      return false;
    }

    Buffer staging{};
    Buffer new_distribution{};
    ImageResource new_texture{};
    VkCommandBuffer command = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    auto cleanup = [&] {
      if (gpu_completion_unproven_) {
        return;
      }
      if (fence != VK_NULL_HANDLE) {
        vkDestroyFence(device_, fence, nullptr);
        fence = VK_NULL_HANDLE;
      }
      if (command != VK_NULL_HANDLE) {
        vkFreeCommandBuffers(device_, cmd_pool_, 1, &command);
        command = VK_NULL_HANDLE;
      }
      destroyBuffer(staging);
      destroyBuffer(new_distribution);
      destroyImage(new_texture);
    };
    if (!createBuffer(image_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      staging) ||
        !createBuffer(table_bytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                      new_distribution) ||
        !createStaticTexture(radiance.width, radiance.height,
                             VK_FORMAT_R32G32B32A32_SFLOAT,
                             new_texture, environment_mip_levels)) {
      cleanup();
      return false;
    }
    std::memcpy(staging.mapped, radiance.rgba.data(),
                static_cast<std::size_t>(image_bytes));

    constexpr std::uint32_t kValidHdr = 1u << 0u;
    constexpr std::uint32_t kBackgroundVisible = 1u << 1u;
    constexpr std::uint32_t kLightingEnabled = 1u << 2u;
    WorldEnvironmentGpuHeader header;
    header.flags = kValidHdr |
                   (resolved.background_visible ? kBackgroundVisible : 0u) |
                   (resolved.environment_lighting ? kLightingEnabled : 0u);
    header.width = radiance.width;
    header.height = radiance.height;
    header.entry_count = static_cast<std::uint32_t>(entry_count64);
    header.lighting_strength = resolved.environment_strength;
    header.background_multiplier = resolved.background_multiplier;
    header.rotation_radians = resolved.rotation_radians;
    header.light_power = {
        estimateEnvironmentPower(radiance, resolved.environment_strength),
        0.0f, 0.0f, 0.0f};
    std::memcpy(new_distribution.mapped, &header, sizeof(header));
    auto *gpu_alias = reinterpret_cast<WorldEnvironmentGpuAlias *>(
        static_cast<std::byte *>(new_distribution.mapped) +
        sizeof(WorldEnvironmentGpuHeader));
    for (std::uint32_t y = 0; y < radiance.height; ++y) {
      for (std::uint32_t x = 0; x < radiance.width; ++x) {
        const std::uint32_t index = y * radiance.width + x;
        gpu_alias[index].acceptance = static_cast<float>(
            std::clamp(distribution.aliasAcceptance(x, y), 0.0, 1.0));
        gpu_alias[index].alias_index =
            distribution.aliasIndex(x, y);
        gpu_alias[index].probability = static_cast<float>(
            std::max(distribution.texelProbability(x, y), 0.0));
      }
    }

    VkCommandBufferAllocateInfo allocate_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    allocate_info.commandPool = cmd_pool_;
    allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocate_info.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &allocate_info, &command) !=
        VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkCommandBufferBeginInfo begin_info{
        VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(command, &begin_info) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = new_texture.image;
    barrier.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, 0, environment_mip_levels, 0, 1};
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &barrier);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {radiance.width, radiance.height, 1};
    vkCmdCopyBufferToImage(command, staging.buffer, new_texture.image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    std::uint32_t source_width = radiance.width;
    std::uint32_t source_height = radiance.height;
    for (std::uint32_t level = 1u; level < environment_mip_levels;
         ++level) {
      barrier.subresourceRange = {
          VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 1u, 0u, 1u};
      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                           nullptr, 1, &barrier);

      const std::uint32_t destination_width =
          std::max(source_width >> 1u, 1u);
      const std::uint32_t destination_height =
          std::max(source_height >> 1u, 1u);
      VkImageBlit blit{};
      blit.srcSubresource = {
          VK_IMAGE_ASPECT_COLOR_BIT, level - 1u, 0u, 1u};
      blit.srcOffsets[1] = {
          static_cast<std::int32_t>(source_width),
          static_cast<std::int32_t>(source_height), 1};
      blit.dstSubresource = {
          VK_IMAGE_ASPECT_COLOR_BIT, level, 0u, 1u};
      blit.dstOffsets[1] = {
          static_cast<std::int32_t>(destination_width),
          static_cast<std::int32_t>(destination_height), 1};
      vkCmdBlitImage(
          command, new_texture.image,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, new_texture.image,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1u, &blit,
          VK_FILTER_LINEAR);

      barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0,
                           0, nullptr, 0, nullptr, 1, &barrier);
      source_width = destination_width;
      source_height = destination_height;
    }
    barrier.subresourceRange = {
        VK_IMAGE_ASPECT_COLOR_BIT, environment_mip_levels - 1u, 1u, 0u, 1u};
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_RAY_TRACING_SHADER_BIT_KHR, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);
    if (vkEndCommandBuffer(command) != VK_SUCCESS) {
      cleanup();
      return false;
    }
    published_asset.radiance = std::move(runtime_candidate.radiance);
    published_asset.distribution = std::move(runtime_candidate.distribution);
    if (!published_asset.valid()) {
      cleanup();
      return false;
    }
    const HdriRuntimeBudget published_budget = runtime_candidate.budget;
    VkFenceCreateInfo fence_info{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    const VkResult fence_result =
        vkCreateFence(device_, &fence_info, nullptr, &fence);
    if (fence_result != VK_SUCCESS) {
      if (fence_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError(
            "vkCreateFence.world_environment_pending", fence_result);
      }
      cleanup();
      return false;
    }
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1u;
    submit.pCommandBuffers = &command;
    const VkResult submit_result =
        vkQueueSubmit(graphics_queue_, 1u, &submit, fence);
    if (submit_result != VK_SUCCESS) {
      if (submit_result == VK_ERROR_DEVICE_LOST) {
        recordFatalVulkanError(
            "vkQueueSubmit.world_environment_pending", submit_result);
      }
      cleanup();
      return false;
    }

    WorldEnvironmentPending &pending = world_environment_pending_;
    pending.staging = staging;
    pending.distribution = new_distribution;
    pending.texture = new_texture;
    pending.upload_command = command;
    pending.upload_fence = fence;
    pending.runtime_asset = std::move(published_asset);
    pending.published = std::move(published_resolved);
    pending.published.hdr = &pending.runtime_asset;
    pending.budget = published_budget;
    pending.distribution_bytes = table_bytes;
    pending.power_estimate = header.light_power[0];
    pending.resource_key = requested_resource_key;
    pending.uploaded_bytes = static_cast<std::uint64_t>(image_bytes);
    pending.gpu_image_bytes = static_cast<std::uint64_t>(gpu_image_bytes);
    pending.generation = resolved.generation;
    pending.source_width = source_asset_width;
    pending.source_height = source_asset_height;
    pending.runtime_width = runtime_width;
    pending.runtime_height = runtime_height;
    pending.mip_levels = environment_mip_levels;
    pending.active = true;
    pending.submitted_at = Clock::now();
    staging = {};
    new_distribution = {};
    new_texture = {};
    command = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;

    xpbd::log::infof(
        "A5_ENVIRONMENT_PENDING_BEGIN generation=%llu image=%llu "
        "table=%llu",
        static_cast<unsigned long long>(pending.generation),
        static_cast<unsigned long long>(gpu_image_bytes),
        static_cast<unsigned long long>(table_bytes));
    if (defer_commit) {
      return true;
    }
    bool ready = false;
    bool superseded = false;
    if (!pollWorldEnvironmentPending(true, ready, superseded) ||
        !ready || superseded) {
      discardWorldEnvironmentPending(
          "synchronous-environment-pending-failure");
      return false;
    }
    bool retirement_complete = false;
    if (!pollWorldEnvironmentRetirement(true, retirement_complete) ||
        !retirement_complete || !beginWorldEnvironmentRetirement()) {
      discardWorldEnvironmentPending(
          "synchronous-environment-retirement-failure");
      return false;
    }
    std::uint64_t committed_bytes = 0u;
    if (!commitWorldEnvironmentPending(committed_bytes)) {
      discardWorldEnvironmentPending(
          "synchronous-environment-commit-failure");
      return false;
    }
    resolved.hdr = &world_environment_runtime_asset_;
    resolved.hdri_runtime_budget = published_budget;
    resolved.resolved_hdri_runtime_width = published_budget.resolved_width;
    resolved.resolved_hdri_runtime_height = published_budget.resolved_height;
    return true;
  }

bool VulkanBackend::ensureWorldEnvironmentResources(
      ResolvedWorldEnvironment &resolved, bool defer_commit) {
    const auto bind_published_runtime = [&](bool restore_gpu_parameters) {
      if (restore_gpu_parameters) {
        resolved.background_visible =
            world_environment_published_.background_visible;
        resolved.environment_lighting =
            world_environment_published_.environment_lighting;
        resolved.environment_strength =
            world_environment_published_.environment_strength;
        resolved.background_exposure =
            world_environment_published_.background_exposure;
        resolved.background_multiplier =
            world_environment_published_.background_multiplier;
        resolved.rotation_radians =
            world_environment_published_.rotation_radians;
        resolved.requested_hdri_runtime_width =
            world_environment_published_.requested_hdri_runtime_width;
        resolved.hdri_runtime_generation =
            world_environment_published_.hdri_runtime_generation;
      }
      resolved.hdr = &world_environment_runtime_asset_;
      resolved.hdri_runtime_budget =
          world_environment_published_.hdri_runtime_budget;
      resolved.resolved_hdri_runtime_width =
          world_environment_published_.resolved_hdri_runtime_width;
      resolved.resolved_hdri_runtime_height =
          world_environment_published_.resolved_hdri_runtime_height;
    };
    if (resolved.sky_rendering != SkyRendering::UserHdri ||
        resolved.hdr == nullptr) {
      if (world_environment_texture_.image != VK_NULL_HANDLE ||
          world_environment_distribution_.buffer != VK_NULL_HANDLE) {
        if (submitGraphicsTransactionAndWait(
                VK_NULL_HANDLE, "world-environment-clear")) {
          clearWorldEnvironmentResources();
        } else {
          return false;
        }
      }
      world_environment_failed_key_ = 0;
      return false;
    }
    const std::uint64_t resource_key =
        worldEnvironmentResourceKey(resolved);
    if (world_environment_ready_ &&
        world_environment_resource_key_ == resource_key) {
      // The GPU resource key already covers every field stored in its image
      // and header. Preserve unrelated current-frame state (celestial/cloud
      // generations, debug view, transparency) while rebinding the immutable
      // runtime asset owned by the published transaction.
      bind_published_runtime(false);
      return true;
    }
    if (world_environment_failed_key_ == resource_key) {
      if (world_environment_ready_) {
        bind_published_runtime(true);
        resolved.warning =
            "HDR update failed; retaining the previous complete environment";
        return true;
      }
      return false;
    }
    if (!uploadWorldEnvironment(resolved, defer_commit)) {
      world_environment_failed_key_ = resource_key;
      if (world_environment_ready_) {
        xpbd::log::warnf(
            "World HDRI GPU upload failed for generation %llu; retaining "
            "the previous complete environment",
            static_cast<unsigned long long>(resolved.generation));
        bind_published_runtime(true);
        resolved.warning =
            "HDR update failed; retaining the previous complete environment";
        return true;
      }
      xpbd::log::warnf(
          "World HDRI GPU upload failed for generation %llu; no previous "
          "environment is available",
          static_cast<unsigned long long>(resolved.generation));
      return false;
    }
    return true;
  }

} // namespace xpbd::gfx::detail
