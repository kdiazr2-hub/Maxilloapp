#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// LoggerCore  (Phase 4)
//
// Singleton clinical/technical logger.  Each call appends a timestamped entry
// to an in-memory list.  The accumulated log can be retrieved as a single
// QString or saved to a UTF-8 text file.
//
// Thread-safety: calls are expected on the Qt main thread only.
//
// Usage:
//   LoggerCore::instance().logDicomLoad(folder, slices, sx, sy, sz);
//   LoggerCore::instance().saveToFile("informe_clinico.txt");
// ─────────────────────────────────────────────────────────────────────────────

#include <QString>
#include <QStringList>

class LoggerCore
{
public:
    static LoggerCore& instance();

    // ── Clinical events ───────────────────────────────────────────────────────
    void logDicomLoad(const QString& folder, int slices,
                      double spacingX, double spacingY, double spacingZ);

    void logStlLoad(const QString& path, const QString& role);

    void logSegmentation(const QString& target,
                         const QString& outputPath,
                         bool success);

    void logRegistration(const QString& name,
                         double landmarkRms,
                         double meanDist,
                         double p95Dist,
                         bool accepted);

    void logCompositeCreation(const QString& name, int outputCells);

    void logExport(const QString& path, const QString& type);

    void logValidation(const QString& context,
                       const QString& message,
                       bool isWarning);

    void logCustom(const QString& category, const QString& message);

    // ── Output ────────────────────────────────────────────────────────────────
    // Returns all entries joined by newlines.
    QString fullLog() const;

    // Saves full log to a UTF-8 plain-text file.
    // Returns true on success; fills *error on failure.
    bool saveToFile(const QString& path, QString* error = nullptr) const;

    // Clears the in-memory log.
    void clear();

private:
    LoggerCore() = default;

    void append(const QString& category, const QString& msg);

    QStringList m_entries;
};
