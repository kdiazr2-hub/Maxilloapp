#pragma once

#include <QString>

class vtkImageData;

class NrrdVolumeExporter
{
public:
    static bool exportToFile(vtkImageData* image,
                             const QString& filePath,
                             QString* errorMessage);
};
