#include "MeasurementSerialization.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

static QJsonArray pointToJson(const std::array<double, 3>& p)
{
    QJsonArray a;
    a.append(p[0]);
    a.append(p[1]);
    a.append(p[2]);
    return a;
}

static std::array<double, 3> pointFromJson(const QJsonArray& a)
{
    return {
        a.size() > 0 ? a[0].toDouble() : 0.0,
        a.size() > 1 ? a[1].toDouble() : 0.0,
        a.size() > 2 ? a[2].toDouble() : 0.0
    };
}

bool MeasurementSerialization::save(const QString& filePath,
                                    const QString& studyInstanceUid,
                                    const QString& seriesInstanceUid,
                                    const std::vector<Measurement>& measurements,
                                    QString* errorMessage)
{
    QJsonObject root;
    root["format"] = "DicomMPRViewer.Measurements";
    root["version"] = 2;
    root["studyInstanceUid"] = studyInstanceUid;
    root["seriesInstanceUid"] = seriesInstanceUid;

    QJsonArray list;
    for (const Measurement& m : measurements) {
        QJsonObject item;
        item["id"] = QString::fromStdString(m.id);
        item["type"] = measurementTypeToString(m.type);
        item["view"] = measurementViewToString(m.viewOrientation);
        item["roiShape"] = measurementROIShapeToString(m.roiShape);
        item["sliceIndex"] = m.sliceIndex;
        item["value"] = m.value;
        item["areaMm2"] = m.areaMm2;
        item["huMean"] = m.huMean;
        item["huMin"] = m.huMin;
        item["huMax"] = m.huMax;
        item["huStdDev"] = m.huStdDev;
        item["sampleCount"] = m.sampleCount;
        item["text"] = QString::fromStdString(m.text);
        item["visible"] = m.visible;
        item["createdAt"] = QString::fromStdString(m.createdAt);

        QJsonArray points;
        for (const auto& p : m.pointsPhysical) {
            points.append(pointToJson(p));
        }
        item["pointsPhysical"] = points;
        list.append(item);
    }
    root["measurements"] = list;

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

bool MeasurementSerialization::load(const QString& filePath,
                                    MeasurementDocument& document,
                                    QString* errorMessage)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    QJsonParseError parseError;
    const QJsonDocument json = QJsonDocument::fromJson(file.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !json.isObject()) {
        if (errorMessage) *errorMessage = parseError.errorString();
        return false;
    }

    const QJsonObject root = json.object();
    document.studyInstanceUid = root["studyInstanceUid"].toString();
    document.seriesInstanceUid = root["seriesInstanceUid"].toString();
    document.measurements.clear();

    const QJsonArray list = root["measurements"].toArray();
    document.measurements.reserve(static_cast<size_t>(list.size()));
    for (const QJsonValue& value : list) {
        const QJsonObject item = value.toObject();
        Measurement m;
        m.id = item["id"].toString().toStdString();
        m.type = measurementTypeFromString(item["type"].toString().toStdString());
        m.viewOrientation = measurementViewFromString(item["view"].toString().toStdString());
        m.roiShape = measurementROIShapeFromString(item["roiShape"].toString().toStdString());
        m.sliceIndex = item["sliceIndex"].toInt();
        m.value = item["value"].toDouble();
        m.areaMm2 = item["areaMm2"].toDouble(m.value);
        m.huMean = item["huMean"].toDouble();
        m.huMin = item["huMin"].toDouble();
        m.huMax = item["huMax"].toDouble();
        m.huStdDev = item["huStdDev"].toDouble();
        m.sampleCount = item["sampleCount"].toInt();
        m.text = item["text"].toString().toStdString();
        m.visible = item["visible"].toBool(true);
        m.createdAt = item["createdAt"].toString().toStdString();

        const QJsonArray points = item["pointsPhysical"].toArray();
        for (const QJsonValue& p : points) {
            m.pointsPhysical.push_back(pointFromJson(p.toArray()));
        }
        document.measurements.push_back(std::move(m));
    }

    return true;
}
