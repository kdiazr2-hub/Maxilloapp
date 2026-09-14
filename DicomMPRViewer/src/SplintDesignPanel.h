#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SplintDesignPanel
//
// Controls of the height-map splint workspace. Pure view: setters never emit
// signals, user actions are reported as intents and MainWindow owns the
// designs, points and previews.
// ─────────────────────────────────────────────────────────────────────────────

#include "SplintHeightmapGenerator.h"

#include <QString>
#include <QStringList>
#include <QWidget>

#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;

class SplintDesignPanel : public QWidget
{
    Q_OBJECT

public:
    struct SourceOption
    {
        QString name;
        int value = 0;
    };
    enum PointGroup
    {
        NoGroup = -1,
        UpperGroup = 0,
        LowerGroup = 1
    };

    enum ExtrasTool
    {
        NoExtrasTool = 0,
        BevelTool = 1,
        WireHoleTool = 2,
        BracketTool = 3
    };

    explicit SplintDesignPanel(QWidget* parent = nullptr);

    void setExtrasTool(int tool);
    void setExtrasSettings(double holeDiameterMm, SplintHoleOrientation orientation, double bracketOffsetMm,
                           double brushRadiusMm);
    void setExtrasSummary(bool bevel, bool bevelPending, int holes, int marks);
    double wireHoleDiameter() const;
    SplintHoleOrientation wireHoleOrientation() const;
    double bracketOffset() const;
    double brushRadius() const;
    // Empty hides the warning.
    void setStaleWarning(const QString& text);

    void setDesigns(const QStringList& names, int currentIndex, bool currentIsBuiltIn);
    void setSourceOptions(const std::vector<SourceOption>& upper, const std::vector<SourceOption>& lower);
    void setSources(int upperValue, int lowerValue);
    int upperSource() const;
    int lowerSource() const;

    void setParams(const SplintHeightmapParams& params);
    // Returns `base` with the values edited in the panel.
    SplintHeightmapParams params(const SplintHeightmapParams& base) const;

    void setPointCounts(int upper, int lower);
    void setActivePointGroup(int group);
    void setContourEditing(bool editing);
    void setContourEdited(bool edited);
    void setInfluencePercent(double percent);
    double influencePercent() const;
    bool showThickness() const;
    void setPreviewStatus(const QString& text);
    void setReport(const QString& text);
    void setCanCreate(bool enabled);
    void setCanExport(bool enabled);

signals:
    void designSelected(int index);
    void newDesignRequested();
    void copyDesignRequested();
    void renameDesignRequested();
    void deleteDesignRequested();
    void sourcesChanged();
    void loadTestStlRequested();
    void pointGroupToggled(int group, bool active);
    void clearPointsRequested(int group);
    void paramsChanged();
    void showThicknessToggled(bool show);
    void contourEditToggled(bool editing);
    void resetContourRequested();
    void influenceChanged(double percent);
    void createRequested();
    void exportRequested();
    void exportPointsRequested();
    void exportReportRequested();
    void extrasToolToggled(int tool, bool active);
    void removeBevelRequested();
    void removeWireHolesRequested();
    void clearBracketMarksRequested();
    void extrasSettingsChanged();

private:
    void updateUndercutEnabled();

    bool m_updating = false;

    QComboBox* m_designCombo = nullptr;
    QPushButton* m_deleteDesignButton = nullptr;
    QComboBox* m_upperSourceCombo = nullptr;
    QComboBox* m_lowerSourceCombo = nullptr;

    QPushButton* m_upperPointsButton = nullptr;
    QPushButton* m_lowerPointsButton = nullptr;
    QLabel* m_pointStatus = nullptr;

    QDoubleSpinBox* m_edgeOffsetSpin = nullptr;
    QDoubleSpinBox* m_filletSpin = nullptr;
    QDoubleSpinBox* m_clearanceSpin = nullptr;
    QDoubleSpinBox* m_minFeatureSpin = nullptr;
    QCheckBox* m_impressionUpperCheck = nullptr;
    QCheckBox* m_impressionLowerCheck = nullptr;
    QCheckBox* m_undercutUpperCheck = nullptr;
    QCheckBox* m_undercutLowerCheck = nullptr;

    QCheckBox* m_showThicknessCheck = nullptr;
    QDoubleSpinBox* m_minThicknessSpin = nullptr;
    QDoubleSpinBox* m_maxThicknessSpin = nullptr;

    QPushButton* m_editContourButton = nullptr;
    QPushButton* m_resetContourButton = nullptr;
    QDoubleSpinBox* m_influenceSpin = nullptr;
    QLabel* m_contourNote = nullptr;

    QPushButton* m_bevelButton = nullptr;
    QPushButton* m_removeBevelButton = nullptr;
    QPushButton* m_holeButton = nullptr;
    QPushButton* m_removeHolesButton = nullptr;
    QDoubleSpinBox* m_holeDiameterSpin = nullptr;
    QComboBox* m_holeOrientationCombo = nullptr;
    QPushButton* m_bracketButton = nullptr;
    QPushButton* m_clearMarksButton = nullptr;
    QComboBox* m_bracketOffsetCombo = nullptr;
    QDoubleSpinBox* m_brushRadiusSpin = nullptr;
    QLabel* m_extrasSummary = nullptr;
    QLabel* m_staleWarning = nullptr;

    QLabel* m_previewStatus = nullptr;
    QLabel* m_report = nullptr;
    QPushButton* m_createButton = nullptr;
    QPushButton* m_exportButton = nullptr;
};
