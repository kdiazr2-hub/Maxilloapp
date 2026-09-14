#include "ProjectSerializer.h"
#include "SplintDesignCore.h"

#include <QFile>
#include <QTemporaryDir>

#include <vtkCubeSource.h>

#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

void testSplintDesignsRoundTrip()
{
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    const QString path = dir.filePath(QStringLiteral("caso.maxilloproject"));

    auto designs = SplintDesignCore::DefaultDesigns(206, -1001, -1002);
    int index = -1;
    QString error;
    require(SplintDesignCore::AddDesign(designs, QStringLiteral("Prueba"), 206, -1002, &index, &error),
            error.toStdString());
    designs[static_cast<size_t>(index)].upperPoints = {{1, 2, 3}, {4, 5, 6}, {7, 8, 9}};
    designs[static_cast<size_t>(index)].params.edgeOffsetMm = 2.5;
    designs[static_cast<size_t>(index)].editedContourUV = {{{0, 0}, {5, 0}, {5, 5}}};

    ProjectState state;
    state.dicomFolder = QStringLiteral("C:/dicom/caso");
    state.splintDesigns = SplintDesignCore::DesignsToJson(designs);
    state.activeSplintDesignId = designs[static_cast<size_t>(index)].id;
    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    cube->Update();
    auto splintMesh = vtkSmartPointer<vtkPolyData>::New();
    splintMesh->DeepCopy(cube->GetOutput());
    ProjObjectEntry entry;
    entry.label = designs[static_cast<size_t>(index)].label;
    entry.name = QStringLiteral("Férula Prueba");
    entry.color = QColor(240, 240, 230);
    state.objects.append(entry);
    state.objectMeshes[entry.label] = splintMesh;

    require(ProjectSerializer::save(path, state, &error), "save failed: " + error.toStdString());
    ProjectState loaded;
    require(ProjectSerializer::load(path, loaded, &error), "load failed: " + error.toStdString());

    require(loaded.splintDesigns == state.splintDesigns, "splint designs changed on disk");
    require(loaded.activeSplintDesignId == state.activeSplintDesignId, "active design not preserved");
    require(loaded.objectMeshes.contains(entry.label) && loaded.objectMeshes[entry.label]->GetNumberOfPolys() > 0,
            "splint mesh of an extra design not preserved");
    const auto restored = SplintDesignCore::DesignsFromJson(loaded.splintDesigns,
                                                            SplintDesignCore::DefaultDesigns(206, -1001, -1002));
    require(restored.size() == 3 && restored[2].name == QStringLiteral("Prueba") &&
                restored[2].editedContourUV == designs[static_cast<size_t>(index)].editedContourUV,
            "designs not restorable from the project");
}

void testLegacyProjectWithoutDesigns()
{
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    const QString path = dir.filePath(QStringLiteral("viejo.maxilloproject"));
    QFile file(path);
    require(file.open(QIODevice::WriteOnly), "cannot write legacy project");
    file.write(R"({"version": 1, "dicomFolder": "C:/dicom/viejo", "objects": [], "assets": {}})");
    file.close();

    ProjectState loaded;
    QString error;
    require(ProjectSerializer::load(path, loaded, &error), "legacy load failed: " + error.toStdString());
    require(loaded.splintDesigns.isEmpty() && loaded.activeSplintDesignId.isEmpty(), "legacy project invented designs");
    const auto designs = SplintDesignCore::DesignsFromJson(loaded.splintDesigns,
                                                           SplintDesignCore::DefaultDesigns(206, -1001, -1002));
    require(designs.size() == 2, "legacy project does not fall back to the default designs");

    // Saving a state without designs keeps the file free of the new keys.
    const QString resaved = dir.filePath(QStringLiteral("resaved.maxilloproject"));
    require(ProjectSerializer::save(resaved, loaded, &error), "resave failed: " + error.toStdString());
    QFile check(resaved);
    require(check.open(QIODevice::ReadOnly), "cannot read resaved project");
    require(!check.readAll().contains("splintDesigns"), "empty designs were written");
}
} // namespace

int main()
{
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"splint designs round trip", testSplintDesignsRoundTrip},
        {"legacy project without designs", testLegacyProjectWithoutDesigns},
    };
    int failures = 0;
    for (const auto& [name, test] : tests) {
        try {
            test();
            std::cout << "PASS " << name << '\n';
        } catch (const std::exception& error) {
            std::cerr << "FAIL " << name << ": " << error.what() << '\n';
            ++failures;
        }
    }
    return failures == 0 ? 0 : 1;
}
