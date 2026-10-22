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
    ThresholdSegmentDialog(bool bone, const HuHistogram& histogram, QWidget* parent = nullptr);

    bool forBone() const { return m_bone; }
    ThresholdRequest request() const;

signals:
    void rangeEdited(double minHu, double maxHu);

private:
    void applyPreset(int index);
    void setBoneMode(bool bone);
    void setRange(double minHu, double maxHu, bool fromUser);
    void emitRange();

    bool m_bone = true;
    HuHistogram m_histogram;
    HuRangeWidget* m_histogramView = nullptr;
    QRadioButton* m_boneChoice = nullptr;
    QRadioButton* m_softChoice = nullptr;
    QLabel* m_hint = nullptr;
    QComboBox* m_preset = nullptr;
    QSpinBox* m_minBox = nullptr;
    QSpinBox* m_maxBox = nullptr;
    QCheckBox* m_fillHoles = nullptr;
    QCheckBox* m_keepLargest = nullptr;
    QPushButton* m_create = nullptr;
    bool m_updating = false;
};
