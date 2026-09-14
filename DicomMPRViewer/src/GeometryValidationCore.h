#pragma once

#include "ValidationResult.h"

#include <QString>

class vtkImageData;
class vtkPolyData;

enum class AnatomicalObjectType
{
    Skull,
    Maxilla,
    Mandible,
    UpperDentalArch,
    LowerDentalArch,
    CompositeMaxilla,
    CompositeMandible,
    Unknown
};

namespace GeometryValidationCore
{
QString anatomicalObjectTypeName(AnatomicalObjectType type);
ValidationResult ValidateDicomGeometry(vtkImageData* image);
ValidationResult ValidateMeshScale(vtkPolyData* mesh, AnatomicalObjectType type);
}
