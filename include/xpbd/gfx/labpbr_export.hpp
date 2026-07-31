#pragma once

#include "xpbd/gfx/labpbr_authoring.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace xpbd::gfx {

struct LabPbrExportResult {
  bool success = false;
  bool overwrite_required = false;
  std::filesystem::path specular_path;
  std::filesystem::path normal_path;
  std::filesystem::path properties_path;
  std::vector<std::filesystem::path> existing_paths;
  std::string error;
};

[[nodiscard]] std::filesystem::path
normalizeLabPbrSpecularPath(const std::filesystem::path &destination);

[[nodiscard]] LabPbrExportResult exportLabPbrBundle(
    const std::filesystem::path &destination,
    const LabPbrCompositionResult &composition,
    const ReadOnlyIrisNormalAsset *normal, bool allow_overwrite);

} // namespace xpbd::gfx
