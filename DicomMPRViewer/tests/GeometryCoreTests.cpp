#include "CompositeModelCore.h"
#include "AppStateManager.h"
#include "GeometryValidation.h"
#include "GeometryValidationCore.h"
#include "LandmarkCore.h"
#include "RegistrationResult.h"
#include "TransformCore.h"

#include <cmath>
#include <iostream>

#include <vtkCubeSource.h>
#include <vtkImageData.h>
#include <vtkLandmarkTransform.h>
#include <vtkMatrix4x4.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>
#include <vtkTransform.h>
#include <limits>

namespace
{
int fail(const char* message)
{
    std::cerr << "FAIL: " << message << "\n";
    return 1;
}

vtkSmartPointer<vtkPolyData> cube(double x, double y, double z)
{
    auto src = vtkSmartPointer<vtkCubeSource>::New();
    src->SetXLength(x);
    src->SetYLength(y);
    src->SetZLength(z);
    src->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(src->GetOutput());
    return out;
}

double distance3(const double a[3], const double b[3])
{
    const double dx = a[0] - b[0];
    const double dy = a[1] - b[1];
    const double dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
}

int main()
{
    auto motionTransform = vtkSmartPointer<vtkTransform>::New();
    const std::array<double, 3> reference{12.0, -70.0, 35.0};
    motionTransform->Translate(reference[0] + 3.0, reference[1] - 2.0, reference[2] + 4.0);
    motionTransform->RotateZ(30.0);
    motionTransform->RotateY(-20.0);
    motionTransform->RotateX(10.0);
    motionTransform->Translate(-reference[0], -reference[1], -reference[2]);
    const auto motion = TransformCore::DescribeRigidMovement(motionTransform->GetMatrix(), reference);
    if (!motion.valid) return fail("Valid movement matrix was rejected.");
    const std::array<double, 3> displacement{3.0, -2.0, 4.0}, angles{10.0, -20.0, 30.0};
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(motion.displacementMm[axis] - displacement[axis]) > 1e-8)
            return fail("Movement must describe the reference point, not the matrix origin.");
        if (std::abs(motion.rotationDeg[axis] - angles[axis]) > 1e-8)
            return fail("XYZ rotation decomposition is incorrect.");
    }
    for (double y : {-90.0, 90.0}) {
        motionTransform->Identity();
        motionTransform->RotateZ(30.0);
        motionTransform->RotateY(y);
        motionTransform->RotateX(10.0);
        const auto singular = TransformCore::DescribeRigidMovement(motionTransform->GetMatrix(), reference);
        if (!singular.valid) return fail("Gimbal-lock rotation was rejected.");
        auto rebuilt = vtkSmartPointer<vtkTransform>::New();
        rebuilt->RotateZ(singular.rotationDeg[2]);
        rebuilt->RotateY(singular.rotationDeg[1]);
        rebuilt->RotateX(singular.rotationDeg[0]);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c)
                if (std::abs(rebuilt->GetMatrix()->GetElement(r, c) - motionTransform->GetMatrix()->GetElement(r, c)) > 1e-8)
                    return fail("Gimbal-lock decomposition changed the rotation.");
    }
    motionTransform->Identity();
    motionTransform->Scale(1.2, 1.0, 1.0);
    if (TransformCore::DescribeRigidMovement(motionTransform->GetMatrix(), reference).valid)
        return fail("Scaling must not be reported as rigid movement.");
    auto invalidMotion = TransformCore::IdentityMatrix();
    invalidMotion->SetElement(0, 3, std::numeric_limits<double>::quiet_NaN());
    if (TransformCore::DescribeRigidMovement(invalidMotion, reference).valid)
        return fail("Non-finite motion must not be reported as zero.");

    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(128, 128, 80);
    image->SetSpacing(0.4, 0.4, 0.8);
    image->SetOrigin(-30.0, -25.0, -20.0);
    image->AllocateScalars(VTK_SHORT, 1);
    QString dicomReport;
    if (!GeometryValidation::ValidateDicomGeometry(image, &dicomReport))
        return fail("DICOM geometry validation rejected a valid image.");
    if (GeometryValidationCore::ValidateDicomGeometry(image).isError())
        return fail("GeometryValidationCore rejected a valid image.");

    auto skull = cube(120.0, 90.0, 110.0);
    QString scaleReport;
    if (!GeometryValidation::ValidateMeshScale(skull, QStringLiteral("Maxilar"), &scaleReport))
        return fail("Mesh scale validation rejected a human-scale mesh.");
    if (GeometryValidationCore::ValidateMeshScale(skull, AnatomicalObjectType::Maxilla).isError())
        return fail("GeometryValidationCore rejected a human-scale maxilla mesh.");

    auto t = TransformCore::IdentityMatrix();
    t->SetElement(0, 3, 12.0);
    t->SetElement(1, 3, -4.0);
    t->SetElement(2, 3, 3.5);
    QString transformReport;
    if (!TransformCore::ValidateRigidTransform(t, &transformReport))
        return fail("Rigid transform validation rejected translation.");

    auto inv = TransformCore::InvertTransform(t);
    auto composed = TransformCore::ComposeTransforms({t.GetPointer(), inv.GetPointer()});
    if (!TransformCore::IsIdentity(composed, 1e-5))
        return fail("Transform inverse roundtrip did not return identity.");

    double p[4] = {1.0, 2.0, 3.0, 1.0};
    double moved[4] = {};
    double back[4] = {};
    t->MultiplyPoint(p, moved);
    inv->MultiplyPoint(moved, back);
    double p3[3] = {p[0], p[1], p[2]};
    double back3[3] = {back[0], back[1], back[2]};
    if (distance3(p3, back3) > 1e-6)
        return fail("Point transform/inverse transform roundtrip failed.");

    auto src = vtkSmartPointer<vtkPoints>::New();
    auto dst = vtkSmartPointer<vtkPoints>::New();
    src->InsertNextPoint(0.0, 0.0, 0.0);
    src->InsertNextPoint(10.0, 0.0, 0.0);
    src->InsertNextPoint(0.0, 10.0, 0.0);
    dst->DeepCopy(src);
    auto landmark = vtkSmartPointer<vtkLandmarkTransform>::New();
    landmark->SetSourceLandmarks(src);
    landmark->SetTargetLandmarks(dst);
    landmark->SetModeToRigidBody();
    landmark->Update();
    if (!TransformCore::IsIdentity(landmark->GetMatrix(), 1e-6))
        return fail("Identical landmarks did not produce identity transform.");

    auto arch = cube(50.0, 30.0, 20.0);
    const vtkIdType bonePtsBefore = skull->GetNumberOfPoints();
    const vtkIdType archPtsBefore = arch->GetNumberOfPoints();
    QString compositeReport;
    QString compositeError;
    auto composite = CompositeModelCore::CreateNonDestructiveComposite(
        skull, nullptr, arch, t, QStringLiteral("Maxilar"), &compositeReport, &compositeError);
    if (!composite || composite->GetNumberOfPoints() == 0)
        return fail("Composite creation failed.");
    if (skull->GetNumberOfPoints() != bonePtsBefore || arch->GetNumberOfPoints() != archPtsBefore)
        return fail("Composite creation modified source objects.");

    AppStateManager appState;
    if (!appState.can(AppCapability::LoadDicom))
        return fail("Open DICOM should be available in the initial state.");
    appState.setVolumeLoaded(true);
    appState.setMaxillaAvailable(true);
    appState.setUpperArchImported(true);
    appState.setUpperPointPairCount(2);
    if (appState.can(AppCapability::MatchUpper))
        return fail("Upper registration enabled with fewer than 3 point pairs.");
    appState.setUpperPointPairCount(3);
    if (!appState.can(AppCapability::MatchUpper))
        return fail("Upper registration did not enable with 3 point pairs.");
    appState.setUpperRegistered(true);
    if (!appState.can(AppCapability::CreateComposite))
        return fail("Composite should be enabled after an accepted registration.");

    RegistrationResult rr;
    rr.landmarkRms = 0.42;
    rr.meanDistance = 0.31;
    rr.maxDistance = 1.2;
    rr.p95Distance = 0.74;
    rr.accepted = true;
    rr.matrix = TransformCore::MatrixToVector(t);
    const auto rr2 = RegistrationResult::fromJson(rr.toJson());
    if (!rr2.accepted || std::abs(rr2.landmarkRms - rr.landmarkRms) > 1e-9 ||
        rr2.matrix.size() != 16)
        return fail("RegistrationResult JSON round-trip failed.");

    Landmark a{QStringLiteral("A"), QStringLiteral("A"), QVector3D(0.0f, 0.0f, 0.0f)};
    Landmark b{QStringLiteral("B"), QStringLiteral("B"), QVector3D(3.0f, 4.0f, 12.0f)};
    if (std::abs(LandmarkCore::distance3D(a, b) - 13.0) > 1e-6)
        return fail("Landmark distance3D failed.");

    Landmark c{QStringLiteral("C"), QStringLiteral("C"), QVector3D(1.0f, 0.0f, 0.0f)};
    Landmark d{QStringLiteral("D"), QStringLiteral("D"), QVector3D(0.0f, 1.0f, 0.0f)};
    Plane plane;
    if (!LandmarkCore::makePlaneFromThreePoints(QStringLiteral("P"), QStringLiteral("XY"), a, c, d, &plane))
        return fail("Plane creation from three non-collinear landmarks failed.");
    Landmark e{QStringLiteral("E"), QStringLiteral("E"), QVector3D(0.0f, 0.0f, 5.0f)};
    if (std::abs(LandmarkCore::pointToPlaneDistance(e, plane) - 5.0) > 1e-6)
        return fail("Point-to-plane distance failed.");

    std::cout << "GeometryCoreTests OK\n";
    return 0;
}
