#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// WindowLevelPresets
//
// Hounsfield Unit window/level presets for CT imaging.
// Window  = contrast range (width of the visible HU band)
// Level   = centre of that band (display centre in HU)
//
// Values are standard radiological conventions; adjust per modality if needed.
// ─────────────────────────────────────────────────────────────────────────────

struct WindowLevelPreset
{
    double      window;
    double      level;
    const char* name;
};

struct WindowLevelPresets
{
    // Bone / skeletal structures – wide window to capture cortical and trabecular
    static constexpr WindowLevelPreset Bone        = { 2000.0,  500.0, "Bone" };

    // Soft tissue – narrow window centred near water (0 HU)
    static constexpr WindowLevelPreset SoftTissue  = {  400.0,   40.0, "Soft Tissue" };

    // Lung parenchyma – large negative centre for air-filled structures
    static constexpr WindowLevelPreset Lung        = { 1500.0, -600.0, "Lung" };

    // Brain – tight window for grey/white matter differentiation
    static constexpr WindowLevelPreset Brain       = {   80.0,   40.0, "Brain" };

    // Abdomen soft tissue (slightly wider than generic soft tissue)
    static constexpr WindowLevelPreset Abdomen     = {  350.0,   40.0, "Abdomen" };
};
