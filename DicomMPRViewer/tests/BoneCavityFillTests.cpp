#include "BoneCavityFill.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vtkImageData.h>
#include <vtkMatrix3x3.h>
#include <vtkPointData.h>
#include <vtkDataArray.h>

static void require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static vtkSmartPointer<vtkImageData> fixture()
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetExtent(-5, 25, 10, 30, -10, 10);
    image->SetSpacing(0.4, 0.6, 1.2);
    image->SetOrigin(35, -60, 105);
    auto direction = vtkSmartPointer<vtkMatrix3x3>::New();
    direction->Identity();
    direction->SetElement(0, 0, 0);
    direction->SetElement(0, 1, -1);
    direction->SetElement(1, 0, 1);
    direction->SetElement(1, 1, 0);
    image->SetDirectionMatrix(direction);
    image->AllocateScalars(VTK_UNSIGNED_SHORT, 1);
    image->GetPointData()->GetScalars()->FillComponent(0, 0);
    // Two separate hollow boxes: only the seeded one may be filled.
    for (int baseX : {0, 12})
        for (int z = -4; z <= 4; ++z)
            for (int y = 14; y <= 24; ++y)
                for (int x = baseX; x <= baseX + 8; ++x) {
                    bool wall = z == -4 || z == 4 || y == 14 || y == 24 || x == baseX || x == baseX + 8;
                    image->SetScalarComponentFromDouble(x, y, z, 0, wall ? 6 : 2);
                }
    for (int label = 3; label <= 8; ++label) {
        if (label == 6) continue;
        image->SetScalarComponentFromDouble(4, 19, label - 5, 0, label);
    }
    return image;
}

static std::array<double, 3> world(vtkImageData* image, int x, int y, int z)
{
    std::array<double, 3> point{};
    const int index[3] = {x, y, z};
    image->TransformIndexToPhysicalPoint(index, point.data());
    return point;
}

int main()
{
    try {
        auto input = fixture();
        auto result = fillEnclosedBoneCavity(input, 6, world(input, 3, 19, 0));
        require(result.labelmap != nullptr, "Enclosed cavity was not filled");
        require(result.addedVoxels == 7 * 9 * 7 - 5, "Unexpected filled voxel count");
        require(result.replacedSoftTissue, "Soft tissue replacement was not reported");
        require(std::abs(result.addedVolumeMm3 - result.addedVoxels * 0.4 * 0.6 * 1.2) < 1e-9,
                "Anisotropic physical volume is wrong");
        require(result.labelmap->GetScalarType() == input->GetScalarType(), "Scalar type changed");
        for (int a = 0; a < 3; ++a) {
            require(result.labelmap->GetSpacing()[a] == input->GetSpacing()[a], "Spacing changed");
            require(result.labelmap->GetOrigin()[a] == input->GetOrigin()[a], "Origin changed");
            for (int b = 0; b < 3; ++b)
                require(result.labelmap->GetDirectionMatrix()->GetElement(a, b) == input->GetDirectionMatrix()->GetElement(a, b),
                        "Direction changed");
        }
        for (int z = -10; z <= 10; ++z)
            for (int y = 10; y <= 30; ++y)
                for (int x = -5; x <= 25; ++x) {
                    const auto old = input->GetScalarComponentAsDouble(x, y, z, 0);
                    const auto expected = x > 0 && x < 8 && y > 14 && y < 24 && z > -4 && z < 4 && old == 2 ? 6 : old;
                    require(result.labelmap->GetScalarComponentAsDouble(x, y, z, 0) == expected,
                            "Fill altered the cortex, another cavity or a protected label");
                }
        require(input->GetScalarComponentAsDouble(3, 19, 0, 0) == 2, "Input was modified");
        require(!fillEnclosedBoneCavity(result.labelmap, 6, world(input, 3, 19, 0)).labelmap,
                "Click on filled bone should not start another fill");
        require(!fillEnclosedBoneCavity(input, 3, world(input, 3, 19, 0)).labelmap, "Tooth label accepted as bone");
        require(!fillEnclosedBoneCavity(input, 6, world(input, 4, 19, 2)).labelmap, "Protected canal seed accepted");
        require(!fillEnclosedBoneCavity(input, 6, world(input, -3, 19, 0)).labelmap, "Exterior seed accepted");
        require(!fillEnclosedBoneCavity(input, 6, {std::numeric_limits<double>::quiet_NaN(), 0, 0}).labelmap,
                "Invalid coordinate accepted");
        input->SetScalarComponentFromDouble(0, 19, 0, 0, 0);
        require(!fillEnclosedBoneCavity(input, 6, world(input, 3, 19, 0)).labelmap, "Open cortex was filled");
        input = fixture();
        input->SetScalarComponentFromDouble(0, 14, -4, 0, 0);
        require(!fillEnclosedBoneCavity(input, 6, world(input, 3, 19, 0)).labelmap, "Diagonal leak was filled");
        input = fixture();
        input->SetScalarComponentFromDouble(0, 19, 0, 0, 7);
        require(!fillEnclosedBoneCavity(input, 6, world(input, 3, 19, 0)).labelmap,
                "Another label artificially sealed a leak");
        input = fixture();
        for (int x = 0; x <= 8; ++x)
            for (int y = 14; y <= 24; ++y)
                for (int z = -4; z <= 4; ++z)
                    if (input->GetScalarComponentAsDouble(x, y, z, 0) == 6)
                        input->SetScalarComponentFromDouble(x, y, z, 0, 5);
        require(fillEnclosedBoneCavity(input, 5, world(input, 3, 19, 0)).labelmap != nullptr,
                "Maxillary cavity was not filled");
        std::cout << "Bone cavity fill: enclosed, protected labels, geometry metadata and leak checks passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
