#pragma once
#include <QDialog>
#include <QElapsedTimer>
class QLabel;
class QProgressBar;
class QPushButton;

class SegmentationProgressDialog : public QDialog {
    Q_OBJECT
public:
    explicit SegmentationProgressDialog(QWidget* parent);
    void setStage(const QString& text);
    void setProgress(int percent);
    void setCancelling();
    void reject() override;
signals:
    void cancelRequested();
private:
    QLabel* m_stage;
    QLabel* m_elapsed;
    QProgressBar* m_bar;
    QPushButton* m_cancel;
    QElapsedTimer m_clock;
    bool m_cancelling = false;
};
