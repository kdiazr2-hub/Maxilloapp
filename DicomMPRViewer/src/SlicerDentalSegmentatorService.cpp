#include "SlicerDentalSegmentatorService.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QtConcurrent>

#include <vtkImageData.h>

#include "NrrdVolumeExporter.h"

#ifndef APP_SOURCE_DIR
#define APP_SOURCE_DIR ""
#endif

SlicerDentalSegmentatorService::SlicerDentalSegmentatorService(QObject* parent)
    : AISegmentationService(parent)
{
    m_process = new QProcess(this);
    m_exportWatcher = new QFutureWatcher<SegmentationExportResult>(this);

    connect(m_exportWatcher, &QFutureWatcher<SegmentationExportResult>::finished,
            this, &SlicerDentalSegmentatorService::onExportFinished);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &SlicerDentalSegmentatorService::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &SlicerDentalSegmentatorService::onReadyReadStderr);
    connect(m_process, &QProcess::started, this, [this] {
        emit progressChanged(30);
        emit statusChanged("Slicer iniciado en modo headless.");
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &SlicerDentalSegmentatorService::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &SlicerDentalSegmentatorService::onProcessError);
}

SlicerDentalSegmentatorService::~SlicerDentalSegmentatorService()
{
    cancel();
}

bool SlicerDentalSegmentatorService::isRunning() const
{
    return m_process && m_process->state() != QProcess::NotRunning;
}

void SlicerDentalSegmentatorService::segment(vtkSmartPointer<vtkImageData> inputVolume,
                                             const QString& outputDirectory,
                                             SegmentationTarget)
{
    if (!inputVolume || inputVolume->GetNumberOfPoints() == 0) {
        emit errorOccurred("No hay volumen cargado para segmentar.");
        return;
    }
    if (isRunning() || m_exportWatcher->isRunning()) {
        emit errorOccurred("Ya hay una segmentacion en progreso.");
        return;
    }

    m_outputDirectory = outputDirectory;
    QDir().mkpath(m_outputDirectory);

    const QString slicerPath = resolveSlicerExecutable();
    if (slicerPath.isEmpty() || !QFileInfo::exists(slicerPath)) {
        emit errorOccurred("No se encontro Slicer.exe. El TAC cargado ya se usa automaticamente; solo falta configurar la ruta de 3D Slicer.");
        return;
    }
    m_slicerExecutablePath = slicerPath;

    const QString scriptPath = resolveScriptPath();
    if (scriptPath.isEmpty() || !QFileInfo::exists(scriptPath)) {
        emit errorOccurred("No se encontro scripts/run_dental_segmentator.py.");
        return;
    }

    const QString inputPath = QDir(m_outputDirectory).filePath("input.nrrd");
    const QString outputPath = QDir(m_outputDirectory).filePath("output_segmentation.nrrd");
    m_outputSegmentationPath = outputPath;

    emit statusChanged("Exportando TAC cargado a NRRD...");
    emit progressChanged(5);

    auto future = QtConcurrent::run([inputVolume, inputPath, outputPath, scriptPath]() {
        SegmentationExportResult result;
        result.inputPath = inputPath;
        result.outputPath = outputPath;
        result.scriptPath = scriptPath;

        QString error;
        if (!NrrdVolumeExporter::exportToFile(inputVolume, inputPath, &error)) {
            result.error = error;
            return result;
        }
        result.ok = true;
        return result;
    });
    m_exportWatcher->setFuture(future);
}

void SlicerDentalSegmentatorService::cancel()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void SlicerDentalSegmentatorService::onExportFinished()
{
    const SegmentationExportResult result = m_exportWatcher->result();
    if (!result.ok) {
        emit errorOccurred("Error exportando NRRD: " + result.error);
        return;
    }

    emit progressChanged(20);
    startSlicerProcess(result);
}

void SlicerDentalSegmentatorService::startSlicerProcess(
    const SegmentationExportResult& exportResult)
{
    emit statusChanged("Ejecutando DentalSegmentator en 3D Slicer...");

    QStringList args;
    args << "--no-main-window"
         << "--python-script"
         << exportResult.scriptPath
         << exportResult.inputPath
         << exportResult.outputPath;

    m_process->setProgram(m_slicerExecutablePath);
    m_process->setArguments(args);
    m_process->setWorkingDirectory(m_outputDirectory);
    m_process->start();
}

void SlicerDentalSegmentatorService::onReadyReadStdout()
{
    appendProcessText(m_process->readAllStandardOutput());
}

void SlicerDentalSegmentatorService::onReadyReadStderr()
{
    appendProcessText(m_process->readAllStandardError());
}

void SlicerDentalSegmentatorService::appendProcessText(const QByteArray& data)
{
    const QString text = QString::fromLocal8Bit(data).trimmed();
    if (text.isEmpty()) return;

    emit statusChanged(text);
    if (text.contains('%')) {
        const QRegularExpression re("(\\d{1,3})\\s*%");
        const auto match = re.match(text);
        if (match.hasMatch()) {
            const int p = std::clamp(match.captured(1).toInt(), 30, 95);
            emit progressChanged(p);
        }
    }
}

void SlicerDentalSegmentatorService::onProcessFinished(int exitCode,
                                                       QProcess::ExitStatus status)
{
    if (status != QProcess::NormalExit || exitCode != 0) {
        emit errorOccurred(QString("Slicer/DentalSegmentator fallo. Codigo: %1").arg(exitCode));
        return;
    }

    if (!QFileInfo::exists(m_outputSegmentationPath)) {
        emit errorOccurred("DentalSegmentator termino, pero no genero output_segmentation.nrrd.");
        return;
    }

    emit progressChanged(100);
    emit statusChanged("Segmentacion IA finalizada.");
    emit segmentationFinished(m_outputSegmentationPath);
}

void SlicerDentalSegmentatorService::onProcessError(QProcess::ProcessError)
{
    if (m_process->state() == QProcess::NotRunning) {
        emit errorOccurred("Error en proceso Slicer: " + m_process->errorString());
    }
}

QString SlicerDentalSegmentatorService::resolveSlicerExecutable() const
{
    if (!m_slicerExecutablePath.isEmpty() && QFileInfo::exists(m_slicerExecutablePath)) {
        return m_slicerExecutablePath;
    }

    const QString envPath = QProcessEnvironment::systemEnvironment().value("SLICER_EXE");
    if (!envPath.isEmpty() && QFileInfo::exists(envPath)) return envPath;

    const QStringList roots = {
        "C:/Program Files",
        "C:/Program Files (x86)",
        QDir::homePath() + "/AppData/Local"
    };
    for (const QString& root : roots) {
        QDir dir(root);
        const QStringList candidates = dir.entryList({"Slicer*"}, QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString& candidate : candidates) {
            const QString exe = dir.filePath(candidate + "/Slicer.exe");
            if (QFileInfo::exists(exe)) return exe;
        }
    }
    return QString();
}

QString SlicerDentalSegmentatorService::resolveScriptPath() const
{
    const QString appScript = QDir(QCoreApplication::applicationDirPath())
                                  .filePath("scripts/run_dental_segmentator.py");
    if (QFileInfo::exists(appScript)) return appScript;

    const QString sourceScript = QDir(QStringLiteral(APP_SOURCE_DIR))
                                     .filePath("scripts/run_dental_segmentator.py");
    if (QFileInfo::exists(sourceScript)) return sourceScript;

    const QString cwdScript = QDir(QDir::currentPath())
                                  .filePath("scripts/run_dental_segmentator.py");
    if (QFileInfo::exists(cwdScript)) return cwdScript;
    return QString();
}
