#include "MaskToObjectCore.h"
#include "BoneCavityFill.h"
#include <vtkImageData.h>
#include <vtkImplicitPolyDataDistance.h>
#include <vtkPolyData.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main()
{
    try {
        for (int label : {1, 5, 6}) {
            auto mask = vtkSmartPointer<vtkImageData>::New();
            mask->SetDimensions(21, 21, 21);
            mask->SetSpacing(0.4, 0.7, 1.2);
            mask->SetOrigin(12, -30, 40);
            const double direction[9] = {0, -1, 0, 1, 0, 0, 0, 0, 1};
            mask->SetDirectionMatrix(direction);
            mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
            for (int z = 0; z < 21; ++z)
                for (int y = 0; y < 21; ++y)
                    for (int x = 0; x < 21; ++x) {
                        const bool outer = x >= 2 && x <= 18 && y >= 2 && y <= 18 && z >= 2 && z <= 18;
                        const bool inner = x >= 4 && x <= 16 && y >= 4 && y <= 16 && z >= 4 && z <= 16;
                        mask->SetScalarComponentFromDouble(x, y, z, 0, outer ? inner ? 2 : label : 0);
                    }
            std::array<double, 3> seed;
            const double index[3] = {10, 10, 10};
            mask->TransformContinuousIndexToPhysicalPoint(index, seed.data());
            const auto unfilled = MaskToObjectCore::Convert(mask, label);
            auto distance = vtkSmartPointer<vtkImplicitPolyDataDistance>::New();
            distance->SetInput(unfilled);
            require(distance->EvaluateFunction(seed.data()) > 0, "Conversion invented new bone filling");
            auto filled = fillEnclosedBoneCavity(mask, label, seed);
            require(filled.labelmap && filled.addedVoxels > 0, "Cavity fixture was not filled");
            auto snapshot = vtkSmartPointer<vtkImageData>::New();
            snapshot->DeepCopy(filled.labelmap);
            const auto stamp = filled.labelmap->GetMTime();
            QString error;
            const auto mesh = MaskToObjectCore::Convert(filled.labelmap, label, &error);
            require(mesh && error.isEmpty(), "Filled mask conversion failed");
            distance->SetInput(mesh);
            require(distance->EvaluateFunction(seed.data()) < 0, "The object lost the committed filling");
            require(filled.labelmap->GetMTime() == stamp, "Conversion modified the source mask");
            require(std::memcmp(snapshot->GetScalarPointer(), filled.labelmap->GetScalarPointer(),
                                snapshot->GetNumberOfPoints()) == 0, "Conversion changed label values");
            // A one-voxel feature must survive conversion, independent of display smoothing.
            filled.labelmap->SetScalarComponentFromDouble(19, 10, 10, 0, label);
            const auto detailed = MaskToObjectCore::Convert(filled.labelmap, label);
            const double tipIndex[3] = {19, 10, 10};
            double tip[3];
            filled.labelmap->TransformContinuousIndexToPhysicalPoint(tipIndex, tip);
            distance->SetInput(detailed);
            require(distance->EvaluateFunction(tip) < 0, "Conversion smoothed away a voxel feature");
        }
        require(!MaskToObjectCore::Convert(nullptr, 5), "Invalid mask accepted");
        std::cout << "MaskToObjectTests OK\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
