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
        auto sealed = vtkSmartPointer<vtkImageData>::New();
        sealed->SetExtent(0, 20, 0, 20, 0, 20);
        sealed->SetSpacing(1.0, 1.0, 1.0);
        sealed->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        sealed->GetPointData()->GetScalars()->FillComponent(0, 0);
        for (int z = 4; z <= 14; ++z)
            for (int y = 4; y <= 14; ++y)
                for (int x = 4; x <= 14; ++x) {
                    const bool wall = x == 4 || x == 14 || y == 4 || y == 14 || z == 4 || z == 14;
                    if (wall)
                        sealed->SetScalarComponentFromDouble(x, y, z, 0, 5);
                }
        auto narrow = fillBetweenCortices(sealed, 5);
        require(narrow != nullptr, "The sealed gap between cortices was not filled");
        require(narrow->GetScalarComponentAsDouble(9, 9, 9, 0) == 5, "Marrow between cortices stayed empty");
        require(sealed->GetScalarComponentAsDouble(9, 9, 9, 0) == 0, "Cortical fill modified the mask");
        require(narrow->GetScalarComponentAsDouble(0, 0, 0, 0) == 0, "Air outside the bone became bone");
        require(narrow->GetScalarComponentAsDouble(1, 9, 9, 0) == 0, "The cortex grew outward");
        sealed->SetScalarComponentFromDouble(4, 9, 9, 0, 0); // a foramen, not a sealed box
        auto opened = fillBetweenCortices(sealed, 5);
        require(opened && opened->GetScalarComponentAsDouble(9, 9, 9, 0) == 5,
                "Marrow stayed empty when a foramen opens the cortex");
        require(opened->GetScalarComponentAsDouble(0, 0, 0, 0) == 0, "A foramen filled the air outside the bone");

        auto sinus = vtkSmartPointer<vtkImageData>::New();
        sinus->SetExtent(0, 44, 0, 44, 0, 44);
        sinus->SetSpacing(1.0, 1.0, 1.0);
        sinus->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        sinus->GetPointData()->GetScalars()->FillComponent(0, 0);
        for (int z = 2; z <= 40; ++z)
            for (int y = 2; y <= 40; ++y)
                for (int x = 2; x <= 40; ++x) {
                    const bool wall = x == 2 || x == 40 || y == 2 || y == 40 || z == 2 || z == 40;
                    if (wall)
                        sinus->SetScalarComponentFromDouble(x, y, z, 0, 5);
                }
        require(fillBetweenCortices(sinus, 5) == nullptr, "A sinus, the nasal cavity or an orbit was filled");

        sealed = vtkSmartPointer<vtkImageData>::New();
        sealed->SetExtent(0, 20, 0, 20, 0, 20);
        sealed->SetSpacing(1.0, 1.0, 1.0);
        sealed->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        sealed->GetPointData()->GetScalars()->FillComponent(0, 0);
        for (int z = 4; z <= 14; ++z)
            for (int y = 4; y <= 14; ++y)
                for (int x = 4; x <= 14; ++x) {
                    const bool wall = x == 4 || x == 14 || y == 4 || y == 14 || z == 4 || z == 14;
                    if (wall)
                        sealed->SetScalarComponentFromDouble(x, y, z, 0, 6);
                }
        sealed->SetScalarComponentFromDouble(8, 8, 8, 0, 7);
        auto kept = fillBetweenCortices(sealed, 6);
        require(kept && kept->GetScalarComponentAsDouble(8, 8, 8, 0) == 7, "The mandibular canal was overwritten");
        require(kept->GetScalarComponentAsDouble(7, 8, 8, 0) == 6, "Marrow beside the canal stayed empty");

        auto density = vtkSmartPointer<vtkImageData>::New();
        density->SetExtent(0, 20, 0, 20, 0, 20);
        density->SetSpacing(1.0, 1.0, 1.0);
        density->AllocateScalars(VTK_SHORT, 1);
        density->GetPointData()->GetScalars()->FillComponent(0, -1000);
        for (int z = 5; z <= 13; ++z)
            for (int y = 5; y <= 13; ++y)
                for (int x = 5; x <= 13; ++x)
                    density->SetScalarComponentFromDouble(x, y, z, 0, 400);
        auto marrow = fillBetweenCortices(sealed, 6, 20.0, density, kMarrowMinimumHu);
        require(marrow && marrow->GetScalarComponentAsDouble(9, 9, 9, 0) == 6, "Cancellous marrow was left hollow");
        require(marrow->GetScalarComponentAsDouble(0, 0, 0, 0) == 0, "Air outside the cortex became bone");
        density->GetPointData()->GetScalars()->FillComponent(0, -1000);
        require(fillBetweenCortices(sealed, 6, 20.0, density, kMarrowMinimumHu) == nullptr,
                "Air between the cortices was filled");
        for (int z = 5; z <= 13; ++z)
            for (int y = 5; y <= 13; ++y)
                for (int x = 5; x <= 13; ++x)
                    density->SetScalarComponentFromDouble(x, y, z, 0, -40);
        auto darkMarrow = fillBetweenCortices(sealed, 6, 20.0, density, kMarrowMinimumHu);
        require(darkMarrow && darkMarrow->GetScalarComponentAsDouble(9, 9, 9, 0) == 6,
                "Dark ramus marrow was left hollow");

        auto orbit = vtkSmartPointer<vtkImageData>::New();
        orbit->SetExtent(0, 20, 0, 20, 0, 20);
        orbit->SetSpacing(1.0, 1.0, 1.0);
        orbit->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        orbit->GetPointData()->GetScalars()->FillComponent(0, 0);
        for (int z = 2; z <= 18; ++z)
            for (int y = 2; y <= 18; ++y)
                for (int x = 2; x <= 18; ++x) {
                    const bool wall = x == 2 || x == 18 || y == 2 || y == 18 || z == 2 || z == 18;
                    if (wall)
                        orbit->SetScalarComponentFromDouble(x, y, z, 0, 6);
                }
        for (int z = 3; z <= 17; ++z)
            for (int x = 3; x <= 17; ++x)
                orbit->SetScalarComponentFromDouble(x, 2, z, 0, 0); // anterior opening, outer rim kept
        density->GetPointData()->GetScalars()->FillComponent(0, -1000);
        for (int z = 3; z <= 17; ++z)
            for (int y = 3; y <= 17; ++y)
                for (int x = 3; x <= 17; ++x)
                    density->SetScalarComponentFromDouble(x, y, z, 0, -40);
        require(fillBetweenCortices(orbit, 6, 20.0, density, kMarrowMinimumHu) == nullptr,
                "An orbit open toward anterior was filled");

        std::cout << "Bone cavity fill: enclosed, protected labels, geometry metadata and leak checks passed.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
