#pragma once

#include <QString>
#include <QVector>
#include <array>

#include <vtkMatrix4x4.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

namespace TransformCore
{
vtkSmartPointer<vtkMatrix4x4> IdentityMatrix();
vtkSmartPointer<vtkMatrix4x4> CloneMatrix(vtkMatrix4x4* matrix);
vtkSmartPointer<vtkMatrix4x4> InvertTransform(vtkMatrix4x4* matrix);
vtkSmartPointer<vtkMatrix4x4> ComposeTransforms(const QVector<vtkMatrix4x4*>& transforms);
vtkSmartPointer<vtkPolyData> ApplyTransformToPolyData(vtkPolyData* mesh, vtkMatrix4x4* matrix);

bool IsIdentity(vtkMatrix4x4* matrix, double tolerance = 1e-6);
bool ValidateRigidTransform(vtkMatrix4x4* matrix, QString* report = nullptr,
                            double tolerance = 1e-3);

QString MatrixToString(vtkMatrix4x4* matrix);
QVector<double> MatrixToVector(vtkMatrix4x4* matrix);
vtkSmartPointer<vtkMatrix4x4> MatrixFromVector(const QVector<double>& values);

struct RigidMovement
{
    bool valid = false;
    std::array<double, 3> displacementMm{};
    std::array<double, 3> rotationDeg{};
};
// Displacement of a fixed reference point; fixed-axis XYZ angles, R = Rz * Ry * Rx.
RigidMovement DescribeRigidMovement(vtkMatrix4x4* matrix, const std::array<double, 3>& reference);
}
