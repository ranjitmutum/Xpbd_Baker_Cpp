#include "xpbd/app/native_dialog.hpp"

namespace xpbd::app {
namespace {

NativeDialogHooks g_hooks{};
int g_dialog_depth = 0;

} // namespace

void setNativeDialogHooks(const NativeDialogHooks &hooks) { g_hooks = hooks; }

NativeDialogHooks nativeDialogHooks() noexcept { return g_hooks; }

bool nativeDialogOpen() noexcept { return g_dialog_depth > 0; }

NativeDialogScope::NativeDialogScope() {
  if (g_dialog_depth == 0) {
    if (g_hooks.prepare && !g_hooks.prepare()) {
      return;
    }
  }
  ++g_dialog_depth;
  ready_ = true;
}

NativeDialogScope::~NativeDialogScope() {
  if (!ready_) {
    return;
  }
  if (g_dialog_depth > 0) {
    --g_dialog_depth;
  }
  if (g_dialog_depth == 0) {
    if (g_hooks.finish) {
      g_hooks.finish();
    }
  }
}

} // namespace xpbd::app
