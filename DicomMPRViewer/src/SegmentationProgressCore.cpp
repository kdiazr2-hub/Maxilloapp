#include "SegmentationProgressCore.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>

std::vector<SegmentationProgressUpdate> SegmentationProgressCore::feed(const QByteArray& bytes, bool flush)
{
    m_pending += bytes;
    std::vector<SegmentationProgressUpdate> updates;
    if (flush && !m_pending.endsWith('\n')) m_pending += '\n';
    int end;
    while ((end = m_pending.indexOf('\n')) >= 0) {
        const QByteArray line = m_pending.left(end).trimmed();
        m_pending.remove(0, end + 1);
        if (line.startsWith("MAXILLO_PROGRESS ")) {
            const auto doc = QJsonDocument::fromJson(line.mid(17));
            const auto obj = doc.object();
            if (!doc.isObject() || !obj.value("stage").isString() || !obj.value("percent").isDouble()) continue;
            const int percent = obj.value("percent").toInt(-1);
            if (percent < -1 || percent > 100 || obj.value("stage").toString().isEmpty()) continue;
            updates.push_back({percent, obj.value("stage").toString().left(240)});
        } else {
            // Legacy Slicer/custom backends: accept explicit leading milestones, not tqdm bars.
            static const QRegularExpression milestone(QStringLiteral("^(\\d{1,3})%\\s+([^|].*)$"));
            const auto match = milestone.match(QString::fromUtf8(line));
            if (match.hasMatch() && match.captured(1).toInt() <= 100)
                updates.push_back({match.captured(1).toInt(), match.captured(2).left(240)});
        }
    }
    if (m_pending.size() > 65536) m_pending.clear();
    return updates;
}

QString SegmentationProgressCore::elapsedText(qint64 milliseconds)
{
    const qint64 seconds = std::max(qint64(0), milliseconds / 1000);
    return QStringLiteral("%1:%2:%3").arg(seconds / 3600, 2, 10, QLatin1Char('0'))
        .arg((seconds / 60) % 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
}
