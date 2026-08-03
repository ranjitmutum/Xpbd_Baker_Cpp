#include "xpbd/gfx/labpbr_import.hpp"

#include "xpbd/gfx/labpbr_authoring.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <iterator>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>

namespace xpbd::gfx {
namespace {

constexpr std::uintmax_t kMaxImportFileBytes =
    kFileByteSnapshotMaximumBytes;

[[nodiscard]] std::uint64_t effectiveImportPeakBytes(
    const LabPbrSuiteImportLimits &limits) noexcept {
  return (std::min)(limits.maximum_peak_bytes,
                    static_cast<std::uint64_t>(
                        kTextureDecodeMaximumPeakBytes));
}

[[nodiscard]] std::size_t narrowTextureBytes(
    std::uint64_t bytes) noexcept {
  return static_cast<std::size_t>((std::min)(
      bytes, static_cast<std::uint64_t>(
                 (std::numeric_limits<std::size_t>::max)())));
}

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](char c) {
    return static_cast<char>(
        std::tolower(static_cast<unsigned char>(c)));
  });
  return value;
}

bool endsWith(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

template <typename Character>
std::basic_string<Character>
lowerAsciiNative(std::basic_string<Character> value) {
  std::transform(value.begin(), value.end(), value.begin(), [](Character c) {
    if (c >= static_cast<Character>('A') &&
        c <= static_cast<Character>('Z')) {
      return static_cast<Character>(c +
                                    (static_cast<Character>('a') -
                                     static_cast<Character>('A')));
    }
    return c;
  });
  return value;
}

template <typename Character>
bool endsWithNative(const std::basic_string<Character> &value,
                    const std::basic_string<Character> &suffix) {
  return value.size() >= suffix.size() &&
         value.substr(value.size() - suffix.size()) == suffix;
}

std::filesystem::path publicPathFromIo(const std::filesystem::path &path) {
#ifdef _WIN32
  const auto native = path.native();
  constexpr std::wstring_view unc_prefix = LR"(\\?\UNC\)";
  constexpr std::wstring_view local_prefix = LR"(\\?\)";
  if (native.starts_with(unc_prefix)) {
    return std::filesystem::path(LR"(\\)" + native.substr(unc_prefix.size()));
  }
  if (native.starts_with(local_prefix)) {
    return std::filesystem::path(native.substr(local_prefix.size()));
  }
#endif
  return path;
}

std::string trim(std::string_view value) {
  const auto first = value.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = value.find_last_not_of(" \t\r\n");
  return std::string(value.substr(first, last - first + 1u));
}

std::filesystem::path normalizedPath(const std::filesystem::path &path) {
  std::error_code error;
  auto absolute = std::filesystem::absolute(path, error);
  if (error) {
    return path.lexically_normal();
  }
  return absolute.lexically_normal();
}

std::optional<std::filesystem::path>
findSiblingCaseInsensitive(const std::filesystem::path &parent,
                           const std::filesystem::path::string_type &filename) {
  std::error_code error;
  const auto exact = parent / std::filesystem::path(filename);
  if (std::filesystem::is_regular_file(pathForFilesystemIo(exact), error)) {
    return normalizedPath(exact);
  }
  error.clear();
  const auto wanted = lowerAsciiNative(filename);
  std::filesystem::directory_iterator iterator(pathForFilesystemIo(parent),
                                                error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto &entry = *iterator;
    std::error_code type_error;
    if (entry.is_regular_file(type_error) && !type_error &&
        lowerAsciiNative(entry.path().filename().native()) == wanted) {
      return normalizedPath(publicPathFromIo(entry.path()));
    }
    iterator.increment(error);
  }
  return std::nullopt;
}

std::filesystem::path::string_type
sidecarName(const std::filesystem::path &stem,
            std::filesystem::path::string_type suffix) {
  auto name = stem.native();
  name += suffix;
  return name;
}

LabPbrSourceFile absentSource(const std::filesystem::path &path) {
  LabPbrSourceFile source;
  source.path = normalizedPath(path);
  return source;
}

bool snapshotFile(const std::filesystem::path &path, LabPbrSourceFile &out,
                   std::string &error, std::string_view label,
                   std::uintmax_t maximum_bytes = kMaxImportFileBytes) {
  try {
    FileByteSnapshot bytes;
    if (!snapshotFileBytes(path, bytes, &error, label,
                           (std::min)(maximum_bytes,
                                      kMaxImportFileBytes))) {
      return false;
    }
    LabPbrSourceFile snapshot;
    snapshot.path = bytes.path;
    snapshot.present = true;
    snapshot.size = bytes.size;
    snapshot.write_time = bytes.write_time;
    snapshot.sha256 = sha256Hex(std::span<const std::uint8_t>(*bytes.bytes));
    snapshot.original_bytes = std::move(bytes.bytes);
    out = std::move(snapshot);
    error.clear();
    return true;
  } catch (const std::bad_alloc &) {
    error = std::string(label) +
            " budget stage failed while hashing the source snapshot";
    return false;
  } catch (const std::length_error &exception) {
    error = std::string(label) + " budget stage failed: " + exception.what();
    return false;
  }
}

bool inspectSnapshot(const LabPbrSourceFile &source, const char *label,
                     TextureImageHeader &out, std::string &error) {
  if (!source.valid() ||
      source.original_bytes->size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    error = std::string(label) +
            " read stage failed: source snapshot is invalid";
    return false;
  }
  TextureImageHeader header;
  std::string inspect_detail;
  if (!inspectTextureImageFromMemory(
          source.original_bytes->data(),
          static_cast<int>(source.original_bytes->size()), header,
          &inspect_detail)) {
    error = std::string(label) + " Header stage failed: " +
            (inspect_detail.empty() ? "invalid image" : inspect_detail);
    return false;
  }
  out = header;
  return true;
}

bool decodeSnapshot(const LabPbrSourceFile &source, const char *label,
                    TextureImage &out, std::string &error,
                    TextureDecodeLimits limits = {}) {
  if (!source.valid() ||
      source.original_bytes->size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    error = std::string(label) + " read stage failed: source snapshot is invalid";
    return false;
  }
  TextureImage decoded;
  std::string decode_detail;
  if (!loadTextureImageFromMemory(
          source.original_bytes->data(),
          static_cast<int>(source.original_bytes->size()), decoded,
          &decode_detail, limits)) {
    error = std::string(label) + " Decode stage failed: " +
            (decode_detail.empty() ? "invalid image" : decode_detail);
    return false;
  }
  decoded.path = pathUtf8String(source.path);
  out = std::move(decoded);
  return true;
}

bool declaresLabPbr13(const LabPbrSourceFile &properties,
                      std::string &declared_format, std::string &error) {
  if (!properties.valid()) {
    error = "texture.properties snapshot is invalid";
    return false;
  }
  const std::string text(properties.original_bytes->begin(),
                         properties.original_bytes->end());
  std::istringstream input(text);
  std::string line;
  bool first_line = true;
  while (std::getline(input, line)) {
    if (first_line && line.size() >= 3u &&
        static_cast<unsigned char>(line[0]) == 0xefu &&
        static_cast<unsigned char>(line[1]) == 0xbbu &&
        static_cast<unsigned char>(line[2]) == 0xbfu) {
      line.erase(0, 3);
    }
    first_line = false;
    const auto cleaned = trim(line);
    if (cleaned.empty() || cleaned[0] == '#' || cleaned[0] == '!') {
      continue;
    }
    const auto separator = cleaned.find_first_of("=:");
    if (separator == std::string::npos ||
        trim(std::string_view(cleaned).substr(0, separator)) != "format") {
      continue;
    }
    declared_format =
        trim(std::string_view(cleaned).substr(separator + 1u));
    if (declared_format != "lab-pbr/1.3") {
      error = "texture.properties declares unsupported format '" +
              declared_format + "'";
      return false;
    }
    return true;
  }
  error = "texture.properties has no format=lab-pbr/1.3 declaration";
  return false;
}

std::string sourceCacheKey(const LabPbrSuiteSource &source) {
  std::string key;
  const auto append = [&](const LabPbrSourceFile &file) {
    const auto path = pathUtf8String(file.path);
    key += std::to_string(path.size());
    key += ':';
    key += path;
    key += file.present ? ":1:" : ":0:";
    key += file.sha256;
    key += '\n';
  };
  append(source.base);
  append(source.specular);
  append(source.normal);
  append(source.properties);
  key += source.confirmed_labpbr13_without_properties ? "confirmed=1" :
                                                                "confirmed=0";
  return key;
}

bool buildMaterial(SharedTextureImage base, SharedTextureImage specular,
                   SharedTextureImage normal,
                   const LabPbrSuiteSource &source,
                   ResolvedMaterialTable &out, std::string &error,
                   std::uint64_t maximum_peak_bytes) {
  if (base == nullptr || specular == nullptr || !base->valid() ||
      !specular->valid() || (normal != nullptr && !normal->valid())) {
    error = "cannot build material from invalid decoded images";
    return false;
  }
  ResolvedMaterialTable material;
  material.width = base->width;
  material.height = base->height;
  material.assets.base = source.base.path;
  material.assets.specular = source.specular.path;
  material.assets.normal = source.normal.path;
  material.assets.properties = source.properties.path;
  material.assets.specular_exists = true;
  material.assets.normal_exists = source.normal.present;
  material.assets.properties_exists = source.properties.present;
  material.format = LabPbrFormat::LabPbr13;
  material.declared_format = "lab-pbr/1.3";
  material.format_declared = source.properties.present;
  if (!buildAuthoredResolvedMaterial(
          std::move(base), material, std::move(normal), std::move(specular),
          out, &error, maximum_peak_bytes)) {
    return false;
  }
  return true;
}

void appendChangedPath(LabPbrSourceChangeReport &report,
                       const std::filesystem::path &path) {
  if (std::find(report.changed_paths.begin(), report.changed_paths.end(),
                path) == report.changed_paths.end()) {
    report.changed_paths.push_back(path);
  }
}

void compareSourceFile(const LabPbrSourceFile &expected,
                       LabPbrSourceChangeReport &report) {
  std::error_code filesystem_error;
  const auto io_path = pathForFilesystemIo(expected.path);
  const auto display_path = pathUtf8String(expected.path);
  const bool present =
      std::filesystem::is_regular_file(io_path, filesystem_error);
  if (filesystem_error) {
    report.error = "Source file-check stage failed for '" + display_path +
                   "': " + filesystem_error.message();
    report.availability_changed = true;
    appendChangedPath(report, expected.path);
    return;
  }
  if (present != expected.present) {
    report.availability_changed = true;
    appendChangedPath(report, expected.path);
    return;
  }
  if (!present) {
    return;
  }

  const auto current_size =
      std::filesystem::file_size(io_path, filesystem_error);
  if (filesystem_error) {
    report.error = "Source file-check stage failed for '" + display_path +
                   "': size query failed: " +
                   filesystem_error.message();
    report.availability_changed = true;
    appendChangedPath(report, expected.path);
    return;
  }
  const auto current_time =
      std::filesystem::last_write_time(io_path, filesystem_error);
  if (filesystem_error) {
    report.error = "Source file-check stage failed for '" + display_path +
                   "': timestamp query failed: " +
                   filesystem_error.message();
    report.availability_changed = true;
    appendChangedPath(report, expected.path);
    return;
  }
  if (current_size == expected.size &&
      current_time == expected.write_time) {
    return;
  }

  LabPbrSourceFile current;
  std::string error;
  if (!snapshotFile(expected.path, current, error, "Source")) {
    report.error = std::move(error);
    report.availability_changed = true;
    appendChangedPath(report, expected.path);
    return;
  }
  report.metadata_changed = true;
  appendChangedPath(report, expected.path);
  if (current.sha256 != expected.sha256) {
    report.content_changed = true;
    appendChangedPath(report, expected.path);
  }
}

} // namespace

bool LabPbrSourceFile::metadataValid() const noexcept {
  return present && !path.empty() && size <= kMaxImportFileBytes &&
         sha256.size() == 64u;
}

bool LabPbrSourceFile::valid() const noexcept {
  return metadataValid() && original_bytes != nullptr &&
         original_bytes->size() == size;
}

bool LabPbrSuiteSource::metadataValid() const noexcept {
  return base.metadataValid() && specular.metadataValid() &&
         (!normal.present || normal.metadataValid()) &&
         (!properties.present || properties.metadataValid()) &&
         (properties.present || confirmed_labpbr13_without_properties) &&
         !cache_key.empty();
}

bool LabPbrSuiteSource::valid() const noexcept {
  return metadataValid() && base.valid() && specular.valid() &&
         (!normal.present || normal.valid()) &&
         (!properties.present || properties.valid());
}

bool ImportedLabPbrSuite::valid() const noexcept {
  return base_image != nullptr && base_image->valid() && material.valid() &&
         material.format == LabPbrFormat::LabPbr13 &&
         material.specular_map_active && source.valid();
}

namespace {

std::uint64_t cacheEntryCharge(const std::string &key,
                               const ImportedLabPbrSuite &suite) noexcept {
  std::uint64_t total = 0;
  std::array<const TextureImage *, 4> counted_images{};
  std::size_t image_count = 0;
  std::array<const std::vector<std::uint8_t> *, 4> counted_sources{};
  std::size_t source_count = 0;
  const auto add = [&total](std::size_t bytes) {
    const auto value = static_cast<std::uint64_t>(bytes);
    if (value > (std::numeric_limits<std::uint64_t>::max)() - total) {
      total = (std::numeric_limits<std::uint64_t>::max)();
    } else {
      total += value;
    }
  };
  const auto add_image = [&](const SharedTextureImage &image) {
    if (image == nullptr ||
        std::find(counted_images.begin(),
                  counted_images.begin() + image_count,
                  image.get()) != counted_images.begin() + image_count) {
      return;
    }
    if (image_count >= counted_images.size()) {
      total = (std::numeric_limits<std::uint64_t>::max)();
      return;
    }
    counted_images[image_count++] = image.get();
    add(image->rgba.capacity());
  };
  const auto add_source = [&](const LabPbrSourceFile &source) {
    if (source.original_bytes == nullptr ||
        std::find(counted_sources.begin(),
                  counted_sources.begin() + source_count,
                  source.original_bytes.get()) !=
            counted_sources.begin() + source_count) {
      return;
    }
    if (source_count >= counted_sources.size()) {
      total = (std::numeric_limits<std::uint64_t>::max)();
      return;
    }
    counted_sources[source_count++] = source.original_bytes.get();
    add(source.original_bytes->capacity());
  };
  add(key.capacity());
  add(key.capacity());
  add(suite.source.cache_key.capacity());
  add_image(suite.base_image);
  add_image(suite.material.baseImageAsset());
  add_image(suite.material.normalImageAsset());
  add_image(suite.material.specularImageAsset());
  add_source(suite.source.base);
  add_source(suite.source.normal);
  add_source(suite.source.specular);
  add_source(suite.source.properties);
  return total;
}

} // namespace

bool LabPbrSuiteImportCache::find(std::string_view key,
                                  ImportedLabPbrSuite &out) noexcept {
  const auto found = index_.find(key);
  if (found == index_.end() || found->second->suite == nullptr) {
    return false;
  }
  try {
    ImportedLabPbrSuite candidate = *found->second->suite;
    candidate.cache_hit = true;
    out = std::move(candidate);
  } catch (...) {
    return false;
  }
  entries_.splice(entries_.begin(), entries_, found->second);
  return true;
}

bool LabPbrSuiteImportCache::store(
    const ImportedLabPbrSuite &suite) noexcept {
  if (!suite.valid()) {
    return false;
  }
  try {
    ImportedLabPbrSuite cached = suite;
    cached.cache_hit = false;
    std::string key = cached.source.cache_key;
    auto owned =
        std::make_shared<const ImportedLabPbrSuite>(std::move(cached));
    const std::uint64_t charge = cacheEntryCharge(key, *owned);
    if (charge > maximum_bytes_) {
      return false;
    }

    const auto existing = index_.find(key);
    if (existing != index_.end()) {
      auto entry = existing->second;
      const std::uint64_t without_existing =
          charged_bytes_ >= entry->charged_bytes
              ? charged_bytes_ - entry->charged_bytes
              : 0u;
      std::uint64_t next_charge = 0;
      if (!detail::checkedLabPbrAdd(without_existing, charge, next_charge)) {
        return false;
      }
      entry->suite = std::move(owned);
      entry->charged_bytes = charge;
      charged_bytes_ = next_charge;
      entries_.splice(entries_.begin(), entries_, entry);
      evictToBudget();
      return true;
    }

    std::uint64_t next_charge = 0;
    if (!detail::checkedLabPbrAdd(charged_bytes_, charge, next_charge)) {
      return false;
    }
    entries_.push_front(
        Entry{std::move(key), std::move(owned), charge});
    try {
      const auto [index_entry, inserted] =
          index_.emplace(entries_.front().key, entries_.begin());
      (void)index_entry;
      if (!inserted) {
        entries_.pop_front();
        return false;
      }
    } catch (...) {
      entries_.pop_front();
      throw;
    }
    charged_bytes_ = next_charge;
    evictToBudget();
    return true;
  } catch (...) {
    return false;
  }
}

void LabPbrSuiteImportCache::evictToBudget() noexcept {
  while (charged_bytes_ > maximum_bytes_ && !entries_.empty()) {
    auto victim = std::prev(entries_.end());
    charged_bytes_ = charged_bytes_ >= victim->charged_bytes
                         ? charged_bytes_ - victim->charged_bytes
                         : 0u;
    index_.erase(victim->key);
    entries_.erase(victim);
  }
}

void LabPbrSuiteImportCache::clear() noexcept {
  index_.clear();
  entries_.clear();
  charged_bytes_ = 0;
}

void LabPbrSuiteImportCache::setMaximumBytes(
    std::uint64_t maximum_bytes) noexcept {
  maximum_bytes_ = maximum_bytes;
  evictToBudget();
}

std::uint64_t LabPbrSuiteImportCache::residentBytes(
    std::span<const TextureImage *const> excluded_images,
    std::span<const std::vector<std::uint8_t> *const> excluded_sources)
    const noexcept {
  std::uint64_t total = 0;
  std::vector<const TextureImage *> counted_images;
  std::vector<const std::vector<std::uint8_t> *> counted_sources;
  const auto add = [&total](std::size_t bytes) {
    const auto value = static_cast<std::uint64_t>(bytes);
    if (value > (std::numeric_limits<std::uint64_t>::max)() - total) {
      total = (std::numeric_limits<std::uint64_t>::max)();
    } else {
      total += value;
    }
  };
  const auto add_image = [&add, &counted_images](
                             const SharedTextureImage &image) {
    if (image == nullptr ||
        std::find(counted_images.begin(), counted_images.end(), image.get()) !=
            counted_images.end()) {
      return;
    }
    counted_images.push_back(image.get());
    add(image->rgba.capacity());
  };
  const auto add_source = [&add, &counted_sources](
                              const LabPbrSourceFile &source) {
    if (source.original_bytes == nullptr ||
        std::find(counted_sources.begin(), counted_sources.end(),
                  source.original_bytes.get()) != counted_sources.end()) {
      return;
    }
    counted_sources.push_back(source.original_bytes.get());
    add(source.original_bytes->capacity());
  };
  try {
    for (const auto *image : excluded_images) {
      if (image != nullptr &&
          std::find(counted_images.begin(), counted_images.end(), image) ==
              counted_images.end()) {
        counted_images.push_back(image);
      }
    }
    for (const auto *source : excluded_sources) {
      if (source != nullptr &&
          std::find(counted_sources.begin(), counted_sources.end(), source) ==
              counted_sources.end()) {
        counted_sources.push_back(source);
      }
    }
    for (const auto &entry : entries_) {
      if (entry.suite == nullptr) {
        return (std::numeric_limits<std::uint64_t>::max)();
      }
      const auto &suite = *entry.suite;
      add(entry.key.capacity());
      add(entry.key.capacity());
      add(suite.source.cache_key.capacity());
      add_image(suite.base_image);
      add_image(suite.material.baseImageAsset());
      add_image(suite.material.normalImageAsset());
      add_image(suite.material.specularImageAsset());
      add_source(suite.source.base);
      add_source(suite.source.normal);
      add_source(suite.source.specular);
      add_source(suite.source.properties);
    }
  } catch (...) {
    return (std::numeric_limits<std::uint64_t>::max)();
  }
  return total;
}

std::vector<std::filesystem::path>
discoverLabPbrSuiteCandidates(const std::filesystem::path &folder,
                              std::string *error) {
  std::vector<std::filesystem::path> candidates;
  std::error_code filesystem_error;
  const auto io_folder = pathForFilesystemIo(folder);
  if (!std::filesystem::is_directory(io_folder, filesystem_error)) {
    if (error != nullptr) {
      *error = filesystem_error
                   ? "cannot inspect LabPBR folder: " +
                         filesystem_error.message()
                   : "LabPBR folder does not exist";
    }
    return candidates;
  }

  std::filesystem::directory_iterator iterator(io_folder, filesystem_error);
  const std::filesystem::directory_iterator end;
  const auto png_suffix = lowerAsciiNative(
      std::filesystem::path(".png").native());
  const auto specular_suffix = lowerAsciiNative(
      std::filesystem::path("_s").native());
  const auto normal_suffix = lowerAsciiNative(
      std::filesystem::path("_n").native());
  while (!filesystem_error && iterator != end) {
    const auto &entry = *iterator;
    std::error_code type_error;
    if (entry.is_regular_file(type_error) && !type_error) {
      const auto public_entry = publicPathFromIo(entry.path());
      const auto lower_filename =
          lowerAsciiNative(public_entry.filename().native());
      const auto lower_stem = lowerAsciiNative(public_entry.stem().native());
      if (endsWithNative(lower_filename, png_suffix) &&
          !endsWithNative(lower_stem, specular_suffix) &&
          !endsWithNative(lower_stem, normal_suffix)) {
        const auto specular_name = sidecarName(
            public_entry.stem(), std::filesystem::path("_s.png").native());
        if (findSiblingCaseInsensitive(public_entry.parent_path(),
                                       specular_name)) {
          candidates.push_back(normalizedPath(public_entry));
        }
      }
    }
    iterator.increment(filesystem_error);
  }
  if (filesystem_error) {
    candidates.clear();
    if (error != nullptr) {
      *error = "cannot enumerate LabPBR folder: " +
               filesystem_error.message();
    }
    return candidates;
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const auto &lhs, const auto &rhs) {
              return lowerAscii(pathUtf8String(lhs)) <
                     lowerAscii(pathUtf8String(rhs));
            });
  if (error != nullptr) {
    error->clear();
  }
  return candidates;
}

LabPbrSuiteImportResult importLabPbrSuite(
    const std::filesystem::path &base_path,
    bool confirm_labpbr13_without_properties,
    LabPbrSuiteImportCache *cache,
    LabPbrSuiteImportLimits limits) {
  LabPbrSuiteImportResult result;
  const auto normalized_base = normalizedPath(base_path);
  const auto lower_extension =
      lowerAsciiNative(normalized_base.extension().native());
  const auto lower_stem = lowerAsciiNative(normalized_base.stem().native());
  const auto png_extension =
      lowerAsciiNative(std::filesystem::path(".png").native());
  const auto specular_suffix =
      lowerAsciiNative(std::filesystem::path("_s").native());
  const auto normal_suffix =
      lowerAsciiNative(std::filesystem::path("_n").native());
  if (normalized_base.empty() || lower_extension != png_extension ||
      endsWithNative(lower_stem, specular_suffix) ||
      endsWithNative(lower_stem, normal_suffix)) {
    result.error =
        "select the base <stem>.png, not a sidecar or non-PNG file";
    return result;
  }

  const auto parent = normalized_base.parent_path();
  const auto stem = normalized_base.stem();
  const auto specular_name =
      sidecarName(stem, std::filesystem::path("_s.png").native());
  const auto normal_name =
      sidecarName(stem, std::filesystem::path("_n.png").native());
  const auto properties_name = std::filesystem::path("texture.properties").native();
  const auto specular = findSiblingCaseInsensitive(parent, specular_name);
  if (!specular) {
    result.error = "Specular Sidecar file-check stage failed: required file is missing: " +
                   pathUtf8String(parent / specular_name);
    return result;
  }
  const auto normal = findSiblingCaseInsensitive(parent, normal_name);
  const auto properties = findSiblingCaseInsensitive(parent, properties_name);

  LabPbrSuiteSource source;
  std::string snapshot_error;
  const std::uint64_t effective_peak = effectiveImportPeakBytes(limits);
  std::uint64_t encoded_snapshot_bytes = 0;
  const auto snapshot_with_budget =
      [&](const std::filesystem::path &path, LabPbrSourceFile &out,
          std::string_view label) {
        std::uint64_t resident_and_cache = 0;
        std::uint64_t already_resident = 0;
        if (!detail::checkedLabPbrAdd(limits.retained_resident_bytes,
                                      limits.cache_bytes,
                                      resident_and_cache) ||
            !detail::checkedLabPbrAdd(resident_and_cache,
                                      encoded_snapshot_bytes,
                                      already_resident)) {
          snapshot_error = std::string(label) +
                           " budget stage failed before snapshot: byte arithmetic overflow";
          return false;
        }
        if (already_resident > effective_peak) {
          snapshot_error = std::string(label) +
                           " budget stage failed before snapshot: retained state exceeds the peak limit";
          return false;
        }
        const auto remaining = effective_peak - already_resident;
        if (!snapshotFile(
                path, out, snapshot_error, label,
                static_cast<std::uintmax_t>((std::min)(
                    remaining,
                    static_cast<std::uint64_t>(
                        (std::numeric_limits<std::uintmax_t>::max)()))))) {
          return false;
        }
        std::uint64_t next_encoded = 0;
        if (!detail::checkedLabPbrAdd(
                encoded_snapshot_bytes,
                static_cast<std::uint64_t>(out.size), next_encoded)) {
          snapshot_error = std::string(label) +
                           " budget stage failed after snapshot: encoded byte overflow";
          return false;
        }
        encoded_snapshot_bytes = next_encoded;
        return true;
      };
  if (!snapshot_with_budget(normalized_base, source.base, "Base") ||
      !snapshot_with_budget(*specular, source.specular,
                            "Specular Sidecar")) {
    result.error = std::move(snapshot_error);
    return result;
  }
  source.normal =
      normal ? LabPbrSourceFile{} :
               absentSource(parent / normal_name);
  if (normal && !snapshot_with_budget(*normal, source.normal,
                                      "Normal Sidecar")) {
    result.error = std::move(snapshot_error);
    return result;
  }
  source.properties =
      properties ? LabPbrSourceFile{} :
                   absentSource(parent / properties_name);
  if (properties &&
      !snapshot_with_budget(*properties, source.properties,
                            "Properties Sidecar")) {
    result.error = std::move(snapshot_error);
    return result;
  }
  source.confirmed_labpbr13_without_properties =
      !source.properties.present &&
      confirm_labpbr13_without_properties;

  TextureImageHeader base_header;
  TextureImageHeader specular_header;
  TextureImageHeader normal_header;
  std::string inspect_error;
  if (!inspectSnapshot(source.base, "Base", base_header, inspect_error) ||
      !inspectSnapshot(source.specular, "Specular Sidecar",
                       specular_header, inspect_error)) {
    result.error = std::move(inspect_error);
    return result;
  }
  if (base_header.source_channels < 3) {
    result.error =
        "Base Decode stage failed: image must contain RGB or RGBA channels";
    return result;
  }
  if (specular_header.source_channels != 4) {
    result.error =
        "Specular Sidecar Decode stage failed: _s.png must be an RGBA image";
    return result;
  }
  if (base_header.width != specular_header.width ||
      base_header.height != specular_header.height) {
    result.error =
        "LabPBR Domain stage failed: _s.png dimensions do not match the base image";
    return result;
  }
  if (source.normal.present) {
    if (!inspectSnapshot(source.normal, "Normal Sidecar", normal_header,
                         inspect_error)) {
      result.error = std::move(inspect_error);
      return result;
    }
    if (normal_header.source_channels != 4) {
      result.error =
          "Normal Sidecar Decode stage failed: _n.png must be an RGBA image";
      return result;
    }
    if (base_header.width != normal_header.width ||
        base_header.height != normal_header.height) {
      result.error =
          "LabPBR Domain stage failed: _n.png dimensions do not match the base image";
      return result;
    }
  }

  if (!source.properties.present &&
      !confirm_labpbr13_without_properties) {
    result.status =
        LabPbrSuiteImportStatus::NeedsLabPbr13Confirmation;
    result.error =
        "Properties Sidecar stage requires explicit LabPBR 1.3 confirmation "
        "because texture.properties is missing";
    return result;
  }

  source.cache_key = sourceCacheKey(source);
  if (cache != nullptr && cache->find(source.cache_key, result.suite)) {
    result.suite.source = std::move(source);
    result.suite.cache_hit = true;
    if (!limits.defer_cache_store) {
      (void)cache->store(result.suite);
    }
    result.status = LabPbrSuiteImportStatus::Imported;
    result.error.clear();
    return result;
  }

  std::uint64_t pixels = 0;
  std::uint64_t rgba_bytes = 0;
  std::uint64_t coverage_bytes = 0;
  const std::uint64_t decoded_image_count =
      source.normal.present ? 3u : 2u;
  const std::uint64_t candidate_image_count = decoded_image_count;
  if (!detail::checkedLabPbrMultiply(
          static_cast<std::uint64_t>(base_header.width),
          static_cast<std::uint64_t>(base_header.height), pixels) ||
      !detail::checkedLabPbrMultiply(pixels, 4u, rgba_bytes)) {
    result.error =
        "LabPBR budget preflight failed: atlas byte arithmetic overflow";
    return result;
  }
  if (limits.has_overrides &&
      !estimateLabPbrUvRunCoveragePeakBytes(
          static_cast<std::uint64_t>(base_header.width),
          static_cast<std::uint64_t>(base_header.height), coverage_bytes,
          &result.error)) {
    result.error = "LabPBR budget preflight failed: " + result.error;
    return result;
  }
  LabPbrMemoryEstimateRequest memory_request;
  memory_request.width = static_cast<std::uint64_t>(base_header.width);
  memory_request.height = static_cast<std::uint64_t>(base_header.height);
  memory_request.candidate_rgba_image_count = candidate_image_count;
  memory_request.resolved_texel_bytes_per_pixel =
      kLabPbrResolvedTexelBytesPerPixel;
  memory_request.resident_fixed_bytes = limits.retained_resident_bytes;
  memory_request.candidate_fixed_bytes =
      limits.has_overrides ? rgba_bytes : 0u;
  memory_request.encoded_snapshot_bytes = encoded_snapshot_bytes;
  memory_request.decoder_peak_bytes = rgba_bytes;
  memory_request.coverage_peak_bytes = coverage_bytes;
  memory_request.cache_bytes = limits.cache_bytes;
  LabPbrMemoryEstimate memory_estimate;
  if (!preflightLabPbrMemory(memory_request, effective_peak,
                             memory_estimate, &result.error)) {
    return result;
  }

  if (source.properties.present) {
    std::string declared_format;
    if (!declaresLabPbr13(source.properties, declared_format,
                          result.error)) {
      result.error = "Properties Sidecar stage failed: " + result.error;
      return result;
    }
  }

  TextureImage base;
  TextureImage specular_image;
  TextureImage normal_image;
  std::string decode_error;
  std::uint64_t decoded_resident_bytes = 0;
  const auto decode_with_budget =
      [&](const LabPbrSourceFile &snapshot, const char *label,
          TextureImage &out) {
        std::uint64_t retained = 0;
        const std::uint64_t other_snapshots =
            encoded_snapshot_bytes - static_cast<std::uint64_t>(snapshot.size);
        if (!detail::checkedLabPbrAdd(limits.retained_resident_bytes,
                                      limits.cache_bytes, retained) ||
            !detail::checkedLabPbrAdd(retained, other_snapshots, retained) ||
            !detail::checkedLabPbrAdd(retained, decoded_resident_bytes,
                                      retained)) {
          decode_error = std::string(label) +
                         " budget stage failed before Decode: byte arithmetic overflow";
          return false;
        }
        TextureDecodeLimits decode_limits;
        decode_limits.maximum_peak_bytes = narrowTextureBytes(effective_peak);
        decode_limits.retained_resident_bytes = narrowTextureBytes(retained);
        if (!decodeSnapshot(snapshot, label, out, decode_error,
                            decode_limits)) {
          return false;
        }
        std::uint64_t next_decoded = 0;
        if (!detail::checkedLabPbrAdd(
                decoded_resident_bytes,
                static_cast<std::uint64_t>(out.rgba.capacity()),
                next_decoded)) {
          decode_error = std::string(label) +
                         " budget stage failed after Decode: resident byte overflow";
          return false;
        }
        decoded_resident_bytes = next_decoded;
        return true;
      };
  if (!decode_with_budget(source.base, "Base", base) ||
      !decode_with_budget(source.specular, "Specular Sidecar",
                          specular_image)) {
    result.error = std::move(decode_error);
    return result;
  }
  if (base.source_channels < 3) {
    result.error =
        "Base Decode stage failed: image must contain RGB or RGBA channels";
    return result;
  }
  if (specular_image.source_channels != 4) {
    result.error =
        "Specular Sidecar Decode stage failed: _s.png must be an RGBA image";
    return result;
  }
  if (base.width != specular_image.width ||
      base.height != specular_image.height) {
    result.error =
        "LabPBR Domain stage failed: _s.png dimensions do not match the base image";
    return result;
  }
  if (source.normal.present) {
    if (!decode_with_budget(source.normal, "Normal Sidecar",
                            normal_image)) {
      result.error = std::move(decode_error);
      return result;
    }
    if (normal_image.source_channels != 4) {
      result.error =
          "Normal Sidecar Decode stage failed: _n.png must be an RGBA image";
      return result;
    }
    if (base.width != normal_image.width ||
        base.height != normal_image.height) {
      result.error =
          "LabPBR Domain stage failed: _n.png dimensions do not match the base image";
      return result;
    }
  }

  ImportedLabPbrSuite imported;
  try {
    auto base_asset =
        std::make_shared<const TextureImage>(std::move(base));
    auto specular_asset =
        std::make_shared<const TextureImage>(std::move(specular_image));
    auto normal_asset = source.normal.present
                            ? std::make_shared<const TextureImage>(
                                  std::move(normal_image))
                            : SharedTextureImage{};
    ResolvedMaterialTable material;
    if (!buildMaterial(base_asset, specular_asset, normal_asset, source,
                       material, result.error, effective_peak)) {
      return result;
    }
    imported.base_image = std::move(base_asset);
    imported.material = std::move(material);
    imported.source = std::move(source);
  } catch (const std::bad_alloc &) {
    result.error =
        "LabPBR budget stage failed while publishing shared image assets";
    return result;
  } catch (const std::length_error &exception) {
    result.error = std::string("LabPBR budget stage failed: ") +
                   exception.what();
    return result;
  }
  if (!imported.valid()) {
    result.error = "strict LabPBR import produced invalid state";
    return result;
  }
  if (cache != nullptr && !limits.defer_cache_store) {
    (void)cache->store(imported);
  }
  result.status = LabPbrSuiteImportStatus::Imported;
  result.suite = std::move(imported);
  result.error.clear();
  return result;
}

LabPbrSourceChangeReport
checkLabPbrSuiteSourceChanges(const LabPbrSuiteSource &source) {
  LabPbrSourceChangeReport report;
  if (!source.metadataValid()) {
    report.availability_changed = true;
    report.error = "active LabPBR source metadata is invalid";
    return report;
  }
  compareSourceFile(source.base, report);
  compareSourceFile(source.specular, report);
  compareSourceFile(source.normal, report);
  compareSourceFile(source.properties, report);
  return report;
}

} // namespace xpbd::gfx
