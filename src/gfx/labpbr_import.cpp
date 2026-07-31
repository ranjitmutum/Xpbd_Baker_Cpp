#include "xpbd/gfx/labpbr_import.hpp"

#include "xpbd/gfx/labpbr_authoring.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>

namespace xpbd::gfx {
namespace {

constexpr std::uintmax_t kMaxImportFileBytes =
    static_cast<std::uintmax_t>((std::numeric_limits<int>::max)());

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
                           std::string_view filename) {
  std::error_code error;
  const auto exact = parent / std::filesystem::path(filename);
  if (std::filesystem::is_regular_file(exact, error)) {
    return normalizedPath(exact);
  }
  error.clear();
  const std::string wanted = lowerAscii(std::string(filename));
  std::filesystem::directory_iterator iterator(parent, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto &entry = *iterator;
    std::error_code type_error;
    if (entry.is_regular_file(type_error) && !type_error &&
        lowerAscii(entry.path().filename().string()) == wanted) {
      return normalizedPath(entry.path());
    }
    iterator.increment(error);
  }
  return std::nullopt;
}

LabPbrSourceFile absentSource(const std::filesystem::path &path) {
  LabPbrSourceFile source;
  source.path = normalizedPath(path);
  return source;
}

bool snapshotFile(const std::filesystem::path &path, LabPbrSourceFile &out,
                  std::string &error) {
  const auto normalized = normalizedPath(path);
  std::error_code filesystem_error;
  if (!std::filesystem::is_regular_file(normalized, filesystem_error)) {
    error = filesystem_error
                ? "cannot inspect source file '" + normalized.string() +
                      "': " + filesystem_error.message()
                : "required source file is missing: " + normalized.string();
    return false;
  }
  const auto initial_size =
      std::filesystem::file_size(normalized, filesystem_error);
  if (filesystem_error) {
    error = "cannot read source file size '" + normalized.string() +
            "': " + filesystem_error.message();
    return false;
  }
  if (initial_size > kMaxImportFileBytes) {
    error = "source file is too large to import: " + normalized.string();
    return false;
  }
  const auto initial_time =
      std::filesystem::last_write_time(normalized, filesystem_error);
  if (filesystem_error) {
    error = "cannot read source timestamp '" + normalized.string() +
            "': " + filesystem_error.message();
    return false;
  }

  std::ifstream input(normalized, std::ios::binary);
  if (!input) {
    error = "cannot open source file: " + normalized.string();
    return false;
  }
  auto bytes = std::make_shared<std::vector<std::uint8_t>>(
      static_cast<std::size_t>(initial_size));
  if (!bytes->empty()) {
    input.read(reinterpret_cast<char *>(bytes->data()),
               static_cast<std::streamsize>(bytes->size()));
    if (!input ||
        input.gcount() != static_cast<std::streamsize>(bytes->size())) {
      error = "source file could not be read completely: " +
              normalized.string();
      return false;
    }
  }

  const auto final_size =
      std::filesystem::file_size(normalized, filesystem_error);
  if (filesystem_error) {
    error = "cannot recheck source file size: " + normalized.string();
    return false;
  }
  const auto final_time =
      std::filesystem::last_write_time(normalized, filesystem_error);
  if (filesystem_error || final_size != initial_size ||
      final_time != initial_time) {
    error = "source file changed while it was being read: " +
            normalized.string();
    return false;
  }

  LabPbrSourceFile snapshot;
  snapshot.path = normalized;
  snapshot.present = true;
  snapshot.size = initial_size;
  snapshot.write_time = initial_time;
  snapshot.sha256 = sha256Hex(std::span<const std::uint8_t>(*bytes));
  snapshot.original_bytes = std::move(bytes);
  out = std::move(snapshot);
  return true;
}

bool decodeSnapshot(const LabPbrSourceFile &source, const char *label,
                    TextureImage &out, std::string &error) {
  if (!source.valid() ||
      source.original_bytes->size() >
          static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
    error = std::string(label) + " source snapshot is invalid";
    return false;
  }
  TextureImage decoded;
  if (!loadTextureImageFromMemory(
          source.original_bytes->data(),
          static_cast<int>(source.original_bytes->size()), decoded, &error)) {
    error = std::string(label) + " decode failed: " +
            (error.empty() ? "invalid image" : error);
    return false;
  }
  decoded.path = source.path.string();
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
    const auto path = file.path.generic_string();
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

bool buildMaterial(const TextureImage &base, const TextureImage &specular,
                   const TextureImage *normal,
                   const LabPbrSuiteSource &source,
                   ResolvedMaterialTable &out, std::string &error) {
  if (!base.valid() || !specular.valid() ||
      (normal != nullptr && !normal->valid())) {
    error = "cannot build material from invalid decoded images";
    return false;
  }
  const auto width = static_cast<std::size_t>(base.width);
  const auto height = static_cast<std::size_t>(base.height);
  if (width > (std::numeric_limits<std::size_t>::max)() / height) {
    error = "resolved material dimensions overflow";
    return false;
  }

  ResolvedMaterialTable material;
  material.width = base.width;
  material.height = base.height;
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
  material.specular_map_active = true;
  material.specular_image = specular;
  material.normal_map_active = normal != nullptr;
  if (normal != nullptr) {
    material.normal_image = *normal;
  }

  const std::size_t texel_count = width * height;
  material.texels.resize(texel_count);
  for (std::size_t texel = 0; texel < texel_count; ++texel) {
    const std::size_t offset = texel * 4u;
    const std::array<std::uint8_t, 4> base_rgba{
        base.rgba[offset], base.rgba[offset + 1u], base.rgba[offset + 2u],
        base.rgba[offset + 3u]};
    const std::array<std::uint8_t, 4> specular_rgba{
        specular.rgba[offset], specular.rgba[offset + 1u],
        specular.rgba[offset + 2u], specular.rgba[offset + 3u]};
    std::array<std::uint8_t, 4> normal_rgba{};
    const std::array<std::uint8_t, 4> *normal_ptr = nullptr;
    if (normal != nullptr) {
      normal_rgba = {normal->rgba[offset], normal->rgba[offset + 1u],
                     normal->rgba[offset + 2u],
                     normal->rgba[offset + 3u]};
      normal_ptr = &normal_rgba;
    }
    material.texels[texel] =
        decodeLabPbrTexel(base_rgba, normal_ptr, &specular_rgba, 4);
  }
  out = std::move(material);
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
  const bool present =
      std::filesystem::is_regular_file(expected.path, filesystem_error);
  if (filesystem_error) {
    report.error = "cannot inspect source file '" + expected.path.string() +
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
      std::filesystem::file_size(expected.path, filesystem_error);
  if (filesystem_error) {
    report.error = "cannot inspect source file size '" +
                   expected.path.string() + "': " +
                   filesystem_error.message();
    report.availability_changed = true;
    appendChangedPath(report, expected.path);
    return;
  }
  const auto current_time =
      std::filesystem::last_write_time(expected.path, filesystem_error);
  if (filesystem_error) {
    report.error = "cannot inspect source timestamp '" +
                   expected.path.string() + "': " +
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
  if (!snapshotFile(expected.path, current, error)) {
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

bool LabPbrSourceFile::valid() const noexcept {
  return present && !path.empty() && size <= kMaxImportFileBytes &&
         sha256.size() == 64u && original_bytes != nullptr &&
         original_bytes->size() == size;
}

bool LabPbrSuiteSource::valid() const noexcept {
  return base.valid() && specular.valid() &&
         (!normal.present || normal.valid()) &&
         (!properties.present || properties.valid()) &&
         (properties.present || confirmed_labpbr13_without_properties) &&
         !cache_key.empty();
}

bool ImportedLabPbrSuite::valid() const noexcept {
  return base_image.valid() && material.valid() &&
         material.format == LabPbrFormat::LabPbr13 &&
         material.specular_map_active && source.valid();
}

bool LabPbrSuiteImportCache::find(std::string_view key,
                                  ImportedLabPbrSuite &out) const {
  const auto found = entries_.find(key);
  if (found == entries_.end()) {
    return false;
  }
  out = found->second;
  out.cache_hit = true;
  return true;
}

void LabPbrSuiteImportCache::store(const ImportedLabPbrSuite &suite) {
  if (!suite.valid()) {
    return;
  }
  auto cached = suite;
  cached.cache_hit = false;
  entries_.insert_or_assign(cached.source.cache_key, std::move(cached));
}

std::vector<std::filesystem::path>
discoverLabPbrSuiteCandidates(const std::filesystem::path &folder,
                              std::string *error) {
  std::vector<std::filesystem::path> candidates;
  std::error_code filesystem_error;
  if (!std::filesystem::is_directory(folder, filesystem_error)) {
    if (error != nullptr) {
      *error = filesystem_error
                   ? "cannot inspect LabPBR folder: " +
                         filesystem_error.message()
                   : "LabPBR folder does not exist";
    }
    return candidates;
  }

  std::filesystem::directory_iterator iterator(folder, filesystem_error);
  const std::filesystem::directory_iterator end;
  while (!filesystem_error && iterator != end) {
    const auto &entry = *iterator;
    std::error_code type_error;
    if (entry.is_regular_file(type_error) && !type_error) {
      const auto filename = entry.path().filename().string();
      const auto lower_filename = lowerAscii(filename);
      const auto lower_stem = lowerAscii(entry.path().stem().string());
      if (endsWith(lower_filename, ".png") &&
          !endsWith(lower_stem, "_s") && !endsWith(lower_stem, "_n")) {
        const std::string specular_name =
            entry.path().stem().string() + "_s.png";
        if (findSiblingCaseInsensitive(entry.path().parent_path(),
                                       specular_name)) {
          candidates.push_back(normalizedPath(entry.path()));
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
              return lowerAscii(lhs.generic_string()) <
                     lowerAscii(rhs.generic_string());
            });
  if (error != nullptr) {
    error->clear();
  }
  return candidates;
}

LabPbrSuiteImportResult importLabPbrSuite(
    const std::filesystem::path &base_path,
    bool confirm_labpbr13_without_properties,
    LabPbrSuiteImportCache *cache) {
  LabPbrSuiteImportResult result;
  const auto normalized_base = normalizedPath(base_path);
  const auto lower_extension =
      lowerAscii(normalized_base.extension().string());
  const auto lower_stem = lowerAscii(normalized_base.stem().string());
  if (normalized_base.empty() || lower_extension != ".png" ||
      endsWith(lower_stem, "_s") || endsWith(lower_stem, "_n")) {
    result.error =
        "select the base <stem>.png, not a sidecar or non-PNG file";
    return result;
  }

  const auto parent = normalized_base.parent_path();
  const auto stem = normalized_base.stem().string();
  const auto specular =
      findSiblingCaseInsensitive(parent, stem + "_s.png");
  if (!specular) {
    result.error = "required LabPBR specular file is missing: " +
                   (parent / (stem + "_s.png")).string();
    return result;
  }
  const auto normal =
      findSiblingCaseInsensitive(parent, stem + "_n.png");
  const auto properties =
      findSiblingCaseInsensitive(parent, "texture.properties");

  LabPbrSuiteSource source;
  std::string snapshot_error;
  if (!snapshotFile(normalized_base, source.base, snapshot_error) ||
      !snapshotFile(*specular, source.specular, snapshot_error)) {
    result.error = std::move(snapshot_error);
    return result;
  }
  source.normal =
      normal ? LabPbrSourceFile{} :
               absentSource(parent / (stem + "_n.png"));
  if (normal && !snapshotFile(*normal, source.normal, snapshot_error)) {
    result.error = std::move(snapshot_error);
    return result;
  }
  source.properties =
      properties ? LabPbrSourceFile{} :
                   absentSource(parent / "texture.properties");
  if (properties &&
      !snapshotFile(*properties, source.properties, snapshot_error)) {
    result.error = std::move(snapshot_error);
    return result;
  }
  source.confirmed_labpbr13_without_properties =
      !source.properties.present &&
      confirm_labpbr13_without_properties;

  TextureImage base;
  TextureImage specular_image;
  TextureImage normal_image;
  std::string decode_error;
  if (!decodeSnapshot(source.base, "base", base, decode_error) ||
      !decodeSnapshot(source.specular, "specular", specular_image,
                      decode_error)) {
    result.error = std::move(decode_error);
    return result;
  }
  if (base.source_channels < 3) {
    result.error = "base image must contain RGB or RGBA channels";
    return result;
  }
  if (specular_image.source_channels != 4) {
    result.error = "LabPBR _s.png must be an RGBA image";
    return result;
  }
  if (base.width != specular_image.width ||
      base.height != specular_image.height) {
    result.error = "LabPBR _s.png dimensions do not match the base image";
    return result;
  }
  if (source.normal.present) {
    if (!decodeSnapshot(source.normal, "normal", normal_image,
                        decode_error)) {
      result.error = std::move(decode_error);
      return result;
    }
    if (normal_image.source_channels != 4) {
      result.error = "LabPBR _n.png must be an RGBA image";
      return result;
    }
    if (base.width != normal_image.width ||
        base.height != normal_image.height) {
      result.error = "LabPBR _n.png dimensions do not match the base image";
      return result;
    }
  }

  if (source.properties.present) {
    std::string declared_format;
    if (!declaresLabPbr13(source.properties, declared_format,
                          result.error)) {
      return result;
    }
  } else if (!confirm_labpbr13_without_properties) {
    result.status =
        LabPbrSuiteImportStatus::NeedsLabPbr13Confirmation;
    result.error =
        "texture.properties is missing; explicit LabPBR 1.3 confirmation "
        "is required";
    return result;
  }

  source.cache_key = sourceCacheKey(source);
  if (cache != nullptr && cache->find(source.cache_key, result.suite)) {
    result.suite.source = source;
    result.suite.cache_hit = true;
    cache->store(result.suite);
    result.status = LabPbrSuiteImportStatus::Imported;
    result.error.clear();
    return result;
  }

  ResolvedMaterialTable material;
  if (!buildMaterial(base, specular_image,
                     source.normal.present ? &normal_image : nullptr,
                     source, material, result.error)) {
    return result;
  }

  ImportedLabPbrSuite imported;
  imported.base_image = std::move(base);
  imported.material = std::move(material);
  imported.source = std::move(source);
  if (!imported.valid()) {
    result.error = "strict LabPBR import produced invalid state";
    return result;
  }
  if (cache != nullptr) {
    cache->store(imported);
  }
  result.status = LabPbrSuiteImportStatus::Imported;
  result.suite = std::move(imported);
  result.error.clear();
  return result;
}

LabPbrSourceChangeReport
checkLabPbrSuiteSourceChanges(const LabPbrSuiteSource &source) {
  LabPbrSourceChangeReport report;
  if (!source.valid()) {
    report.availability_changed = true;
    report.error = "active LabPBR source snapshot is invalid";
    return report;
  }
  compareSourceFile(source.base, report);
  compareSourceFile(source.specular, report);
  compareSourceFile(source.normal, report);
  compareSourceFile(source.properties, report);
  return report;
}

} // namespace xpbd::gfx
