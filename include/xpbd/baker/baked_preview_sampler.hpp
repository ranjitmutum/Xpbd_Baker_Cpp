#pragma once

#include "xpbd/baker/baked_frame.hpp"
#include "xpbd/loader/bedrock_model_data.hpp"

#include <array>
#include <vector>

namespace xpbd::baker {








struct BakedPreviewScratch {
  BakedFrame frame;
  std::vector<std::array<double, 3>> bind_rotations;

  void reset();
};


class BakedPreviewSampler {
public:










  [[nodiscard]] static const BakedFrame &
  sample(const std::vector<BakedFrame> &frames,
         const std::vector<loader::Bone> &model_bones, double time,
         BakedPreviewScratch &scratch, double loop_duration = 0.0);
};

}
