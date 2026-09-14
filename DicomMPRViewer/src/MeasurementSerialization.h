#pragma once

#include <vector>

#include <QString>

#include "Measurement.h"

struct MeasurementDocument
{
    QString studyInstanceUid;
    QString seriesInstanceUid;
    std::vector<Measurement> measurements;
};

class MeasurementSerialization
{
public:
    static bool save(const QString& filePath,
                     const QString& studyInstanceUid,
                     const QString& seriesInstanceUid,
                     const std::vector<Measurement>& measurements,
                     QString* errorMessage);
    static bool load(const QString& filePath,
                     MeasurementDocument& document,
                     QString* errorMessage);
};
