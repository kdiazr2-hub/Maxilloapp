#pragma once

#include <QObject>
#include <QProcess>
#include <QString>
#include <vtkSmartPointer.h>

class vtkImageData;

// ─────────────────────────────────────────────────────────────────────────────
// BoneSplitterService
//
// Splits the bone label (1) in a segmentation labelmap into:
//   Label 5 — Maxilar   (upper jaw)
//   Label 6 — Mandíbula (lower jaw)
//
// Uses an external Python script (split_maxilla_mandible.py) that runs
// connected-component analysis (scipy) with a Z-profile fallback.
//
// Workflow:
//   1. Export the labelmap to a temp NRRD file
//   2. Locate Python + the script (same logic as StandaloneDentalSegmentatorService)
//   3. Run the script via QProcess (async, streams progress to stdout)
//   4. On success, emit splitFinished(outputNrrdPath)
// ─────────────────────────────────────────────────────────────────────────────
class BoneSplitterService : public QObject
{
    Q_OBJECT

public:
    explicit BoneSplitterService(QObject* parent = nullptr);
    ~BoneSplitterService() override;

    // Begin the split operation.
    // labelmap  : current full segmentation labelmap
    // outputDir : directory where temp + result NRRDs will be written
    void split(vtkSmartPointer<vtkImageData> labelmap,
               const QString& outputDir);

    bool isRunning() const;

signals:
    void statusChanged(const QString& message);
    void progressChanged(int percent);
    void splitFinished(const QString& outputNrrdPath);
    void errorOccurred(const QString& message);

private slots:
    void onReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    QString findPython() const;
    QString findScript() const;
    void emitError(const QString& msg);

    QProcess* m_process   = nullptr;
    QString   m_outputPath;
};
