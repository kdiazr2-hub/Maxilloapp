#include "MeasurementManager.h"

#include <algorithm>
#include <cstdlib>

MeasurementManager::MeasurementManager(QObject* parent)
    : QObject(parent)
{
}

QString MeasurementManager::addMeasurement(Measurement measurement)
{
    if (measurement.id.empty()) {
        measurement.id = nextId();
    }
    m_measurements.push_back(std::move(measurement));
    emit measurementsChanged();
    return QString::fromStdString(m_measurements.back().id);
}

bool MeasurementManager::removeMeasurement(const QString& id)
{
    const std::string key = id.toStdString();
    const auto oldSize = m_measurements.size();
    m_measurements.erase(
        std::remove_if(m_measurements.begin(), m_measurements.end(),
                       [&](const Measurement& m) { return m.id == key; }),
        m_measurements.end());

    if (m_measurements.size() == oldSize) return false;
    emit measurementsChanged();
    return true;
}

void MeasurementManager::replaceMeasurements(std::vector<Measurement> measurements)
{
    m_measurements = std::move(measurements);
    updateNextIdFromExisting();
    emit measurementsChanged();
}

void MeasurementManager::clear()
{
    if (m_measurements.empty()) return;
    m_measurements.clear();
    m_nextId = 1;
    emit measurementsChanged();
}

void MeasurementManager::setAllVisible(bool visible)
{
    m_allVisible = visible;
    emit measurementsChanged();
}

std::string MeasurementManager::nextId()
{
    return "M" + std::to_string(m_nextId++);
}

void MeasurementManager::updateNextIdFromExisting()
{
    int maxId = 0;
    for (const Measurement& m : m_measurements) {
        if (m.id.size() > 1 && (m.id[0] == 'M' || m.id[0] == 'm')) {
            maxId = std::max(maxId, std::atoi(m.id.c_str() + 1));
        }
    }
    m_nextId = maxId + 1;
}
