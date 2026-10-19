#pragma once

#include <QFile>
#include <QFutureWatcher>
#include <QProcess>
#include <QString>
#include <QStringList>

#include "AISegmentationService.h"

struct StandaloneSegmentationResult
{
    bool ok = false;
    bool needsExternalProcess = false;
    QString inputPath;
    QString outputPath;
    SegmentationTarget target = SegmentationTarget::Bone;
    QString error;
};

class StandaloneDentalSegmentatorService : public AISegmentationService
{
    Q_OBJECT

public:
    explicit StandaloneDentalSegmentatorService(QObject* parent = nullptr);
    ~StandaloneDentalSegmentatorService() override;

    void segment(vtkSmartPointer<vtkImageData> inputVolume,
                 const QString& outputDirectory,
                 SegmentationTarget target) override;
    void cancel() override;

    bool isRunning() const;

private slots:
    void onWorkerFinished();
    void onReadyReadStdout();
    void onReadyReadStderr();
    void onProcessFinished(int exitCode, QProcess::ExitStatus status);
    void onProcessError(QProcess::ProcessError error);

private:
    struct ExternalCommand
    {
        QString program;
        QStringList args;
        bool isValid() const { return !program.isEmpty(); }
    };

    ExternalCommand resolveExternalCommand(const QString& inputPath,
                                           const QString& outputPath,
                                           SegmentationTarget target) const;
    QString resolveScriptPath() const;
    void startExternalProcess(const QString& inputPath, const QString& outputPath);
    void appendProcessText(const QByteArray& data, bool stderrChannel);

    QString m_outputDirectory;
    QString m_outputSegmentationPath;
    QString m_processLog;
    // The run's output, as it arrives, in segmentacion.log next to the output: a frozen or killed run has no
    // error dialog, and the file still says which step it reached (user's report, 2026-10-19: "se congeló").
    QFile m_processLogFile;
    ExternalCommand m_externalCommand;
    QProcess* m_process = nullptr;
    QFutureWatcher<StandaloneSegmentationResult>* m_workerWatcher = nullptr;
};
