#pragma once

#include <QString>
#include <vtkSmartPointer.h>

#include <array>
#include <vector>

class vtkPolyData;

struct SplintGenerationInputs
{
    vtkPolyData* upperOcclusion = nullptr;
    vtkPolyData* lowerOcclusion = nullptr;
    QString name;
    double thicknessMm = 2.6;
    double archWidthMm = 18.0;
    double borderPaddingMm = 3.0;
    double indentationDepthMm = 1.2;
    double filletMm = 0.0;
    bool useUpperImpression = true;
    bool useLowerImpression = true;
    bool tryBooleanIndentation = true;
    std::vector<std::array<double, 3>> upperVestibularPoints;
    std::vector<std::array<double, 3>> upperPalatalPoints;
    std::vector<std::array<double, 3>> lowerVestibularPoints;
    std::vector<std::array<double, 3>> lowerLingualPoints;
};

struct SplintGenerationResult
{
    vtkSmartPointer<vtkPolyData> mesh;
    QString report;
    bool booleanApplied = false;
};

class SplintGenerator
{
public:
    static SplintGenerationResult Generate(const SplintGenerationInputs& inputs);
};
