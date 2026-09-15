#include "AISegmentationService.h"
#include <QDir>
#include <QProcess>

AISegmentationService::AISegmentationService(QObject* parent)
    : QObject(parent)
{
}

AISegmentationService::~AISegmentationService() = default;

void AISegmentationService::beginRun()
{
    m_runActive = true;
    m_cancelRequested = false;
    m_stdoutProgress.reset();
    m_stderrProgress.reset();
}
void AISegmentationService::failRun(const QString& error)
{
    if (!m_runActive) return;
    if (m_cancelRequested) { finishCancelled(); return; }
    m_runActive = false;
    emit errorOccurred(error);
}
void AISegmentationService::succeedRun(const QString& path)
{
    if (!m_runActive) return;
    if (m_cancelRequested) { finishCancelled(); return; }
    m_runActive = false;
    emit segmentationFinished(path);
}
void AISegmentationService::finishCancelled()
{
    if (!m_runActive) return;
    m_runActive = false;
    emit cancelled();
}
void AISegmentationService::publishProcessProgress(const QByteArray& data, bool stderrChannel, bool flush)
{
    if (!m_runActive || m_cancelRequested) return;
    for (const auto& update : (stderrChannel ? m_stderrProgress : m_stdoutProgress).feed(data, flush)) {
        emit statusChanged(update.stage);
        emit progressChanged(update.percent);
    }
}
void AISegmentationService::stopProcessTree(QProcess* process)
{
#ifdef Q_OS_WIN
    // nnU-Net is a child of the Python launcher. Stop only this owned PID tree.
    if (process->processId() > 0) {
        auto* stopper = new QProcess(this);
        connect(stopper, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), process,
                [stopper, process](int code, QProcess::ExitStatus) {
            if (code != 0 && process->state() != QProcess::NotRunning) process->kill();
            stopper->deleteLater();
        });
        connect(stopper, &QProcess::errorOccurred, process, [stopper, process](QProcess::ProcessError) {
            process->kill();
            stopper->deleteLater();
        });
        stopper->start(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/taskkill.exe"),
                       {"/PID", QString::number(process->processId()), "/T", "/F"});
        return;
    }
#endif
    process->kill();
}
