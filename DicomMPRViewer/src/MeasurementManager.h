#pragma once

#include <vector>

#include <QObject>
#include <QString>

#include "Measurement.h"

class MeasurementManager : public QObject
{
    Q_OBJECT

public:
    explicit MeasurementManager(QObject* parent = nullptr);

    const std::vector<Measurement>& measurements() const { return m_measurements; }
    bool allVisible() const { return m_allVisible; }

    QString addMeasurement(Measurement measurement);
    bool removeMeasurement(const QString& id);
    void replaceMeasurements(std::vector<Measurement> measurements);
    void clear();
    void setAllVisible(bool visible);

signals:
    void measurementsChanged();

private:
    std::string nextId();
    void updateNextIdFromExisting();

    std::vector<Measurement> m_measurements;
    int m_nextId = 1;
    bool m_allVisible = true;
};
