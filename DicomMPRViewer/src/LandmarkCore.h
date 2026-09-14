#pragma once

#include <QString>
#include <QStringList>
#include <QVector3D>

enum class LandmarkAnatomicalType
{
    Unknown,
    Nasion,
    Sella,
    ANS,
    PNS,
    APoint,
    BPoint,
    Menton,
    Pogonion,
    GonionRight,
    GonionLeft,
    CondylionRight,
    CondylionLeft,
    OrbitaleRight,
    OrbitaleLeft,
    PorionRight,
    PorionLeft,
    UpperDentalMidline,
    LowerDentalMidline
};

struct Landmark
{
    QString id;
    QString name;
    QVector3D positionWorld;
    LandmarkAnatomicalType anatomicalType = LandmarkAnatomicalType::Unknown;
    QString associatedObjectId;
};

struct Plane
{
    QString id;
    QString name;
    QVector3D pointWorld;
    QVector3D normalWorld;
    QStringList createdFromLandmarks;
};

namespace LandmarkCore
{
double distance3D(const Landmark& a, const Landmark& b);
double distance3D(const QVector3D& a, const QVector3D& b);
double pointToPlaneDistance(const QVector3D& pointWorld, const Plane& plane);
double pointToPlaneDistance(const Landmark& landmark, const Plane& plane);
bool makePlaneFromThreePoints(const QString& id,
                              const QString& name,
                              const Landmark& a,
                              const Landmark& b,
                              const Landmark& c,
                              Plane* outPlane);
QString anatomicalTypeName(LandmarkAnatomicalType type);
}
