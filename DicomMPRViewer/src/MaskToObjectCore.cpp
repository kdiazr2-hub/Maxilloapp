#include "MaskToObjectCore.h"
#include "MeshGenerator.h"
#include <vtkImageData.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>

vtkSmartPointer<vtkPolyData> MaskToObjectCore::Convert(vtkImageData* mask, int label, QString* error)
{
    if (error) error->clear();
    if (!mask || !mask->GetPointData()->GetScalars() || mask->GetNumberOfScalarComponents() != 1) {
        if (error) *error = QStringLiteral("Se necesita una máscara de etiquetas válida.");
        return nullptr;
    }
    return MeshGenerator::generateMesh(mask, label, false, 0, error);
}
