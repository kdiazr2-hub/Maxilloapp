#include "StandaloneDentalSegmentatorService.h"

#include <algorithm>
#include <array>
#include <deque>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QtConcurrent>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include "NrrdVolumeExporter.h"

#ifndef APP_SOURCE_DIR
#define APP_SOURCE_DIR ""
#endif

static unsigned char labelForTarget(SegmentationTarget target)
{
    switch (target) {
        case SegmentationTarget::Bone:       return 1;
        case SegmentationTarget::SoftTissue: return 2;
        case SegmentationTarget::Teeth:      return 3;
        case SegmentationTarget::Airway:     return 4;
    }
    return 1;
}

static QString targetName(SegmentationTarget target)
{
    switch (target) {
        case SegmentationTarget::Bone:       return "dental";
        case SegmentationTarget::SoftTissue: return "tejido blando";
        case SegmentationTarget::Teeth:      return "dientes";
        case SegmentationTarget::Airway:     return "via aerea";
        case SegmentationTarget::FullAuto:   return "completo";
    }
    return "hueso";
}

static bool matchesTarget(double hu, SegmentationTarget target)
{
    switch (target) {
        case SegmentationTarget::Bone:
            return hu >= 250.0;
        case SegmentationTarget::SoftTissue:
            return hu >= -150.0 && hu <= 250.0;
        case SegmentationTarget::Teeth:
            return hu >= 1200.0;
        case SegmentationTarget::Airway:
            return hu <= -450.0;
    }
    return false;
}

static bool canUseLocalFallback(SegmentationTarget target)
{
    return target == SegmentationTarget::SoftTissue;
}

static bool createLocalLabelmap(vtkImageData* image,
                                const QString& outputPath,
                                SegmentationTarget target,
                                QString* errorMessage)
{
    if (!image || image->GetNumberOfPoints() == 0) {
        if (errorMessage) *errorMessage = "No hay volumen cargado para segmentar.";
        return false;
    }

    int extent[6] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    double origin[3] = {0.0, 0.0, 0.0};
    image->GetExtent(extent);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);

    auto labelmap = vtkSmartPointer<vtkImageData>::New();
    labelmap->SetExtent(extent);
    labelmap->SetSpacing(spacing);
    labelmap->SetOrigin(origin);
    labelmap->AllocateScalars(VTK_UNSIGNED_CHAR, 1);

    const unsigned char label = labelForTarget(target);
    const int nx = extent[1] - extent[0] + 1;
    const int ny = extent[3] - extent[2] + 1;
    const int nz = extent[5] - extent[4] + 1;
    const auto linearIndex = [=](int x, int y, int z) -> size_t {
        return static_cast<size_t>(x - extent[0]) +
               static_cast<size_t>(nx) *
                   (static_cast<size_t>(y - extent[2]) +
                    static_cast<size_t>(ny) * static_cast<size_t>(z - extent[4]));
    };

    if (target == SegmentationTarget::Airway) {
        std::vector<unsigned char> airState(
            static_cast<size_t>(nx) * static_cast<size_t>(ny) * static_cast<size_t>(nz),
            0);
        std::deque<std::array<int, 3>> queue;

        auto enqueueExternalAir = [&](int x, int y, int z) {
            const size_t idx = linearIndex(x, y, z);
            if (airState[idx] != 1) return;
            airState[idx] = 2;
            queue.push_back({x, y, z});
        };

        for (int z = extent[4]; z <= extent[5]; ++z) {
            for (int y = extent[2]; y <= extent[3]; ++y) {
                for (int x = extent[0]; x <= extent[1]; ++x) {
                    const double hu = image->GetScalarComponentAsDouble(x, y, z, 0);
                    airState[linearIndex(x, y, z)] = matchesTarget(hu, target) ? 1 : 0;
                }
            }
        }

        for (int z = extent[4]; z <= extent[5]; ++z) {
            for (int y = extent[2]; y <= extent[3]; ++y) {
                enqueueExternalAir(extent[0], y, z);
                enqueueExternalAir(extent[1], y, z);
            }
            for (int x = extent[0]; x <= extent[1]; ++x) {
                enqueueExternalAir(x, extent[2], z);
                enqueueExternalAir(x, extent[3], z);
            }
        }
        for (int y = extent[2]; y <= extent[3]; ++y) {
            for (int x = extent[0]; x <= extent[1]; ++x) {
                enqueueExternalAir(x, y, extent[4]);
                enqueueExternalAir(x, y, extent[5]);
            }
        }

        constexpr int dirs[6][3] = {
            { 1, 0, 0}, {-1, 0, 0}, {0,  1, 0},
            { 0,-1, 0}, { 0, 0, 1}, {0,  0,-1}
        };
        while (!queue.empty()) {
            const auto p = queue.front();
            queue.pop_front();
            for (const auto& d : dirs) {
                const int x = p[0] + d[0];
                const int y = p[1] + d[1];
                const int z = p[2] + d[2];
                if (x < extent[0] || x > extent[1] ||
                    y < extent[2] || y > extent[3] ||
                    z < extent[4] || z > extent[5]) {
                    continue;
                }
                enqueueExternalAir(x, y, z);
            }
        }

        for (int z = extent[4]; z <= extent[5]; ++z) {
            for (int y = extent[2]; y <= extent[3]; ++y) {
                for (int x = extent[0]; x <= extent[1]; ++x) {
                    auto* out = static_cast<unsigned char*>(
                        labelmap->GetScalarPointer(x, y, z));
                    if (!out) {
                        if (errorMessage) *errorMessage = "Error creando labelmap local.";
                        return false;
                    }
                    out[0] = airState[linearIndex(x, y, z)] == 1 ? label : 0;
                }
            }
        }

        return NrrdVolumeExporter::exportToFile(labelmap, outputPath, errorMessage);
    }

    for (int z = extent[4]; z <= extent[5]; ++z) {
        for (int y = extent[2]; y <= extent[3]; ++y) {
            for (int x = extent[0]; x <= extent[1]; ++x) {
                const double hu = image->GetScalarComponentAsDouble(x, y, z, 0);
                auto* out = static_cast<unsigned char*>(
                    labelmap->GetScalarPointer(x, y, z));
                if (!out) {
                    if (errorMessage) *errorMessage = "Error creando labelmap local.";
                    return false;
                }
                out[0] = matchesTarget(hu, target) ? label : 0;
            }
        }
    }

    return NrrdVolumeExporter::exportToFile(labelmap, outputPath, errorMessage);
}

static bool commandUsesPlaceholders(const QStringList& args)
{
    return std::any_of(args.begin(), args.end(), [](const QString& arg) {
        return arg.contains("{input}") || arg.contains("{output}");
    });
}

static QStringList replacePlaceholders(QStringList args,
                                       const QString& inputPath,
                                       const QString& outputPath)
{
    for (QString& arg : args) {
        arg.replace("{input}", inputPath);
        arg.replace("{output}", outputPath);
    }
    return args;
}

static QString stripOuterQuotes(QString value)
{
    value = value.trimmed();
    while (value.size() >= 2 &&
           ((value.startsWith('"') && value.endsWith('"')) ||
            (value.startsWith('\'') && value.endsWith('\'')))) {
        value = value.mid(1, value.size() - 2).trimmed();
    }
    return value;
}

static QString resolveExecutable(QString program)
{
    program = stripOuterQuotes(program);
    if (program.isEmpty()) return {};

    const QFileInfo info(program);
    if (info.isAbsolute()) {
        return info.exists() && info.isFile() ? info.absoluteFilePath() : QString();
    }

    const QString found = QStandardPaths::findExecutable(program);
    return found.isEmpty() ? QString() : found;
}

static QStringList pythonCandidatePaths()
{
    QStringList candidates;
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString userProfile = env.value("USERPROFILE").trimmed();
    const QString localAppData = env.value("LOCALAPPDATA").trimmed();
    const QString programFiles = env.value("ProgramFiles").trimmed();
    const QString programFilesX86 = env.value("ProgramFiles(x86)").trimmed();

    auto add = [&](const QString& path) {
        if (!path.isEmpty() && !candidates.contains(path)) candidates << path;
    };

    if (!userProfile.isEmpty()) {
        add(QDir(userProfile).filePath("anaconda3/envs/dentalgpu/python.exe"));
        add(QDir(userProfile).filePath("miniconda3/envs/dentalgpu/python.exe"));
        add(QDir(userProfile).filePath("mambaforge/envs/dentalgpu/python.exe"));
        add(QDir(userProfile).filePath("miniforge3/envs/dentalgpu/python.exe"));
    }

    add(env.value("DENTALSEGMENTATOR_PYTHON").trimmed());
    if (!userProfile.isEmpty()) {
        add(QDir(userProfile).filePath("anaconda3/python.exe"));
        add(QDir(userProfile).filePath("miniconda3/python.exe"));
        add(QDir(userProfile).filePath("mambaforge/python.exe"));
        add(QDir(userProfile).filePath("miniforge3/python.exe"));
    }
    if (!localAppData.isEmpty()) {
        const QDir pyRoot(QDir(localAppData).filePath("Programs/Python"));
        for (const QString& dir : pyRoot.entryList(QStringList() << "Python*", QDir::Dirs | QDir::NoDotAndDotDot)) {
            add(QDir(pyRoot.filePath(dir)).filePath("python.exe"));
        }
    }
    for (const QString& root : {programFiles, programFilesX86}) {
        if (root.isEmpty()) continue;
        const QDir base(root);
        for (const QString& dir : base.entryList(QStringList() << "Python*", QDir::Dirs | QDir::NoDotAndDotDot)) {
            add(QDir(base.filePath(dir)).filePath("python.exe"));
        }
    }

    for (const char* name : {"python.exe", "python", "python3.exe", "python3", "py.exe", "py"}) {
        const QString found = QStandardPaths::findExecutable(QString::fromLatin1(name));
        add(found);
    }
    return candidates;
}

static QString findPythonExecutable()
{
    for (const QString& candidate : pythonCandidatePaths()) {
        const QString resolved = resolveExecutable(candidate);
        if (!resolved.isEmpty()) return resolved;
    }
    return {};
}

StandaloneDentalSegmentatorService::StandaloneDentalSegmentatorService(QObject* parent)
    : AISegmentationService(parent)
{
    m_process = new QProcess(this);
    m_workerWatcher = new QFutureWatcher<StandaloneSegmentationResult>(this);

    connect(m_workerWatcher, &QFutureWatcher<StandaloneSegmentationResult>::finished,
            this, &StandaloneDentalSegmentatorService::onWorkerFinished);
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &StandaloneDentalSegmentatorService::onReadyReadStdout);
    connect(m_process, &QProcess::readyReadStandardError,
            this, &StandaloneDentalSegmentatorService::onReadyReadStderr);
    connect(m_process, &QProcess::started, this, [this] {
        emit progressChanged(35);
        emit statusChanged("DentalSegmentator standalone iniciado.");
    });
    connect(m_process, qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this, &StandaloneDentalSegmentatorService::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred,
            this, &StandaloneDentalSegmentatorService::onProcessError);
}

StandaloneDentalSegmentatorService::~StandaloneDentalSegmentatorService()
{
    cancel();
}

bool StandaloneDentalSegmentatorService::isRunning() const
{
    return (m_process && m_process->state() != QProcess::NotRunning) ||
           (m_workerWatcher && m_workerWatcher->isRunning());
}

void StandaloneDentalSegmentatorService::segment(
    vtkSmartPointer<vtkImageData> inputVolume,
    const QString& outputDirectory,
    SegmentationTarget target)
{
    if (!inputVolume || inputVolume->GetNumberOfPoints() == 0) {
        emit errorOccurred("No hay volumen cargado para segmentar.");
        return;
    }
    if (isRunning()) {
        emit errorOccurred("Ya hay una segmentacion en progreso.");
        return;
    }

    m_outputDirectory = outputDirectory;
    QDir().mkpath(m_outputDirectory);

    const QString inputPath = QDir(m_outputDirectory).filePath("input.nrrd");
    const QString outputPath = QDir(m_outputDirectory).filePath("output_segmentation.nrrd");
    m_outputSegmentationPath = outputPath;
    m_externalCommand = resolveExternalCommand(inputPath, outputPath, target);

    if (m_externalCommand.isValid()) {
        emit statusChanged(QString("Exportando TAC para DentalSegmentator %1...")
                               .arg(targetName(target)));
        emit progressChanged(5);
    } else if (canUseLocalFallback(target)) {
        emit statusChanged(QString("Segmentando %1 localmente por HU, sin Slicer.exe.")
                               .arg(targetName(target)));
        emit progressChanged(10);
    } else {
        emit errorOccurred(
            "DentalSegmentator standalone no esta configurado. Instale Python con "
            "SimpleITK, numpy, torch y nnunetv2, o defina DENTALSEGMENTATOR_PYTHON. "
            "Este flujo no usa Slicer.exe.");
        return;
    }

    auto future = QtConcurrent::run([inputVolume, inputPath, outputPath,
                                     useExternal = m_externalCommand.isValid(),
                                     target] {
        StandaloneSegmentationResult result;
        result.inputPath = inputPath;
        result.outputPath = outputPath;
        result.needsExternalProcess = useExternal;
        result.target = target;

        QString error;
        if (useExternal) {
            if (!NrrdVolumeExporter::exportToFile(inputVolume, inputPath, &error)) {
                result.error = error;
                return result;
            }
        } else if (canUseLocalFallback(target)) {
            if (!createLocalLabelmap(inputVolume, outputPath, target, &error)) {
                result.error = error;
                return result;
            }
        } else {
            result.error = "No hay backend DentalSegmentator standalone configurado.";
            return result;
        }

        if (!error.isEmpty()) {
            result.error = error;
            return result;
        }

        result.ok = true;
        return result;
    });
    m_workerWatcher->setFuture(future);
}

void StandaloneDentalSegmentatorService::cancel()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
}

void StandaloneDentalSegmentatorService::onWorkerFinished()
{
    const StandaloneSegmentationResult result = m_workerWatcher->result();
    if (!result.ok) {
        emit errorOccurred("Error preparando segmentacion: " + result.error);
        return;
    }

    if (result.needsExternalProcess) {
        emit progressChanged(25);
        startExternalProcess(result.inputPath, result.outputPath);
        return;
    }

    emit progressChanged(100);
    emit statusChanged(QString("Segmentacion local finalizada: %1.")
                           .arg(targetName(result.target)));
    emit segmentationFinished(result.outputPath);
}

void StandaloneDentalSegmentatorService::startExternalProcess(const QString&,
                                                              const QString&)
{
    emit statusChanged("Ejecutando DentalSegmentator standalone...");
    m_processLog.clear();
    const QString program = resolveExecutable(m_externalCommand.program);
    if (program.isEmpty()) {
        emit errorOccurred(
            "No se encontro el ejecutable para DentalSegmentator standalone:\n" +
            m_externalCommand.program +
            "\n\nConfigure DENTALSEGMENTATOR_PYTHON con la ruta completa de python.exe.");
        return;
    }
    m_process->setProgram(program);
    m_process->setArguments(m_externalCommand.args);
    m_process->setWorkingDirectory(m_outputDirectory);
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("KMP_DUPLICATE_LIB_OK", "TRUE");
    m_process->setProcessEnvironment(env);
    m_process->start();
}

void StandaloneDentalSegmentatorService::onReadyReadStdout()
{
    appendProcessText(m_process->readAllStandardOutput());
}

void StandaloneDentalSegmentatorService::onReadyReadStderr()
{
    appendProcessText(m_process->readAllStandardError());
}

void StandaloneDentalSegmentatorService::appendProcessText(const QByteArray& data)
{
    const QString text = QString::fromLocal8Bit(data).trimmed();
    if (text.isEmpty()) return;

    m_processLog += text + "\n";
    emit statusChanged(text);
    if (text.contains('%')) {
        const QRegularExpression re("(\\d{1,3})\\s*%");
        const auto match = re.match(text);
        if (match.hasMatch()) {
            const int p = std::clamp(match.captured(1).toInt(), 35, 95);
            emit progressChanged(p);
        }
    }
}

void StandaloneDentalSegmentatorService::onProcessFinished(
    int exitCode,
    QProcess::ExitStatus status)
{
    if (status != QProcess::NormalExit || exitCode != 0) {
        emit errorOccurred(
            QString("DentalSegmentator standalone fallo. Codigo: %1\n\n%2")
                .arg(exitCode)
                .arg(m_processLog.right(6000)));
        return;
    }

    if (!QFileInfo::exists(m_outputSegmentationPath)) {
        emit errorOccurred("DentalSegmentator termino, pero no genero output_segmentation.nrrd.");
        return;
    }

    emit progressChanged(100);
    emit statusChanged("Segmentacion DentalSegmentator finalizada.");
    emit segmentationFinished(m_outputSegmentationPath);
}

void StandaloneDentalSegmentatorService::onProcessError(QProcess::ProcessError)
{
    if (m_process->state() == QProcess::NotRunning) {
        emit errorOccurred(
            "Error en DentalSegmentator standalone: " + m_process->errorString() +
            "\n\nEjecutable: " + m_process->program() +
            "\n\nLog:\n" + m_processLog.right(4000) +
            "\n\nSi usa Anaconda, configure por ejemplo:\n"
            "setx DENTALSEGMENTATOR_PYTHON \"C:\\Users\\Usuario\\anaconda3\\envs\\dentalgpu\\python.exe\"");
    }
}

StandaloneDentalSegmentatorService::ExternalCommand
StandaloneDentalSegmentatorService::resolveExternalCommand(
    const QString& inputPath,
    const QString& outputPath,
    SegmentationTarget target) const
{
    if (canUseLocalFallback(target)) {
        return {};
    }

    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    const QString cliCommand = env.value("DENTALSEGMENTATOR_CMD").trimmed();
    if (!cliCommand.isEmpty()) {
        QStringList parts = QProcess::splitCommand(cliCommand);
        if (!parts.isEmpty()) {
            const QString program = resolveExecutable(parts.takeFirst());
            if (program.isEmpty()) return {};
            ExternalCommand command;
            command.program = program;
            command.args = replacePlaceholders(parts, inputPath, outputPath);
            if (!commandUsesPlaceholders(parts)) {
                command.args << inputPath << outputPath;
            }
            command.args << "--target" << targetName(target);
            return command;
        }
    }

    const QString scriptPath = resolveScriptPath();
    if (!scriptPath.isEmpty() && QFileInfo::exists(scriptPath)) {
        const QString python = findPythonExecutable();
        if (!python.isEmpty()) {
            ExternalCommand command;
            command.program = python;
            command.args << scriptPath << inputPath << outputPath
                         << "--target" << targetName(target);
            if (target == SegmentationTarget::Airway) {
                if (hasAirwaySeeds()) {
                    const auto s1 = airwaySeed1();
                    const auto s2 = airwaySeed2();
                    const QString val1 = QStringLiteral("%1,%2,%3")
                                             .arg(QString::number(s1[0], 'f', 4))
                                             .arg(QString::number(s1[1], 'f', 4))
                                             .arg(QString::number(s1[2], 'f', 4));
                    const QString val2 = QStringLiteral("%1,%2,%3")
                                             .arg(QString::number(s2[0], 'f', 4))
                                             .arg(QString::number(s2[1], 'f', 4))
                                             .arg(QString::number(s2[2], 'f', 4));
                    command.args << QStringLiteral("--seed=%1").arg(val1)
                                 << QStringLiteral("--seed2=%1").arg(val2);
                } else if (hasAirwaySeed()) {
                    const auto seed = airwaySeed();
                    const QString val = QStringLiteral("%1,%2,%3")
                                            .arg(QString::number(seed[0], 'f', 4))
                                            .arg(QString::number(seed[1], 'f', 4))
                                            .arg(QString::number(seed[2], 'f', 4));
                    command.args << QStringLiteral("--seed=%1").arg(val);
                }
            }
            return command;
        }
    }

    return {};
}

QString StandaloneDentalSegmentatorService::resolveScriptPath() const
{
    const QString appScript = QDir(QCoreApplication::applicationDirPath())
                                  .filePath("scripts/run_standalone_dental_segmentator.py");
    if (QFileInfo::exists(appScript)) return appScript;

    const QString sourceScript = QDir(QStringLiteral(APP_SOURCE_DIR))
                                     .filePath("scripts/run_standalone_dental_segmentator.py");
    if (QFileInfo::exists(sourceScript)) return sourceScript;

    const QString cwdScript = QDir(QDir::currentPath())
                                  .filePath("scripts/run_standalone_dental_segmentator.py");
    if (QFileInfo::exists(cwdScript)) return cwdScript;

    Q_UNUSED(QStandardPaths::findExecutable("python"));
    return QString();
}
