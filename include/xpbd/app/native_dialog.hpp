#pragma once

// Native OS file dialogs must not race the GPU render path (especially RT).
// Hooks pause presentation before the modal loop and resume after it returns.

#include <cstdint>

namespace xpbd::app {

struct NativeDialogHooks {
  // Platform window handle (HWND on Windows); may be null.
  void *owner_window = nullptr;
  // Called on the UI thread immediately before the modal dialog runs.
  bool (*prepare)() = nullptr;
  // Called after the modal dialog returns (success or cancel).
  void (*finish)() = nullptr;
};

void setNativeDialogHooks(const NativeDialogHooks &hooks);
[[nodiscard]] NativeDialogHooks nativeDialogHooks() noexcept;

// True while a system file dialog is on the UI thread.
[[nodiscard]] bool nativeDialogOpen() noexcept;

// RAII: prepare → … → finish. Nested opens are counted.
class NativeDialogScope {
public:
  NativeDialogScope();
  ~NativeDialogScope();
  NativeDialogScope(const NativeDialogScope &) = delete;
  NativeDialogScope &operator=(const NativeDialogScope &) = delete;

  [[nodiscard]] bool ready() const noexcept { return ready_; }

private:
  bool ready_ = false;
};

} // namespace xpbd::app
