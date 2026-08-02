#pragma once

#include "xpbd/gfx/labpbr_material.hpp"
#include "xpbd/gfx/labpbr_memory.hpp"

#include <cstdint>
#include <filesystem>
#include <list>
#include <map>
#include <memory>
#include <span>
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

  [[nodiscard]] bool metadataValid() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
};

struct LabPbrSuiteSource {
  LabPbrSourceFile base;
  LabPbrSourceFile specular;
  LabPbrSourceFile normal;
  LabPbrSourceFile properties;
  bool confirmed_labpbr13_without_properties = false;
  std::string cache_key;

  [[nodiscard]] bool metadataValid() const noexcept;
  [[nodiscard]] bool valid() const noexcept;
};

struct ImportedLabPbrSuite {
  SharedTextureImage base_image;
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
  bool has_overrides = false;
  bool defer_cache_store = false;
};

inline constexpr std::uint64_t kLabPbrDefaultImportCacheBudgetBytes =
    std::uint64_t{256} * 1024u * 1024u;

class LabPbrSuiteImportCache {
public:
  explicit LabPbrSuiteImportCache(
      std::uint64_t maximum_bytes =
          kLabPbrDefaultImportCacheBudgetBytes) noexcept
      : maximum_bytes_(maximum_bytes) {}
  LabPbrSuiteImportCache(const LabPbrSuiteImportCache &) = delete;
  LabPbrSuiteImportCache &operator=(const LabPbrSuiteImportCache &) = delete;

  [[nodiscard]] bool find(std::string_view key,
                          ImportedLabPbrSuite &out) noexcept;
  [[nodiscard]] bool store(const ImportedLabPbrSuite &suite) noexcept;
  void clear() noexcept;
  void setMaximumBytes(std::uint64_t maximum_bytes) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return index_.size(); }
  [[nodiscard]] std::uint64_t maximumBytes() const noexcept {
    return maximum_bytes_;
  }
  [[nodiscard]] std::uint64_t residentBytes(
      std::span<const TextureImage *const> excluded_images = {},
      std::span<const std::vector<std::uint8_t> *const> excluded_sources = {})
      const noexcept;

private:
  struct Entry {
    std::string key;
    std::shared_ptr<const ImportedLabPbrSuite> suite;
    std::uint64_t charged_bytes = 0;
  };
  using EntryList = std::list<Entry>;

  void evictToBudget() noexcept;

  EntryList entries_;
  std::map<std::string, EntryList::iterator, std::less<>> index_;
  std::uint64_t charged_bytes_ = 0;
  std::uint64_t maximum_bytes_ = kLabPbrDefaultImportCacheBudgetBytes;
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
