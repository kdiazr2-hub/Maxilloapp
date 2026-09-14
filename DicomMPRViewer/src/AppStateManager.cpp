#include "AppStateManager.h"

#include <algorithm>

namespace
{
uint32_t bit(AppState state)
{
    return static_cast<uint32_t>(state);
}

uint32_t bit(AppCapability capability)
{
    return static_cast<uint32_t>(capability);
}
}

AppStateManager::AppStateManager()
{
    recompute();
}

void AppStateManager::setState(AppState state)
{
    m_states |= bit(state);
    if (state == AppState::StudyLoaded) {
        m_states &= ~bit(AppState::NoStudyLoaded);
        m_volumeLoaded = true;
    } else if (state == AppState::SegmentationAvailable) {
        m_segmentationDone = true;
    } else if (state == AppState::MeshesGenerated) {
        m_meshesGenerated = true;
    } else if (state == AppState::STLImported) {
        m_exportableObjectAvailable = true;
    } else if (state == AppState::RegistrationAccepted) {
        m_states |= bit(AppState::RegistrationComputed);
    } else if (state == AppState::CompositeCreated) {
        m_exportableObjectAvailable = true;
    } else if (state == AppState::ProjectDirty) {
        m_projectDirty = true;
    }
    recompute();
}

void AppStateManager::clearState(AppState state)
{
    m_states &= ~bit(state);
    if (state == AppState::StudyLoaded) {
        m_volumeLoaded = false;
    } else if (state == AppState::SegmentationAvailable) {
        m_segmentationDone = false;
    } else if (state == AppState::MeshesGenerated) {
        m_meshesGenerated = false;
    } else if (state == AppState::ProjectDirty) {
        m_projectDirty = false;
    }
    recompute();
}

bool AppStateManager::hasState(AppState state) const
{
    return (m_states & bit(state)) != 0;
}

void AppStateManager::setVolumeLoaded(bool v)
{
    m_volumeLoaded = v;
    if (v) {
        m_states |= bit(AppState::StudyLoaded);
        m_states &= ~bit(AppState::NoStudyLoaded);
    } else {
        reset();
        return;
    }
    recompute();
}

void AppStateManager::setSegmentationDone(bool v)
{
    m_segmentationDone = v;
    if (v) m_states |= bit(AppState::SegmentationAvailable);
    else m_states &= ~bit(AppState::SegmentationAvailable);
    recompute();
}

void AppStateManager::setMeshesGenerated(bool v)
{
    m_meshesGenerated = v;
    if (v) m_states |= bit(AppState::MeshesGenerated);
    else m_states &= ~bit(AppState::MeshesGenerated);
    recompute();
}

void AppStateManager::setUpperArchImported(bool v)
{
    m_upperArchImported = v;
    if (!v) {
        m_upperRegistered = false;
        m_upperCompositeReady = false;
        m_upperPointPairCount = 0;
    }
    if (m_upperArchImported || m_lowerArchImported) m_states |= bit(AppState::STLImported);
    else m_states &= ~bit(AppState::STLImported);
    recompute();
}

void AppStateManager::setLowerArchImported(bool v)
{
    m_lowerArchImported = v;
    if (!v) {
        m_lowerRegistered = false;
        m_lowerCompositeReady = false;
        m_lowerPointPairCount = 0;
    }
    if (m_upperArchImported || m_lowerArchImported) m_states |= bit(AppState::STLImported);
    else m_states &= ~bit(AppState::STLImported);
    recompute();
}

void AppStateManager::setMaxillaAvailable(bool v)
{
    m_maxillaAvailable = v;
    recompute();
}

void AppStateManager::setMandibleAvailable(bool v)
{
    m_mandibleAvailable = v;
    recompute();
}

void AppStateManager::setUpperPointPairCount(int count)
{
    m_upperPointPairCount = std::max(0, count);
    if (m_upperPointPairCount >= 3 || m_lowerPointPairCount >= 3)
        m_states |= bit(AppState::LandmarksSelected);
    else
        m_states &= ~bit(AppState::LandmarksSelected);
    recompute();
}

void AppStateManager::setLowerPointPairCount(int count)
{
    m_lowerPointPairCount = std::max(0, count);
    if (m_upperPointPairCount >= 3 || m_lowerPointPairCount >= 3)
        m_states |= bit(AppState::LandmarksSelected);
    else
        m_states &= ~bit(AppState::LandmarksSelected);
    recompute();
}

void AppStateManager::setAnyRegistrationPoints(bool v)
{
    m_anyRegistrationPoints = v;
    recompute();
}

void AppStateManager::setUpperRegistered(bool v)
{
    m_upperRegistered = v;
    if (!v) {
        m_upperCompositeReady = false;
    }
    if (m_upperRegistered || m_lowerRegistered) {
        m_states |= bit(AppState::RegistrationComputed);
        m_states |= bit(AppState::RegistrationAccepted);
    } else {
        m_states &= ~bit(AppState::RegistrationComputed);
        m_states &= ~bit(AppState::RegistrationAccepted);
    }
    recompute();
}

void AppStateManager::setLowerRegistered(bool v)
{
    m_lowerRegistered = v;
    if (!v) {
        m_lowerCompositeReady = false;
    }
    if (m_upperRegistered || m_lowerRegistered) {
        m_states |= bit(AppState::RegistrationComputed);
        m_states |= bit(AppState::RegistrationAccepted);
    } else {
        m_states &= ~bit(AppState::RegistrationComputed);
        m_states &= ~bit(AppState::RegistrationAccepted);
    }
    recompute();
}

void AppStateManager::setUpperCompositeReady(bool v)
{
    m_upperCompositeReady = v;
    if (m_upperCompositeReady || m_lowerCompositeReady)
        m_states |= bit(AppState::CompositeCreated);
    else
        m_states &= ~bit(AppState::CompositeCreated);
    recompute();
}

void AppStateManager::setLowerCompositeReady(bool v)
{
    m_lowerCompositeReady = v;
    if (m_upperCompositeReady || m_lowerCompositeReady)
        m_states |= bit(AppState::CompositeCreated);
    else
        m_states &= ~bit(AppState::CompositeCreated);
    recompute();
}

void AppStateManager::setExportableObjectAvailable(bool v)
{
    m_exportableObjectAvailable = v;
    recompute();
}

void AppStateManager::setProjectDirty(bool v)
{
    m_projectDirty = v;
    if (v) m_states |= bit(AppState::ProjectDirty);
    else m_states &= ~bit(AppState::ProjectDirty);
    recompute();
}

void AppStateManager::reset()
{
    m_states = bit(AppState::NoStudyLoaded);
    m_caps = 0;
    m_volumeLoaded = false;
    m_segmentationDone = false;
    m_meshesGenerated = false;
    m_upperArchImported = false;
    m_lowerArchImported = false;
    m_maxillaAvailable = false;
    m_mandibleAvailable = false;
    m_upperPointPairCount = 0;
    m_lowerPointPairCount = 0;
    m_anyRegistrationPoints = false;
    m_upperRegistered = false;
    m_lowerRegistered = false;
    m_upperCompositeReady = false;
    m_lowerCompositeReady = false;
    m_exportableObjectAvailable = false;
    m_projectDirty = false;
    recompute();
}

bool AppStateManager::can(AppCapability c) const
{
    return (m_caps & bit(c)) != 0;
}

void AppStateManager::recompute()
{
    uint32_t caps = 0;
    auto set = [&](AppCapability c) { caps |= bit(c); };

    set(AppCapability::LoadDicom);

    if (m_volumeLoaded) {
        set(AppCapability::RunSegmentation);
        set(AppCapability::ImportUpperArch);
        set(AppCapability::ImportLowerArch);
        set(AppCapability::SaveProject);
    }

    if (m_volumeLoaded && m_segmentationDone) {
        set(AppCapability::SplitBone);
    }

    if (m_upperArchImported || m_lowerArchImported) {
        set(AppCapability::ResetStl);
        set(AppCapability::ExportComposite);
    }

    if (m_maxillaAvailable && m_upperArchImported) {
        set(AppCapability::SelectMaxillaryPoints);
    }

    if (m_mandibleAvailable && m_lowerArchImported) {
        set(AppCapability::SelectMandibularPoints);
    }

    const bool canUpper = m_maxillaAvailable && m_upperArchImported && m_upperPointPairCount >= 3;
    const bool canLower = m_mandibleAvailable && m_lowerArchImported && m_lowerPointPairCount >= 3;
    if (canUpper) set(AppCapability::MatchUpper);
    if (canLower) set(AppCapability::MatchLower);
    if (canUpper && canLower) set(AppCapability::MatchBoth);

    if (m_upperRegistered || m_lowerRegistered) {
        set(AppCapability::CreateComposite);
        set(AppCapability::ExportPackage);
    }

    if (m_upperCompositeReady || m_lowerCompositeReady || m_exportableObjectAvailable) {
        set(AppCapability::ExportComposite);
    }

    if (m_anyRegistrationPoints) {
        set(AppCapability::ClearPoints);
    }

    m_caps = caps;
}
