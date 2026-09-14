#pragma once

#include <cstdint>

enum class AppState : uint32_t
{
    NoStudyLoaded          = 1u << 0,
    StudyLoaded            = 1u << 1,
    SegmentationAvailable  = 1u << 2,
    MeshesGenerated        = 1u << 3,
    STLImported            = 1u << 4,
    LandmarksSelected      = 1u << 5,
    RegistrationComputed   = 1u << 6,
    RegistrationAccepted   = 1u << 7,
    CompositeCreated       = 1u << 8,
    ProjectDirty           = 1u << 9,
};

enum class AppCapability : uint32_t
{
    LoadDicom              = 1u << 0,
    RunSegmentation        = 1u << 1,
    SplitBone              = 1u << 2,
    ImportUpperArch        = 1u << 3,
    ImportLowerArch        = 1u << 4,
    SelectMaxillaryPoints  = 1u << 5,
    SelectMandibularPoints = 1u << 6,
    MatchUpper             = 1u << 7,
    MatchLower             = 1u << 8,
    MatchBoth              = 1u << 9,
    CreateComposite        = 1u << 10,
    ExportComposite        = 1u << 11,
    ExportPackage          = 1u << 12,
    SaveProject            = 1u << 13,
    ClearPoints            = 1u << 14,
    ResetStl               = 1u << 15,
};

class AppStateManager
{
public:
    AppStateManager();

    void setState(AppState state);
    void clearState(AppState state);
    bool hasState(AppState state) const;

    void setVolumeLoaded(bool v);
    void setSegmentationDone(bool v);
    void setMeshesGenerated(bool v);
    void setUpperArchImported(bool v);
    void setLowerArchImported(bool v);
    void setMaxillaAvailable(bool v);
    void setMandibleAvailable(bool v);
    void setUpperPointPairCount(int count);
    void setLowerPointPairCount(int count);
    void setAnyRegistrationPoints(bool v);
    void setUpperRegistered(bool v);
    void setLowerRegistered(bool v);
    void setUpperCompositeReady(bool v);
    void setLowerCompositeReady(bool v);
    void setExportableObjectAvailable(bool v);
    void setProjectDirty(bool v);

    void reset();

    bool can(AppCapability c) const;
    uint32_t capabilities() const { return m_caps; }

    bool canOpenDicom() const { return true; }
    bool canSaveProject() const { return can(AppCapability::SaveProject); }
    bool canUseImagePresets() const { return m_volumeLoaded; }
    bool canUseMeasurements() const { return m_volumeLoaded || m_exportableObjectAvailable; }
    bool canRunDentalAI() const { return can(AppCapability::RunSegmentation); }
    bool canShowSegmentationOverlay() const { return m_segmentationDone; }
    bool canEditMask() const { return m_segmentationDone; }
    bool canImportSTL() const { return m_volumeLoaded; }
    bool canSelectMaxillaryPoints() const { return can(AppCapability::SelectMaxillaryPoints); }
    bool canSelectMandibularPoints() const { return can(AppCapability::SelectMandibularPoints); }
    bool canRegisterMaxilla() const { return can(AppCapability::MatchUpper); }
    bool canRegisterMandible() const { return can(AppCapability::MatchLower); }
    bool canRegisterBoth() const { return can(AppCapability::MatchBoth); }
    bool canCreateComposite() const { return can(AppCapability::CreateComposite); }
    bool canExportSTL() const { return can(AppCapability::ExportComposite); }
    bool canExportRegistration() const { return can(AppCapability::ExportPackage); }
    bool canClearPoints() const { return can(AppCapability::ClearPoints); }
    bool canResetSTL() const { return can(AppCapability::ResetStl); }

private:
    void recompute();

    uint32_t m_states = static_cast<uint32_t>(AppState::NoStudyLoaded);
    uint32_t m_caps = 0;

    bool m_volumeLoaded = false;
    bool m_segmentationDone = false;
    bool m_meshesGenerated = false;
    bool m_upperArchImported = false;
    bool m_lowerArchImported = false;
    bool m_maxillaAvailable = false;
    bool m_mandibleAvailable = false;
    int  m_upperPointPairCount = 0;
    int  m_lowerPointPairCount = 0;
    bool m_anyRegistrationPoints = false;
    bool m_upperRegistered = false;
    bool m_lowerRegistered = false;
    bool m_upperCompositeReady = false;
    bool m_lowerCompositeReady = false;
    bool m_exportableObjectAvailable = false;
    bool m_projectDirty = false;
};
