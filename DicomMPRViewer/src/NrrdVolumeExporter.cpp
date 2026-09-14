#include "NrrdVolumeExporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <vtkImageData.h>

static bool scalarTypeToNrrd(int vtkType, QString& nrrdType, int& byteCount)
{
    switch (vtkType) {
        case VTK_UNSIGNED_CHAR: nrrdType = "uchar"; byteCount = 1; return true;
        case VTK_CHAR:          nrrdType = "char"; byteCount = 1; return true;
        case VTK_SIGNED_CHAR:   nrrdType = "signed char"; byteCount = 1; return true;
        case VTK_UNSIGNED_SHORT:nrrdType = "ushort"; byteCount = 2; return true;
        case VTK_SHORT:         nrrdType = "short"; byteCount = 2; return true;
        case VTK_UNSIGNED_INT:  nrrdType = "uint"; byteCount = 4; return true;
        case VTK_INT:           nrrdType = "int"; byteCount = 4; return true;
        case VTK_FLOAT:         nrrdType = "float"; byteCount = 4; return true;
        case VTK_DOUBLE:        nrrdType = "double"; byteCount = 8; return true;
        default: break;
    }
    return false;
}

bool NrrdVolumeExporter::exportToFile(vtkImageData* image,
                                      const QString& filePath,
                                      QString* errorMessage)
{
    if (!image || image->GetNumberOfPoints() == 0) {
        if (errorMessage) *errorMessage = "Volumen nulo o vacio.";
        return false;
    }
    if (image->GetNumberOfScalarComponents() != 1) {
        if (errorMessage) *errorMessage = "Solo se soportan volumenes escalares de un componente.";
        return false;
    }

    QString typeName;
    int scalarBytes = 0;
    if (!scalarTypeToNrrd(image->GetScalarType(), typeName, scalarBytes)) {
        if (errorMessage) *errorMessage = "Tipo escalar VTK no soportado para NRRD.";
        return false;
    }

    QFileInfo info(filePath);
    QDir().mkpath(info.absolutePath());

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (errorMessage) *errorMessage = file.errorString();
        return false;
    }

    int extent[6] = {};
    double spacing[3] = {1.0, 1.0, 1.0};
    double origin[3] = {0.0, 0.0, 0.0};
    image->GetExtent(extent);
    image->GetSpacing(spacing);
    image->GetOrigin(origin);

    const int sizes[3] = {
        extent[1] - extent[0] + 1,
        extent[3] - extent[2] + 1,
        extent[5] - extent[4] + 1
    };

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);
    out << "NRRD0005\n";
    out << "# Exported by DicomMPRViewer\n";
    out << "type: " << typeName << "\n";
    out << "dimension: 3\n";
    out << "space: left-posterior-superior\n";
    out << "sizes: " << sizes[0] << " " << sizes[1] << " " << sizes[2] << "\n";
    out << "space directions: (" << spacing[0] << ",0,0) "
        << "(0," << spacing[1] << ",0) "
        << "(0,0," << spacing[2] << ")\n";
    out << "space origin: (" << origin[0] << "," << origin[1] << "," << origin[2] << ")\n";
    out << "kinds: domain domain domain\n";
    out << "encoding: raw\n";
    out << "endian: little\n\n";
    out.flush();

    for (int z = extent[4]; z <= extent[5]; ++z) {
        for (int y = extent[2]; y <= extent[3]; ++y) {
            for (int x = extent[0]; x <= extent[1]; ++x) {
                const char* ptr = static_cast<const char*>(image->GetScalarPointer(x, y, z));
                if (!ptr || file.write(ptr, scalarBytes) != scalarBytes) {
                    if (errorMessage) *errorMessage = "Error escribiendo datos NRRD.";
                    return false;
                }
            }
        }
    }

    return true;
}
