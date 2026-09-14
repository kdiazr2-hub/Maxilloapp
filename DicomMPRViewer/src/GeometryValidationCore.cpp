#include "GeometryValidationCore.h"

#include "GeometryValidation.h"

#include <vtkPolyData.h>

namespace GeometryValidationCore
{
QString anatomicalObjectTypeName(AnatomicalObjectType type)
{
    switch (type) {
    case AnatomicalObjectType::Skull: return QStringLiteral("Cranio");
    case AnatomicalObjectType::Maxilla: return QStringLiteral("Maxilar");
    case AnatomicalObjectType::Mandible: return QStringLiteral("Mandibula");
    case AnatomicalObjectType::UpperDentalArch: return QStringLiteral("Arco dental superior STL");
    case AnatomicalObjectType::LowerDentalArch: return QStringLiteral("Arco dental inferior STL");
    case AnatomicalObjectType::CompositeMaxilla: return QStringLiteral("Compuesto maxilar");
    case AnatomicalObjectType::CompositeMandible: return QStringLiteral("Compuesto mandibular");
    case AnatomicalObjectType::Unknown: break;
    }
    return QStringLiteral("Desconocido");
}

ValidationResult ValidateDicomGeometry(vtkImageData* image)
{
    return GeometryValidation::validateDicom(image);
}

ValidationResult ValidateMeshScale(vtkPolyData* mesh, AnatomicalObjectType type)
{
    return GeometryValidation::validateMeshScale(mesh, anatomicalObjectTypeName(type));
}
}
