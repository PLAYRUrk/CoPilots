#pragma once
// Theme.h — CoPilots dark modern ImGui theme.
// Call apply() once after ImGui context is created.

namespace cp {
namespace ui {

struct Theme {
    static void apply();
};

// Shared accent colour (blue-ish)
static constexpr float kAccentR = 0.22f;
static constexpr float kAccentG = 0.56f;
static constexpr float kAccentB = 0.94f;
static constexpr float kAccentA = 1.00f;

} // namespace ui
} // namespace cp
