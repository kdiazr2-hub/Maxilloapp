#pragma once

#include <QString>

#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

namespace CompositeModelCore
{
vtkSmartPointer<vtkPolyData> CreateNonDestructiveComposite(
    vtkPolyData* boneMesh,
    vtkMatrix4x4* boneToWorld,
    vtkPolyData* dentalMesh,
    vtkMatrix4x4* dentalToWorld,
    const QString& anatomyName,
    QString* report = nullptr,
    QString* error = nullptr);
}
