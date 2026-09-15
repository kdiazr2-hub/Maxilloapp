#pragma once
#include <QByteArray>
#include <QString>
#include <vector>

struct SegmentationProgressUpdate {
    int percent = -1;
    QString stage;
};

// Newline-framed UTF-8 progress; process reads may split a line or a character.
class SegmentationProgressCore {
public:
    std::vector<SegmentationProgressUpdate> feed(const QByteArray& bytes, bool flush = false);
    void reset() { m_pending.clear(); }
    static QString elapsedText(qint64 milliseconds);
private:
    QByteArray m_pending;
};
