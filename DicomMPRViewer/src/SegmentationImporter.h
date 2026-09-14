#pragma once

#include <QString>
#include <vtkSmartPointer.h>

class vtkImageData;

class SegmentationImporter
{
public:
    static vtkSmartPointer<vtkImageData> importLabelmap(const QString& filePath,
                                                        QString* errorMessage);
};
