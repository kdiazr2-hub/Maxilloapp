#include "LandmarkCore.h"

#include <cmath>

namespace
{
double length(const QVector3D& v)
{
    return std::sqrt(static_cast<double>(QVector3D::dotProduct(v, v)));
}
}

namespace LandmarkCore
{
double distance3D(const QVector3D& a, const QVector3D& b)
{
    return length(a - b);
}

double distance3D(const Landmark& a, const Landmark& b)
{
    return distance3D(a.positionWorld, b.positionWorld);
}

double pointToPlaneDistance(const QVector3D& pointWorld, const Plane& plane)
{
    const double nLen = length(plane.normalWorld);
    if (nLen <= 1e-12) {
        return 0.0;
    }

    const QVector3D delta = pointWorld - plane.pointWorld;
    const double signedDistance =
        static_cast<double>(QVector3D::dotProduct(delta, plane.normalWorld)) / nLen;
    return std::abs(signedDistance);
}

double pointToPlaneDistance(const Landmark& landmark, const Plane& plane)
{
    return pointToPlaneDistance(landmark.positionWorld, plane);
}

bool makePlaneFromThreePoints(const QString& id,
                              const QString& name,
                              const Landmark& a,
                              const Landmark& b,
                              const Landmark& c,
                              Plane* outPlane)
{
    if (!outPlane) {
        return false;
    }

    const QVector3D ab = b.positionWorld - a.positionWorld;
    const QVector3D ac = c.positionWorld - a.positionWorld;
    const QVector3D normal = QVector3D::crossProduct(ab, ac);
    if (length(normal) <= 1e-8) {
        return false;
    }

    outPlane->id = id;
    outPlane->name = name;
    outPlane->pointWorld = a.positionWorld;
    outPlane->normalWorld = normal.normalized();
    outPlane->createdFromLandmarks = {a.id, b.id, c.id};
    return true;
}

QString anatomicalTypeName(LandmarkAnatomicalType type)
{
    switch (type) {
    case LandmarkAnatomicalType::Nasion: return QStringLiteral("Nasion");
    case LandmarkAnatomicalType::Sella: return QStringLiteral("Sella");
    case LandmarkAnatomicalType::ANS: return QStringLiteral("ANS");
    case LandmarkAnatomicalType::PNS: return QStringLiteral("PNS");
    case LandmarkAnatomicalType::APoint: return QStringLiteral("A point");
    case LandmarkAnatomicalType::BPoint: return QStringLiteral("B point");
    case LandmarkAnatomicalType::Menton: return QStringLiteral("Menton");
    case LandmarkAnatomicalType::Pogonion: return QStringLiteral("Pogonion");
    case LandmarkAnatomicalType::GonionRight: return QStringLiteral("Gonion derecho");
    case LandmarkAnatomicalType::GonionLeft: return QStringLiteral("Gonion izquierdo");
    case LandmarkAnatomicalType::CondylionRight: return QStringLiteral("Condylion derecho");
    case LandmarkAnatomicalType::CondylionLeft: return QStringLiteral("Condylion izquierdo");
    case LandmarkAnatomicalType::OrbitaleRight: return QStringLiteral("Orbitale derecho");
    case LandmarkAnatomicalType::OrbitaleLeft: return QStringLiteral("Orbitale izquierdo");
    case LandmarkAnatomicalType::PorionRight: return QStringLiteral("Porion derecho");
    case LandmarkAnatomicalType::PorionLeft: return QStringLiteral("Porion izquierdo");
    case LandmarkAnatomicalType::UpperDentalMidline: return QStringLiteral("Linea media dental superior");
    case LandmarkAnatomicalType::LowerDentalMidline: return QStringLiteral("Linea media dental inferior");
    case LandmarkAnatomicalType::Unknown: break;
    }
    return QStringLiteral("Desconocido");
}
}
