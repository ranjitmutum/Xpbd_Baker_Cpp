#include "xpbd/loader/animation_loader.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>

namespace xpbd::loader {

AnimationRoot AnimationLoader::load(const std::filesystem::path &file_path) {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Failed to open animation file: " +
                             file_path.string());
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  try {
    return loadFromString(ss.str());
  } catch (const std::exception &e) {
    throw std::runtime_error(std::string("Invalid animation JSON: ") +
                             e.what());
  }
}

AnimationRoot AnimationLoader::loadFromString(const std::string &json_text) {
  nlohmann::ordered_json root = nlohmann::ordered_json::parse(json_text);
  if (!root.is_object()) {
    throw std::runtime_error("Animation JSON root must be an object");
  }
  if (!root.contains("animations") || !root.at("animations").is_object()) {
    throw std::runtime_error(
        "Not a valid Bedrock animation: missing 'animations' object");
  }
  return AnimationRoot::fromOrderedJson(root);
}

}
