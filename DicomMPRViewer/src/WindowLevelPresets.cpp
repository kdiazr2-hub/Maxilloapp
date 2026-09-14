#include "WindowLevelPresets.h"

// All preset values are defined as constexpr in the header.
// This translation unit exists so the linker finds the symbols when
// their addresses are taken (ODR-used), e.g. passing by const-ref.
constexpr WindowLevelPreset WindowLevelPresets::Bone;
constexpr WindowLevelPreset WindowLevelPresets::SoftTissue;
constexpr WindowLevelPreset WindowLevelPresets::Lung;
constexpr WindowLevelPreset WindowLevelPresets::Brain;
constexpr WindowLevelPreset WindowLevelPresets::Abdomen;
