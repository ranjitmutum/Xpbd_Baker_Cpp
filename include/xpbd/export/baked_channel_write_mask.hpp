#pragma once

namespace xpbd::export_ {


struct BakedChannelWriteMask {
  bool position = true;
  bool rotation = true;
  bool scale = false;
};

inline constexpr BakedChannelWriteMask kBakedChannelWriteMask{};

}
