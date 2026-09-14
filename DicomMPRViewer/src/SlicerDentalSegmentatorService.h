#pragma once

#include <QFutureWatcher>
#include <QProcess>
#include <QString>

#include "AISegmentationService.h"

struct SegmentationExportResult
{
    bool ok = false;
    QString inputPath;
    QString outputPath;
    QString scriptPath;
    QString error;
};

class SlicerDentalSegmentatorService : public AISegmentationService
{
    Q_OBJECT

public:
    explicit SlicerDentalSegmentatorService(QObject* parent = nullptr);
    ~SlicerDentalSegmentatorService() override;

    void segment(vtkSmartPointer<vtkImageData> inputVolume,
                 const QString& outputDirectory,
                 SegmentationTarget target) override;
    void cancel() override;

    void setSlicerExecutablePath(const QString& path) { m_slicerExecutablePath = path; }
    QString slicerExecutablePath() const { return m_slicerExecutablePath; }
    QString autoDetectSlicerExecutable() const { return resolveSlicerExecutable(); }
    bool isRunning() const;

private slots:
    void onExportFinished();
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    QString resolveSlicerExecutable() const;
    QString resolveScriptPath() const;
    void startSlicerProcess(const SegmentationExportResult& exportResult);
    void appendProcessText(const QByteArray& data);

    QString m_slicerExecutablePath;
    QString m_outputDirectory;
    QString m_outputSegmentationPath;
    QProcess* m_process = nullptr;
    QFutureWatcher<SegmentationExportResult>* m_exportWatcher = nullptr;
};
