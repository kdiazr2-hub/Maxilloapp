#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// OsteotomyWizardPanel
//
// ProPlan-style "Plan Osteotomy" wizard: select osteotomy type → select bone →
// indicate landmark points → modify cutting path → finalize. Pure view:
// setters never emit signals, user actions are reported as intents and
// MainWindow owns the plan, the meshes and the 3D interaction.
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QString>
#include <QStringList>
#include <QWidget>

#include <array>
#include <vector>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QRadioButton;
class QStackedWidget;
class QTableWidget;
class QVBoxLayout;

class OsteotomyWizardPanel : public QWidget
{
    Q_OBJECT

public:
    enum Step
    {
        TypeStep = 0,
        BoneStep = 1,
        LandmarkStep = 2,
        PathStep = 3,
        FinalizeStep = 4
    };
    enum NextAction
    {
        AnotherOsteotomy = 0,
        OcclusionRegistration = 1,
        Reposition = 2
    };
    // Cutting path properties; BSSO uses thickness and the three extensions.
    struct PathProperties
    {
        double widthMm = 120.0;
        double thicknessMm = 1.0;
        double extensionRightMm = 20.0;
        double extensionLeftMm = 20.0;
        double posteriorExtensionMm = 20.0;
        double inferiorExtensionMm = 30.0;
        double mediolateralExtensionMm = 15.0;
    };
    struct ObjectRow
    {
        QString name;
        QColor color;
    };
    struct BoneOption
    {
        QString name;
        int label = 0;
    };

    explicit OsteotomyWizardPanel(QWidget* parent = nullptr);

    void setStep(int step);
    int step() const { return m_step; }
    // type index = OsteotomyType; done marks osteotomies already applied.
    void setTypes(const QStringList& names, const std::array<bool, 3>& available, const std::array<bool, 3>& done,
                  int current);
    int selectedType() const;
    void setBoneOptions(const std::vector<BoneOption>& options, int currentLabel, const QString& linkNote);
    int selectedBone() const;
    // One entry per landmark; current = landmark being indicated (-1 none).
    void setLandmarks(const QStringList& names, const QStringList& hints, const std::vector<bool>& placed, int current);
    void setPathMode(bool bsso);
    void setPathProperties(const PathProperties& properties);
    PathProperties pathProperties() const;
    void setGizmoActive(bool active);
    int gizmoSide() const; // BSSO: 0 right, 1 left
    bool showContour() const;
    void setShowContour(bool show);
    void setCreatedObjects(const std::vector<ObjectRow>& rows);
    int nextAction() const;
    void setNavigation(bool canBack, bool canNext, const QString& nextText);
    void setStatus(const QString& text);

signals:
    void typeChosen(int type);
    void boneChosen(int label);
    void landmarkChosen(int index);
    void previousLandmarkRequested();
    void nextLandmarkRequested();
    void clearLandmarksRequested();
    void gizmoToggled(bool active);
    void resetPathRequested();
    void propertiesChanged();
    void showContourToggled(bool show);
    void showSlicesRequested();
    void backRequested();
    void nextRequested();
    void cancelRequested();

private:
    void updateStepIndicator();

    bool m_updating = false;
    int m_step = TypeStep;

    std::array<QLabel*, 5> m_stepDots{};
    std::array<QLabel*, 5> m_stepTexts{};
    QLabel* m_title = nullptr;
    QStackedWidget* m_stack = nullptr;

    QButtonGroup* m_typeGroup = nullptr;
    std::array<QRadioButton*, 3> m_typeButtons{};

    QComboBox* m_boneCombo = nullptr;
    QLabel* m_linkNote = nullptr;

    QLabel* m_landmarkPrompt = nullptr;
    QLabel* m_landmarkHint = nullptr;
    QWidget* m_landmarkList = nullptr;
    QVBoxLayout* m_landmarkLayout = nullptr;
    std::vector<QPushButton*> m_landmarkButtons;

    QPushButton* m_gizmoButton = nullptr;
    QComboBox* m_gizmoSideCombo = nullptr;
    QWidget* m_pathPropertiesBox = nullptr;
    QWidget* m_bssoPropertiesBox = nullptr;
    QDoubleSpinBox* m_widthSpin = nullptr;
    QDoubleSpinBox* m_thicknessSpin = nullptr;
    QDoubleSpinBox* m_extRightSpin = nullptr;
    QDoubleSpinBox* m_extLeftSpin = nullptr;
    QDoubleSpinBox* m_bssoThicknessSpin = nullptr;
    QDoubleSpinBox* m_posteriorSpin = nullptr;
    QDoubleSpinBox* m_inferiorSpin = nullptr;
    QDoubleSpinBox* m_mediolateralSpin = nullptr;
    QCheckBox* m_showContourCheck = nullptr;
    bool m_bssoMode = false;

    QTableWidget* m_objectTable = nullptr;
    QButtonGroup* m_nextGroup = nullptr;

    QLabel* m_status = nullptr;
    QPushButton* m_backButton = nullptr;
    QPushButton* m_nextButton = nullptr;
};
