#pragma once

#include <QObject>
#include <QString>
#include <vtkSmartPointer.h>

class vtkImageData;

enum class SegmentationTarget
{
    Bone,
    SoftTissue,
    Teeth,
    Airway,
    FullAuto
};

class AISegmentationService : public QObject
{
    Q_OBJECT

public:
    explicit AISegmentationService(QObject* parent = nullptr);
    ~AISegmentationService() override;

    virtual void segment(vtkSmartPointer<vtkImageData> inputVolume,
                         const QString& outputDirectory,
                         SegmentationTarget target) = 0;
    virtual void cancel() = 0;

    void setAirwaySeed(double x, double y, double z) {
        m_airwaySeed[0] = x;
        m_airwaySeed[1] = y;
        m_airwaySeed[2] = z;
        m_hasAirwaySeed = true;
    }
    void setAirwaySeeds(const std::array<double, 3>& p1, const std::array<double, 3>& p2) {
        m_airwaySeed1 = p1;
        m_airwaySeed2 = p2;
        m_hasAirwaySeeds = true;
    }
    void clearAirwaySeed() {
        m_hasAirwaySeed = false;
        m_hasAirwaySeeds = false;
    }
    bool hasAirwaySeed() const {
        return m_hasAirwaySeed;
    }
    std::array<double, 3> airwaySeed() const {
        return m_airwaySeed;
    }
    bool hasAirwaySeeds() const {
        return m_hasAirwaySeeds;
    }
    std::array<double, 3> airwaySeed1() const {
        return m_airwaySeed1;
    }
    std::array<double, 3> airwaySeed2() const {
        return m_airwaySeed2;
    }

signals:
    void progressChanged(int progress);
    void statusChanged(const QString& status);
    void segmentationFinished(const QString& outputSegmentationPath);
    void errorOccurred(const QString& error);

protected:
    std::array<double, 3> m_airwaySeed = {0.0, 0.0, 0.0};
    bool m_hasAirwaySeed = false;
    std::array<double, 3> m_airwaySeed1 = {0.0, 0.0, 0.0};
    std::array<double, 3> m_airwaySeed2 = {0.0, 0.0, 0.0};
    bool m_hasAirwaySeeds = false;
};
