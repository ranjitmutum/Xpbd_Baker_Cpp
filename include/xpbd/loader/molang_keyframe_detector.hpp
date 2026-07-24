#pragma once

#include "xpbd/export/baked_channel_write_mask.hpp"
#include "xpbd/loader/bedrock_animation_data.hpp"

#include <set>
#include <string>
#include <vector>

namespace xpbd::loader {

enum class MolangChannel { Position, Rotation, Scale };

struct MolangOverwrite {
  std::string bone_name;
  MolangChannel channel = MolangChannel::Position;

  bool operator==(const MolangOverwrite &) const = default;
};

enum class MolangAnimationRole { Source, TransitionTarget };
enum class MolangBakeAction { SampleAsZeroPreserve, OverwriteWithBakedKeys };


struct MolangBakeWarning {
  MolangAnimationRole role = MolangAnimationRole::Source;
  MolangBakeAction action = MolangBakeAction::SampleAsZeroPreserve;
  std::string animation_name;
  std::string bone_name;
  MolangChannel channel = MolangChannel::Position;

  bool operator==(const MolangBakeWarning &) const = default;
};


class MolangKeyframeDetector {
public:
  [[nodiscard]] static std::vector<MolangOverwrite>
  findOverwrittenChannels(const Animation *animation,
                          const std::vector<std::string> &selected_bone_ids,
                          export_::BakedChannelWriteMask write_mask =
                              export_::kBakedChannelWriteMask);




  [[nodiscard]] static std::vector<MolangBakeWarning>
  findBakeWarnings(const Animation *animation, MolangAnimationRole role,
                   const std::string &animation_name,
                   const std::set<std::string> &input_dependency_ids,
                   const std::vector<std::string> &selected_bone_ids,
                   export_::BakedChannelWriteMask write_mask =
                       export_::kBakedChannelWriteMask);

  [[nodiscard]] static bool
  hasMolangKeyframes(const Animation *animation,
                     const std::vector<std::string> &selected_bone_ids);

  [[nodiscard]] static const char *channelName(MolangChannel channel);
};

}
