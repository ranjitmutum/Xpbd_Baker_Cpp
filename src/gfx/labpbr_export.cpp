#include "xpbd/gfx/labpbr_export.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <utility>

namespace xpbd::gfx {
namespace {

constexpr std::string_view kLabPbrProperties = "format=lab-pbr/1.3\n";

std::string lowerAscii(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(),
                 [](unsigned char c) {
                   return static_cast<char>(std::tolower(c));
                 });
  return value;
}

void writeBytes(const std::filesystem::path &path,
                std::span<const std::uint8_t> bytes) {
  if (bytes.size() >
      static_cast<std::size_t>(
          (std::numeric_limits<std::streamsize>::max)())) {
    throw std::runtime_error("output is too large to write");
  }
  std::ofstream output(path, std::ios::binary | std::ios::trunc);
  if (!output) {
    throw std::runtime_error("failed to create " + path.string());
  }
  output.write(reinterpret_cast<const char *>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
  if (!output) {
    throw std::runtime_error("failed to write " + path.string());
  }
}

void writeText(const std::filesystem::path &path, std::string_view text) {
  writeBytes(path, std::span<const std::uint8_t>(
                       reinterpret_cast<const std::uint8_t *>(text.data()),
                       text.size()));
}

std::string readText(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("failed to reopen " + path.string());
  }
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

std::filesystem::path createTransactionDirectory(
    const std::filesystem::path &parent, std::string_view suffix) {
  static std::atomic<std::uint64_t> counter{0};
  const auto timestamp =
      std::chrono::steady_clock::now().time_since_epoch().count();
  for (std::uint64_t attempt = 0; attempt < 32u; ++attempt) {
    const auto id = counter.fetch_add(1u, std::memory_order_relaxed);
    const auto candidate =
        parent / (".xpbd_labpbr_" + std::to_string(timestamp) + "_" +
                  std::to_string(id) + "_" + std::string(suffix));
    std::error_code error;
    if (std::filesystem::create_directory(candidate, error)) {
      return candidate;
    }
    if (error && error != std::errc::file_exists) {
      throw std::runtime_error("failed to create export staging directory: " +
                               error.message());
    }
  }
  throw std::runtime_error("failed to allocate a unique export staging path");
}

void removeKnownFilesAndDirectory(
    const std::filesystem::path &directory,
    const std::vector<std::filesystem::path> &files) noexcept {
  std::error_code ignored;
  for (const auto &file : files) {
    std::filesystem::remove(file, ignored);
    ignored.clear();
  }
  std::filesystem::remove(directory, ignored);
}

struct PendingFile {
  std::filesystem::path target;
  std::filesystem::path staged;
  std::filesystem::path backup;
  bool backed_up = false;
  bool installed = false;
};

} // namespace

std::filesystem::path
normalizeLabPbrSpecularPath(const std::filesystem::path &destination) {
  if (destination.empty()) {
    return {};
  }
  auto result = destination;
  if (lowerAscii(result.extension().string()) != ".png") {
    result.replace_extension(".png");
  }
  std::string stem = result.stem().string();
  if (stem.size() < 2u ||
      lowerAscii(stem.substr(stem.size() - 2u)) != "_s") {
    stem += "_s";
    result.replace_filename(stem + ".png");
  }
  return result;
}

LabPbrExportResult exportLabPbrBundle(
    const std::filesystem::path &destination,
    const LabPbrCompositionResult &composition,
    const ReadOnlyIrisNormalAsset *normal, bool allow_overwrite,
    const TextureImage *deferred_source_specular) {
  LabPbrExportResult result;
  try {
    if (!composition.exportable()) {
      throw std::runtime_error(
          "LabPBR composition has validation errors or UV conflicts");
    }
    const bool materialization_deferred =
        (composition.specular == nullptr ||
         !composition.specular->valid()) &&
        composition.specular_materialization_deferred;
    const int specular_width = materialization_deferred
                                   ? composition.deferred_width
                                   : composition.specular->width;
    const int specular_height = materialization_deferred
                                    ? composition.deferred_height
                                    : composition.specular->height;
    const auto normalized = normalizeLabPbrSpecularPath(destination);
    if (normalized.empty()) {
      throw std::runtime_error("LabPBR export destination is empty");
    }
    result.specular_path = std::filesystem::absolute(normalized);
    const auto parent = result.specular_path.parent_path();
    if (parent.empty() || !std::filesystem::is_directory(parent)) {
      throw std::runtime_error("LabPBR export directory does not exist");
    }
    std::string base_stem = result.specular_path.stem().string();
    base_stem.resize(base_stem.size() - 2u);
    result.normal_path = parent / (base_stem + "_n.png");
    result.properties_path = parent / "texture.properties";

    if (normal != nullptr) {
      if (!normal->valid() ||
          normal->decoded->width != specular_width ||
          normal->decoded->height != specular_height ||
          sha256Hex(*normal->original_file_bytes) != normal->sha256) {
        throw std::runtime_error(
            "imported Iris normal bytes or checksum are invalid");
      }
    }

    std::vector<std::filesystem::path> targets{
        result.specular_path, result.properties_path};
    if (normal != nullptr) {
      targets.insert(targets.begin() + 1, result.normal_path);
    }
    for (const auto &target : targets) {
      if (std::filesystem::exists(target)) {
        result.existing_paths.push_back(target);
      }
    }
    if (!allow_overwrite && !result.existing_paths.empty()) {
      result.overwrite_required = true;
      return result;
    }

    TextureImage materialized_specular;
    const TextureImage *effective_specular = composition.specular.get();
    if (materialization_deferred) {
      std::string materialize_error;
      if (!materializeLabPbrSpecular(
              specular_width, specular_height, deferred_source_specular,
              materialized_specular, &materialize_error)) {
        throw std::runtime_error(
            materialize_error.empty()
                ? "failed to materialize deferred LabPBR specular"
                : materialize_error);
      }
      effective_specular = &materialized_specular;
    }

    const auto stage_directory =
        createTransactionDirectory(parent, "stage");
    std::filesystem::path backup_directory;
    std::vector<std::filesystem::path> stage_files;
    std::vector<std::filesystem::path> backup_files;
    std::vector<PendingFile> pending;
    try {
      backup_directory = createTransactionDirectory(parent, "backup");
      const auto staged_specular =
          stage_directory / result.specular_path.filename();
      std::vector<std::uint8_t> encoded_specular;
      std::string png_error;
      if (!encodePngRgba8(effective_specular->width,
                          effective_specular->height,
                          effective_specular->rgba, encoded_specular,
                          &png_error)) {
        throw std::runtime_error(
            png_error.empty() ? "failed to encode LabPBR specular PNG"
                              : png_error);
      }
      writeBytes(staged_specular, encoded_specular);
      stage_files.push_back(staged_specular);
      pending.push_back({result.specular_path, staged_specular,
                         backup_directory /
                             result.specular_path.filename()});

      if (normal != nullptr) {
        const auto staged_normal =
            stage_directory / result.normal_path.filename();
        writeBytes(staged_normal, *normal->original_file_bytes);
        stage_files.push_back(staged_normal);
        pending.push_back({result.normal_path, staged_normal,
                           backup_directory /
                               result.normal_path.filename()});
      }

      const auto staged_properties =
          stage_directory / result.properties_path.filename();
      writeText(staged_properties, kLabPbrProperties);
      stage_files.push_back(staged_properties);
      pending.push_back({result.properties_path, staged_properties,
                         backup_directory /
                             result.properties_path.filename()});

      TextureImage verified_specular;
      std::string validation_error;
      if (!loadTextureImage(staged_specular, verified_specular,
                            &validation_error) ||
          verified_specular.width != effective_specular->width ||
          verified_specular.height != effective_specular->height ||
          verified_specular.source_channels != 4 ||
          verified_specular.rgba != effective_specular->rgba) {
        throw std::runtime_error(
            validation_error.empty()
                ? "staged LabPBR specular PNG failed round-trip validation"
                : validation_error);
      }
      if (normal != nullptr) {
        ReadOnlyIrisNormalAsset verified_normal;
        if (!importReadOnlyIrisNormal(
                stage_directory / result.normal_path.filename(),
                effective_specular->width, effective_specular->height,
                verified_normal, &validation_error) ||
            verified_normal.original_file_bytes == nullptr ||
            normal->original_file_bytes == nullptr ||
            *verified_normal.original_file_bytes !=
                *normal->original_file_bytes ||
            verified_normal.sha256 != normal->sha256) {
          throw std::runtime_error(
              validation_error.empty()
                  ? "staged Iris normal failed byte-preservation validation"
                  : validation_error);
        }
      }
      if (readText(staged_properties) != kLabPbrProperties) {
        throw std::runtime_error(
            "staged texture.properties failed round-trip validation");
      }

      for (auto &file : pending) {
        if (std::filesystem::exists(file.target)) {
          std::filesystem::rename(file.target, file.backup);
          file.backed_up = true;
          backup_files.push_back(file.backup);
        }
      }
      for (auto &file : pending) {
        std::filesystem::rename(file.staged, file.target);
        file.installed = true;
      }

      removeKnownFilesAndDirectory(stage_directory, stage_files);
      removeKnownFilesAndDirectory(backup_directory, backup_files);
      result.success = true;
      return result;
    } catch (...) {
      std::error_code ignored;
      for (auto it = pending.rbegin(); it != pending.rend(); ++it) {
        if (it->installed) {
          std::filesystem::remove(it->target, ignored);
          ignored.clear();
        }
        if (it->backed_up && std::filesystem::exists(it->backup)) {
          std::filesystem::rename(it->backup, it->target, ignored);
          ignored.clear();
        }
      }
      removeKnownFilesAndDirectory(stage_directory, stage_files);
      if (!backup_directory.empty()) {
        // Never delete a backup file that could not be restored.
        std::filesystem::remove(backup_directory, ignored);
      }
      throw;
    }
  } catch (const std::exception &error) {
    result.error = error.what();
    return result;
  }
}

} // namespace xpbd::gfx
