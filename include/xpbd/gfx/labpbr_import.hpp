#pragma once

#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/labpbr_memory.hpp"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace xpbd::gfx {

struct LabPbrSourceFile {
  std::filesystem::path path;
  bool present = false;
  std::uintmax_t size = 0;
  std::filesystem::file_time_type write_time{};
  std::string sha256;
  std::shared_ptr<const std::vector<std::uint8_t>> original_bytes;

  [[nodiscard]] bool valid() const noexcept;
};

struct LabPbrSuiteSource {
  LabPbrSourceFile base;
  LabPbrSourceFile specular;
  LabPbrSourceFile normal;
  LabPbrSourceFile properties;
  bool confirmed_labpbr13_without_properties = false;
  std::string cache_key;

  [[nodiscard]] bool valid() const noexcept;
};

struct ImportedLabPbrSuite {
  TextureImage base_image;
  ResolvedMaterialTable material;
  LabPbrSuiteSource source;
  bool cache_hit = false;

  [[nodiscard]] bool valid() const noexcept;
};

enum class LabPbrSuiteImportStatus {
  Imported,
  NeedsLabPbr13Confirmation,
  Failed,
};

struct LabPbrSuiteImportResult {
  LabPbrSuiteImportStatus status = LabPbrSuiteImportStatus::Failed;
  ImportedLabPbrSuite suite;
  std::string error;

  [[nodiscard]] bool imported() const noexcept {
    return status == LabPbrSuiteImportStatus::Imported && suite.valid();
  }
};

struct LabPbrSuiteImportLimits {
  std::uint64_t maximum_peak_bytes = kLabPbrDefaultPeakBudgetBytes;
  std::uint64_t retained_resident_bytes = 0;
  std::uint64_t cache_bytes = 0;
  bool copy_normal_to_iris_asset = false;
};

class LabPbrSuiteImportCache {
public:
  [[nodiscard]] bool find(std::string_view key,
                          ImportedLabPbrSuite &out) const;
  void store(const ImportedLabPbrSuite &suite);
  void clear() { entries_.clear(); }
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }
  [[nodiscard]] std::uint64_t residentBytes() const noexcept;

private:
  std::map<std::string, ImportedLabPbrSuite, std::less<>> entries_;
};

struct LabPbrSourceChangeReport {
  bool content_changed = false;
  bool availability_changed = false;
  bool metadata_changed = false;
  std::vector<std::filesystem::path> changed_paths;
  std::string error;

  [[nodiscard]] bool reloadRecommended() const noexcept {
    return content_changed || availability_changed || metadata_changed;
  }
};

[[nodiscard]] std::vector<std::filesystem::path>
discoverLabPbrSuiteCandidates(const std::filesystem::path &folder,
                              std::string *error = nullptr);

[[nodiscard]] LabPbrSuiteImportResult importLabPbrSuite(
    const std::filesystem::path &base_path,
    bool confirm_labpbr13_without_properties,
    LabPbrSuiteImportCache *cache = nullptr,
    LabPbrSuiteImportLimits limits = {});

[[nodiscard]] LabPbrSourceChangeReport
checkLabPbrSuiteSourceChanges(const LabPbrSuiteSource &source);

} // namespace xpbd::gfx
