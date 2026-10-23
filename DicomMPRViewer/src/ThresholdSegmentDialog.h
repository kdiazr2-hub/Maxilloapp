#pragma once

#include "ThresholdMaskCore.h"

#include <QDialog>

class QComboBox;
class QCheckBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QSpinBox;
class HuRangeWidget;

// Modeless window: the three slices behind it keep showing the window as it moves.
class ThresholdSegmentDialog : public QDialog
{
    Q_OBJECT

public:
    ThresholdSegmentDialog(bool bone, const HuHistogram& histogram, QWidget* parent = nullptr,
                           bool coneBeam = false, double suggestedMin = 0.0, double suggestedMax = 0.0);

    bool forBone() const { return m_bone; }
    ThresholdRequest request() const;
    bool wantsSeed() const;
    void setSeed(double x, double y, double z);

signals:
    void rangeEdited(double minHu, double maxHu);
    void seedModeChanged(bool on);

private:
    void applyPreset(int index);
    void setBoneMode(bool bone, int presetIndex);
    void setRange(double minHu, double maxHu, bool fromUser);
    void emitRange();
    void showSuggestedConeBeamRange();

    bool m_bone = true;
    bool m_coneBeam = false;
    double m_suggestedMin = 0.0;
    double m_suggestedMax = 0.0;
    HuHistogram m_histogram;
    HuRangeWidget* m_histogramView = nullptr;
    QRadioButton* m_boneChoice = nullptr;
    QRadioButton* m_softChoice = nullptr;
    QLabel* m_hint = nullptr;
    QLabel* m_coneBeamNote = nullptr;
    QLabel* m_seedLabel = nullptr;
    QComboBox* m_preset = nullptr;
    QSpinBox* m_minBox = nullptr;
    QSpinBox* m_maxBox = nullptr;
    QCheckBox* m_fillHoles = nullptr;
    QCheckBox* m_keepConnected = nullptr;
    QPushButton* m_create = nullptr;
    bool m_updating = false;
    bool m_hasSeed = false;
    double m_seed[3] = {0.0, 0.0, 0.0};
};
