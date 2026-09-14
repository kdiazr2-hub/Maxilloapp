#include "CompositeBlockCore.h"
#include "ProjectSerializer.h"
#include "SplintDesignCore.h"

#include <QFile>
#include <QTemporaryDir>

#include <vtkAppendPolyData.h>
#include <vtkCellData.h>
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

void testCompositePartsAndBlocksRoundTrip()
{
    QTemporaryDir dir;
    require(dir.isValid(), "no temporary directory");
    const QString path = dir.filePath(QStringLiteral("compuesto.maxilloproject"));

    auto boneSource = vtkSmartPointer<vtkCubeSource>::New();
    boneSource->SetCenter(0.0, 0.0, 20.0);
    boneSource->Update();
    auto dentalSource = vtkSmartPointer<vtkCubeSource>::New();
    dentalSource->Update();
    auto append = vtkSmartPointer<vtkAppendPolyData>::New();
    append->AddInputData(CompositeBlockCore::TagPart(boneSource->GetOutput(), CompositeBlockCore::BonePart));
    append->AddInputData(CompositeBlockCore::TagPart(dentalSource->GetOutput(), CompositeBlockCore::DentalPart));
    append->Update();
    auto composite = vtkSmartPointer<vtkPolyData>::New();
    composite->DeepCopy(append->GetOutput());

    CompositeCutBlock block;
    block.center = {1.0, 2.0, 3.0};
    block.sizeMm = {40.0, 30.0, 15.0};
    block.valid = true;
    ProjectState state;
    state.upperCompositeMesh = composite;
    state.compositeBlocks[QStringLiteral("upper")] = CompositeBlockCore::BlockToJson(block);

    QString error;
    require(ProjectSerializer::save(path, state, &error), "save failed: " + error.toStdString());
    ProjectState loaded;
    require(ProjectSerializer::load(path, loaded, &error), "load failed: " + error.toStdString());
    require(CompositeBlockCore::HasParts(loaded.upperCompositeMesh), "composite part tags lost on disk");
    const auto dental = CompositeBlockCore::ExtractPart(loaded.upperCompositeMesh, CompositeBlockCore::DentalPart);
    require(dental && dental->GetNumberOfPolys() == dentalSource->GetOutput()->GetNumberOfPolys(),
            "dental part changed on disk");
    const CompositeCutBlock restored = CompositeBlockCore::BlockFromJson(loaded.compositeBlocks.value(QStringLiteral("upper")).toObject());
    require(restored.valid && restored.center == block.center && restored.sizeMm == block.sizeMm, "cutting block lost on disk");

    // An untagged mesh saved over it must not come back with stale tags.
    loaded.upperCompositeMesh = CompositeBlockCore::ExtractPart(loaded.upperCompositeMesh, CompositeBlockCore::BonePart);
    loaded.upperCompositeMesh->GetCellData()->RemoveArray(CompositeBlockCore::PartArrayName);
    require(ProjectSerializer::save(path, loaded, &error), "resave failed: " + error.toStdString());
    ProjectState reloaded;
    require(ProjectSerializer::load(path, reloaded, &error), "reload failed: " + error.toStdString());
    require(reloaded.upperCompositeMesh && !CompositeBlockCore::HasParts(reloaded.upperCompositeMesh),
            "stale part tags came back after saving an untagged composite");
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
        {"composite parts and blocks round trip", testCompositePartsAndBlocksRoundTrip},
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
