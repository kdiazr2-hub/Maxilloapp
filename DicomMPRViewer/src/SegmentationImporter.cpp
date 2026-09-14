#include "SegmentationImporter.h"

#include <QFileInfo>

#include <vtkImageData.h>
#include <vtkNrrdReader.h>

vtkSmartPointer<vtkImageData> SegmentationImporter::importLabelmap(
    const QString& filePath,
    QString* errorMessage)
{
    if (!QFileInfo::exists(filePath)) {
        if (errorMessage) *errorMessage = "No existe el labelmap: " + filePath;
        return nullptr;
    }

    auto reader = vtkSmartPointer<vtkNrrdReader>::New();
    reader->SetFileName(filePath.toLocal8Bit().constData());
    reader->Update();

    vtkImageData* output = reader->GetOutput();
    if (!output || output->GetNumberOfPoints() == 0 || !output->GetPointData()) {
        if (errorMessage) *errorMessage = "El NRRD de segmentacion no contiene datos validos.";
        return nullptr;
    }

    auto labelmap = vtkSmartPointer<vtkImageData>::New();
    labelmap->DeepCopy(output);
    return labelmap;
}
