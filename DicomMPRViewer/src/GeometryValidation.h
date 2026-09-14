#pragma once

#include "ValidationResult.h"

#include <QString>

#include <vtkImageData.h>
#include <vtkPolyData.h>

namespace GeometryValidation
{
QString MeshBoundsText(vtkPolyData* mesh);
QString MeshCentroidText(vtkPolyData* mesh);
double  MeshDiagonalLength(vtkPolyData* mesh);

bool ValidateDicomGeometry(vtkImageData* image, QString* report = nullptr);
bool ValidateMeshScale(vtkPolyData* mesh, const QString& anatomy, QString* report = nullptr);
bool ValidateCompositeInputs(vtkPolyData* boneWorld, vtkPolyData* archWorld,
                             const QString& anatomy, QString* report = nullptr);

ValidationResult validateDicom(vtkImageData* vol);
ValidationResult validateMeshScale(vtkPolyData* mesh, const QString& anatomyType);
}
