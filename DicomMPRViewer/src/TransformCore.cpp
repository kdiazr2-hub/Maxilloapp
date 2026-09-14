#include "TransformCore.h"

#include <algorithm>
#include <cmath>

#include <vtkPolyData.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>

namespace
{
double det3(vtkMatrix4x4* m)
{
    const double a00 = m->GetElement(0, 0);
    const double a01 = m->GetElement(0, 1);
    const double a02 = m->GetElement(0, 2);
    const double a10 = m->GetElement(1, 0);
    const double a11 = m->GetElement(1, 1);
    const double a12 = m->GetElement(1, 2);
    const double a20 = m->GetElement(2, 0);
    const double a21 = m->GetElement(2, 1);
    const double a22 = m->GetElement(2, 2);
    return a00 * (a11 * a22 - a12 * a21)
         - a01 * (a10 * a22 - a12 * a20)
         + a02 * (a10 * a21 - a11 * a20);
}
}

namespace TransformCore
{
RigidMovement DescribeRigidMovement(vtkMatrix4x4* matrix, const std::array<double, 3>& reference)
{
    RigidMovement result;
    if (!matrix) return result;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(matrix->GetElement(r, c))) return result;
    for (double coordinate : reference)
        if (!std::isfinite(coordinate)) return result;
    if (!ValidateRigidTransform(matrix, nullptr, 1e-5)) return result;
    const double point[4] = {reference[0], reference[1], reference[2], 1.0};
    double moved[4];
    matrix->MultiplyPoint(point, moved);
    for (int axis = 0; axis < 3; ++axis)
        result.displacementMm[axis] = moved[axis] - reference[axis];
    const double cy = std::hypot(matrix->GetElement(0, 0), matrix->GetElement(1, 0));
    const double y = std::atan2(-matrix->GetElement(2, 0), cy);
    const double x = cy > 1e-8
        ? std::atan2(matrix->GetElement(2, 1), matrix->GetElement(2, 2))
        : std::atan2(-matrix->GetElement(1, 2), matrix->GetElement(1, 1));
    const double z = cy > 1e-8
        ? std::atan2(matrix->GetElement(1, 0), matrix->GetElement(0, 0)) : 0.0;
    const double radiansToDegrees = 180.0 / std::acos(-1.0);
    result.rotationDeg = {x * radiansToDegrees, y * radiansToDegrees, z * radiansToDegrees};
    result.valid = true;
    return result;
}

vtkSmartPointer<vtkMatrix4x4> IdentityMatrix()
{
    auto out = vtkSmartPointer<vtkMatrix4x4>::New();
    out->Identity();
    return out;
}

vtkSmartPointer<vtkMatrix4x4> CloneMatrix(vtkMatrix4x4* matrix)
{
    auto out = IdentityMatrix();
    if (matrix) out->DeepCopy(matrix);
    return out;
}

vtkSmartPointer<vtkMatrix4x4> InvertTransform(vtkMatrix4x4* matrix)
{
    auto out = IdentityMatrix();
    if (!matrix) return out;
    vtkMatrix4x4::Invert(matrix, out);
    return out;
}

vtkSmartPointer<vtkMatrix4x4> ComposeTransforms(const QVector<vtkMatrix4x4*>& transforms)
{
    auto result = IdentityMatrix();
    for (vtkMatrix4x4* t : transforms) {
        if (!t) continue;
        auto tmp = IdentityMatrix();
        vtkMatrix4x4::Multiply4x4(t, result, tmp);
        result->DeepCopy(tmp);
    }
    return result;
}

vtkSmartPointer<vtkPolyData> ApplyTransformToPolyData(vtkPolyData* mesh, vtkMatrix4x4* matrix)
{
    if (!mesh || mesh->GetNumberOfPoints() == 0) return nullptr;
    if (!matrix || IsIdentity(matrix)) {
        auto copy = vtkSmartPointer<vtkPolyData>::New();
        copy->DeepCopy(mesh);
        return copy;
    }

    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(matrix);

    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();

    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

bool IsIdentity(vtkMatrix4x4* matrix, double tolerance)
{
    if (!matrix) return true;
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            const double expected = (r == c) ? 1.0 : 0.0;
            if (std::abs(matrix->GetElement(r, c) - expected) > tolerance)
                return false;
        }
    }
    return true;
}

bool ValidateRigidTransform(vtkMatrix4x4* matrix, QString* report, double tolerance)
{
    if (!matrix) {
        if (report) *report = QStringLiteral("Transformacion nula.");
        return false;
    }

    const double det = det3(matrix);
    double maxNormErr = 0.0;
    double maxDot = 0.0;
    for (int c = 0; c < 3; ++c) {
        double normSq = 0.0;
        for (int r = 0; r < 3; ++r)
            normSq += matrix->GetElement(r, c) * matrix->GetElement(r, c);
        maxNormErr = std::max(maxNormErr, std::abs(std::sqrt(normSq) - 1.0));
    }
    for (int a = 0; a < 3; ++a) {
        for (int b = a + 1; b < 3; ++b) {
            double dot = 0.0;
            for (int r = 0; r < 3; ++r)
                dot += matrix->GetElement(r, a) * matrix->GetElement(r, b);
            maxDot = std::max(maxDot, std::abs(dot));
        }
    }

    const bool bottomRowOk =
        std::abs(matrix->GetElement(3, 0)) <= tolerance &&
        std::abs(matrix->GetElement(3, 1)) <= tolerance &&
        std::abs(matrix->GetElement(3, 2)) <= tolerance &&
        std::abs(matrix->GetElement(3, 3) - 1.0) <= tolerance;

    const bool ok = std::abs(det - 1.0) <= tolerance * 10.0 &&
                    maxNormErr <= tolerance * 10.0 &&
                    maxDot <= tolerance * 10.0 &&
                    bottomRowOk;
    if (report) {
        *report = QStringLiteral("det=%1 maxNormErr=%2 maxDot=%3 bottomRow=%4")
            .arg(det, 0, 'f', 6)
            .arg(maxNormErr, 0, 'f', 6)
            .arg(maxDot, 0, 'f', 6)
            .arg(bottomRowOk ? QStringLiteral("ok") : QStringLiteral("bad"));
    }
    return ok;
}

QString MatrixToString(vtkMatrix4x4* matrix)
{
    if (!matrix) return QStringLiteral("<sin matriz>");
    QString out;
    for (int r = 0; r < 4; ++r) {
        if (r > 0) out += QLatin1Char('\n');
        out += QStringLiteral("%1 %2 %3 %4")
            .arg(matrix->GetElement(r, 0), 0, 'f', 8)
            .arg(matrix->GetElement(r, 1), 0, 'f', 8)
            .arg(matrix->GetElement(r, 2), 0, 'f', 8)
            .arg(matrix->GetElement(r, 3), 0, 'f', 8);
    }
    return out;
}

QVector<double> MatrixToVector(vtkMatrix4x4* matrix)
{
    QVector<double> values;
    values.reserve(16);
    vtkSmartPointer<vtkMatrix4x4> src = matrix ? CloneMatrix(matrix) : IdentityMatrix();
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            values.append(src->GetElement(r, c));
    return values;
}

vtkSmartPointer<vtkMatrix4x4> MatrixFromVector(const QVector<double>& values)
{
    auto out = IdentityMatrix();
    if (values.size() < 16) return out;
    int idx = 0;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            out->SetElement(r, c, values[idx++]);
    return out;
}
}
