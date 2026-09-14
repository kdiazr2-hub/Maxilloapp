#include "LoggerCore.h"

#include <QDateTime>
#include <QFile>
#include <QStringConverter>
#include <QTextStream>

// ─────────────────────────────────────────────────────────────────────────────
// Singleton
// ─────────────────────────────────────────────────────────────────────────────

LoggerCore& LoggerCore::instance()
{
    static LoggerCore inst;
    return inst;
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helper
// ─────────────────────────────────────────────────────────────────────────────

void LoggerCore::append(const QString& category, const QString& msg)
{
    const QString ts = QDateTime::currentDateTime()
                           .toString(QStringLiteral("yyyy-MM-ddTHH:mm:ss"));
    m_entries.append(
        QStringLiteral("[%1][%2] %3").arg(ts, category, msg));
}

// ─────────────────────────────────────────────────────────────────────────────
// Clinical event loggers
// ─────────────────────────────────────────────────────────────────────────────

void LoggerCore::logDicomLoad(const QString& folder, int slices,
                               double spacingX, double spacingY, double spacingZ)
{
    append(QStringLiteral("DICOM_LOAD"),
           QStringLiteral("folder=\"%1\" slices=%2 spacing=(%3,%4,%5)mm")
               .arg(folder)
               .arg(slices)
               .arg(spacingX, 0, 'f', 4)
               .arg(spacingY, 0, 'f', 4)
               .arg(spacingZ, 0, 'f', 4));
}

void LoggerCore::logStlLoad(const QString& path, const QString& role)
{
    append(QStringLiteral("STL_LOAD"),
           QStringLiteral("role=%1 path=\"%2\"").arg(role, path));
}

void LoggerCore::logSegmentation(const QString& target,
                                  const QString& outputPath,
                                  bool success)
{
    append(QStringLiteral("SEGMENTATION"),
           QStringLiteral("target=%1 success=%2 output=\"%3\"")
               .arg(target)
               .arg(success ? QStringLiteral("yes") : QStringLiteral("no"))
               .arg(outputPath));
}

void LoggerCore::logRegistration(const QString& name,
                                   double landmarkRms,
                                   double meanDist,
                                   double p95Dist,
                                   bool accepted)
{
    append(QStringLiteral("REGISTRATION"),
           QStringLiteral("arch=%1 landmark_rms=%2mm mean_dist=%3mm p95_dist=%4mm accepted=%5")
               .arg(name)
               .arg(landmarkRms, 0, 'f', 3)
               .arg(meanDist,    0, 'f', 3)
               .arg(p95Dist,     0, 'f', 3)
               .arg(accepted ? QStringLiteral("yes") : QStringLiteral("no")));
}

void LoggerCore::logCompositeCreation(const QString& name, int outputCells)
{
    append(QStringLiteral("COMPOSITE"),
           QStringLiteral("name=%1 cells=%2").arg(name).arg(outputCells));
}

void LoggerCore::logExport(const QString& path, const QString& type)
{
    append(QStringLiteral("EXPORT"),
           QStringLiteral("type=%1 path=\"%2\"").arg(type, path));
}

void LoggerCore::logValidation(const QString& context,
                                const QString& message,
                                bool isWarning)
{
    const QString severity = isWarning
        ? QStringLiteral("WARNING")
        : QStringLiteral("ERROR");
    append(QStringLiteral("VALIDATION"),
           QStringLiteral("[%1] context=%2 msg=\"%3\"")
               .arg(severity, context, message));
}

void LoggerCore::logCustom(const QString& category, const QString& message)
{
    append(category, message);
}

// ─────────────────────────────────────────────────────────────────────────────
// Output
// ─────────────────────────────────────────────────────────────────────────────

QString LoggerCore::fullLog() const
{
    return m_entries.join(QLatin1Char('\n'));
}

bool LoggerCore::saveToFile(const QString& path, QString* error) const
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        if (error)
            *error = QStringLiteral("No se pudo abrir para escritura: ") + path;
        return false;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out << QStringLiteral("=== MaxilloApp — Log Clínico ===\n");
    out << QStringLiteral("Generado: ")
        << QDateTime::currentDateTime().toString(Qt::ISODate)
        << QStringLiteral("\n\n");
    for (const QString& entry : m_entries)
        out << entry << QLatin1Char('\n');
    return true;
}

void LoggerCore::clear()
{
    m_entries.clear();
}
