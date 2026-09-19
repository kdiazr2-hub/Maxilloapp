// PlateProbe — builds custom Le Fort plates on a real project's bones, the way GUIAS does, and reports and
// renders them. A development tool (not a CTest): real anatomy is what the synthetic tests cannot reproduce.
//
//   PlateProbe.exe --project proyecto.maxilloproject --out carpeta [--advance 6] [--down 3] [--detail 0.3]
//
// It takes the bones before repositioning and the Le Fort landmarks from the project, moves the segment by
// the given advancement (forward) and descent, puts a paranasal plate on each side of the aperture (two
// holes above the cut, two below), builds them with PlateCore, and writes the plates as STL, a report, and
// frontal / oblique / lateral renderings of the planned bone with the plates on it.

#include "GuideDesignCore.h"
#include "OsteotomyCore.h"
#include "PlateCore.h"
#include "WrapCore.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <vtkActor.h>
#include <vtkCamera.h>
#include <vtkPNGWriter.h>
#include <vtkPolyData.h>
#include <vtkPolyDataMapper.h>
#include <vtkProperty.h>
#include <vtkRenderWindow.h>
#include <vtkRenderer.h>
#include <vtkSTLReader.h>
#include <vtkSTLWriter.h>
#include <vtkStaticCellLocator.h>
#include <vtkTransform.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkWindowToImageFilter.h>
#include <vtkXMLPolyDataReader.h>

#include <chrono>
#include <cmath>
#include <iostream>

namespace
{
using Vec3 = std::array<double, 3>;

vtkSmartPointer<vtkPolyData> readMesh(const QString& path)
{
    // The .vtp next to the .stl keeps the same geometry and loads faster.
    const QString vtp = QFileInfo(path).path() + QStringLiteral("/") + QFileInfo(path).completeBaseName() +
                        QStringLiteral(".vtp");
    if (QFileInfo::exists(vtp)) {
        auto reader = vtkSmartPointer<vtkXMLPolyDataReader>::New();
        reader->SetFileName(vtp.toUtf8().constData());
        reader->Update();
        auto out = vtkSmartPointer<vtkPolyData>::New();
        out->DeepCopy(reader->GetOutput());
        return out;
    }
    auto reader = vtkSmartPointer<vtkSTLReader>::New();
    reader->SetFileName(path.toUtf8().constData());
    reader->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(reader->GetOutput());
    return out;
}

vtkSmartPointer<vtkPolyData> moved(vtkPolyData* mesh, const std::array<double, 16>& m)
{
    auto transform = vtkSmartPointer<vtkTransform>::New();
    transform->SetMatrix(m.data());
    auto filter = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
    filter->SetInputData(mesh);
    filter->SetTransform(transform);
    filter->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(filter->GetOutput());
    return out;
}

// First surface hit along a line from `from` to `to`.
bool hit(vtkPolyData* mesh, const Vec3& from, const Vec3& to, Vec3& at)
{
    auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
    locator->SetDataSet(mesh);
    locator->BuildLocator();
    double t = 0.0, x[3] = {}, pcoords[3] = {};
    int subId = 0;
    double a[3] = {from[0], from[1], from[2]}, b[3] = {to[0], to[1], to[2]};
    if (!locator->IntersectWithLine(a, b, 1e-6, t, x, pcoords, subId))
        return false;
    at = {x[0], x[1], x[2]};
    return true;
}

void render(const std::vector<vtkPolyData*>& bones, const std::vector<vtkPolyData*>& plates, const Vec3& focal,
            const Vec3& eyeDirection, const QString& file)
{
    auto renderer = vtkSmartPointer<vtkRenderer>::New();
    renderer->SetBackground(0.12, 0.12, 0.14);
    for (vtkPolyData* bone : bones) {
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(bone);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.87, 0.80, 0.72);
        renderer->AddActor(actor);
    }
    for (vtkPolyData* plate : plates) {
        auto mapper = vtkSmartPointer<vtkPolyDataMapper>::New();
        mapper->SetInputData(plate);
        mapper->ScalarVisibilityOff();
        auto actor = vtkSmartPointer<vtkActor>::New();
        actor->SetMapper(mapper);
        actor->GetProperty()->SetColor(0.62, 0.67, 0.78);
        actor->GetProperty()->SetSpecular(0.4);
        renderer->AddActor(actor);
    }
    auto window = vtkSmartPointer<vtkRenderWindow>::New();
    window->SetOffScreenRendering(1);
    window->SetSize(1100, 850);
    window->AddRenderer(renderer);
    vtkCamera* camera = renderer->GetActiveCamera();
    camera->SetFocalPoint(focal.data());
    camera->SetPosition(focal[0] + 200.0 * eyeDirection[0], focal[1] + 200.0 * eyeDirection[1],
                        focal[2] + 200.0 * eyeDirection[2]);
    camera->SetViewUp(0.0, 0.0, 1.0);
    camera->ParallelProjectionOn();
    camera->SetParallelScale(26.0);
    renderer->ResetCameraClippingRange();
    window->Render();
    auto grab = vtkSmartPointer<vtkWindowToImageFilter>::New();
    grab->SetInput(window);
    grab->Update();
    auto writer = vtkSmartPointer<vtkPNGWriter>::New();
    writer->SetFileName(file.toUtf8().constData());
    writer->SetInputConnection(grab->GetOutputPort());
    writer->Write();
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QString projectPath, outDir = QStringLiteral(".");
    double advance = 6.0, down = 3.0, detail = 0.3, closing = 3.0;
    for (int i = 1; i + 1 < argc; ++i) {
        const QString key = QString::fromLocal8Bit(argv[i]);
        const QString value = QString::fromLocal8Bit(argv[i + 1]);
        if (key == QStringLiteral("--project")) projectPath = value, ++i;
        else if (key == QStringLiteral("--out")) outDir = value, ++i;
        else if (key == QStringLiteral("--advance")) advance = value.toDouble(), ++i;
        else if (key == QStringLiteral("--down")) down = value.toDouble(), ++i;
        else if (key == QStringLiteral("--detail")) detail = value.toDouble(), ++i;
        else if (key == QStringLiteral("--closing")) closing = value.toDouble(), ++i;
    }
    QFile file(projectPath);
    if (!file.open(QIODevice::ReadOnly)) {
        std::cerr << "cannot open the project\n";
        return 1;
    }
    const QJsonObject project = QJsonDocument::fromJson(file.readAll()).object();
    const QDir projectDir = QFileInfo(projectPath).dir();
    const QJsonObject preop = project.value(QStringLiteral("assets")).toObject().value(QStringLiteral("preRepositionMeshes")).toObject();
    const auto cranial = readMesh(projectDir.filePath(preop.value(QStringLiteral("205")).toString()));
    const auto segmentBefore = readMesh(projectDir.filePath(preop.value(QStringLiteral("206")).toString()));
    std::cout << "cranial base " << cranial->GetNumberOfPolys() << " triangles, Le Fort segment "
              << segmentBefore->GetNumberOfPolys() << "\n";

    // The Le Fort cut, as the segment reference stored it: pilar R, piriform R, piriform L, pilar L.
    const QJsonObject plan = project.value(QStringLiteral("osteotomyPlan")).toObject();
    const QJsonArray flat = plan.value(QStringLiteral("segmentReferences")).toObject().value(QStringLiteral("206")).toObject()
                                .value(QStringLiteral("landmarks")).toArray();
    if (flat.size() != 12) {
        std::cerr << "no Le Fort landmarks in the project\n";
        return 1;
    }
    std::array<Vec3, 4> l{};
    for (int i = 0; i < 4; ++i)
        l[static_cast<size_t>(i)] = {flat[3 * i].toDouble(), flat[3 * i + 1].toDouble(), flat[3 * i + 2].toDouble()};
    const OsteotomyPath path = OsteotomyCore::LeFortPath({l[1], l[2], l[0], l[3]});
    if (!path.valid) {
        std::cerr << "the Le Fort path is not valid: " << path.error.toStdString() << "\n";
        return 1;
    }

    // Forward: from the segment's middle towards the piriform landmarks, level.
    double b[6] = {};
    segmentBefore->GetBounds(b);
    const Vec3 middle{0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
    const Vec3 piriform{0.5 * (l[1][0] + l[2][0]), 0.5 * (l[1][1] + l[2][1]), 0.5 * (l[1][2] + l[2][2])};
    Vec3 forward{piriform[0] - middle[0], piriform[1] - middle[1], 0.0};
    const double length = std::hypot(forward[0], forward[1]);
    forward = {forward[0] / length, forward[1] / length, 0.0};
    const std::array<double, 16> motion{1.0, 0.0, 0.0, advance * forward[0], 0.0, 1.0, 0.0, advance * forward[1],
                                        0.0, 0.0, 1.0, -down, 0.0, 0.0, 0.0, 1.0};
    const auto segmentPlanned = moved(segmentBefore, motion);
    std::cout << "motion: " << advance << " mm forward, " << down << " mm down\n";

    const auto started = std::chrono::steady_clock::now();
    WrapParams wrapParams;
    wrapParams.gapClosingMm = closing;
    wrapParams.smallestDetailMm = detail;
    const WrapResult wrap = WrapCore::Wrap({cranial.Get(), segmentPlanned.Get()}, wrapParams);
    if (!wrap.ok) {
        std::cerr << "wrap failed: " << wrap.error.toStdString() << "\n";
        return 1;
    }
    GuideDesignParams prepareParams;
    prepareParams.base.smallestDetailMm = detail;
    prepareParams.base.thicknessMm = 1.0;
    const GuidePreparation planned = GuideDesignCore::Prepare(wrap.mesh, prepareParams);
    std::cout << "planned bone wrapped in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s\n";

    // A paranasal plate each side: 5 mm lateral to the piriform landmark, holes 11 and 5.5 mm above the cut
    // (on the cranial base) and 5.5 and 11 mm below it (on the segment, before the move, then moved).
    std::vector<PlateDesign> plates;
    for (const int side : {1, 2}) {
        const Vec3 landmark = l[static_cast<size_t>(side)];
        const double outward = side == 1 ? -1.0 : 1.0; // x grows to the patient's left
        PlateDesign plate;
        plate.name = side == 1 ? QStringLiteral("Placa derecha") : QStringLiteral("Placa izquierda");
        plate.side = side == 1 ? PlateSide::Right : PlateSide::Left;
        for (const double dz : {11.0, 5.5, -5.5, -11.0}) {
            const Vec3 target{landmark[0] + 5.0 * outward, landmark[1], landmark[2] + dz};
            const Vec3 from{target[0] + 30.0 * forward[0], target[1] + 30.0 * forward[1], target[2]};
            const Vec3 to{target[0] - 30.0 * forward[0], target[1] - 30.0 * forward[1], target[2]};
            Vec3 at{};
            const bool onSegment = dz < 0.0;
            if (!hit(onSegment ? segmentBefore : cranial, from, to, at)) {
                std::cerr << plate.name.toStdString() << ": no bone for the hole at " << dz << " mm\n";
                continue;
            }
            PlateHole hole;
            hole.center = onSegment ? PlateCore::TransformPoint(motion, at) : at;
            hole.axis = forward;
            plate.holes.push_back(hole);
        }
        plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, static_cast<int>(plate.holes.size()), 0);
        plates.push_back(plate);
    }
    // An L plate on the right, as the surgeon's case had: the piriform arm (three holes), the buttress arm on the
    // zygomaticomaxillary pilar (three holes) and the bar joining their lowest holes along the segment.
    {
        PlateDesign plate;
        plate.name = QStringLiteral("Placa en L derecha");
        plate.side = PlateSide::Right;
        plate.kind = PlateTemplate::LShape;
        const auto addHole = [&](const Vec3& landmark, double lateralMm, double dz) {
            const Vec3 target{landmark[0] + lateralMm, landmark[1], landmark[2] + dz};
            const Vec3 from{target[0] + 30.0 * forward[0], target[1] + 30.0 * forward[1], target[2]};
            const Vec3 to{target[0] - 30.0 * forward[0], target[1] - 30.0 * forward[1], target[2]};
            Vec3 at{};
            const bool onSegment = dz < 0.0;
            if (!hit(onSegment ? segmentBefore : cranial, from, to, at)) {
                std::cerr << "L plate: no bone for a hole at " << dz << " mm\n";
                return false;
            }
            plate.holes.push_back({onSegment ? PlateCore::TransformPoint(motion, at) : at, forward, PlateBone::Unknown});
            return true;
        };
        int first = 0;
        for (const double dz : {11.0, 5.5, -6.0})
            first += addHole(l[1], -4.0, dz) ? 1 : 0;
        int second = 0;
        for (const double dz : {9.0, 4.5, -6.0})
            second += addHole(l[0], 2.0, dz) ? 1 : 0;
        plate.struts = PlateCore::TemplateStruts(PlateTemplate::LShape, first, second);
        plates.push_back(plate);
    }
    PlateCore::AssignBones(plates, cranial, segmentPlanned);
    const PlateBoneQuery boneAt = PlateCore::MakeBoneQuery(cranial, segmentPlanned, motion, path);

    QDir().mkpath(outDir);
    std::vector<vtkSmartPointer<vtkPolyData>> built;
    Vec3 focal{0.0, 0.0, 0.0};
    int holes = 0;
    for (const PlateDesign& plate : plates) {
        PlateParams params;
        const auto t0 = std::chrono::steady_clock::now();
        const PlateBuildResult result = PlateCore::Build(planned, plate, params, boneAt);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!result.ok) {
            std::cerr << plate.name.toStdString() << ": " << result.error.toStdString() << "\n";
            continue;
        }
        // How far into the bone the plate reaches: the planned wrap's field at its vertices.
        double deepest = 0.0;
        int inside = 0;
        for (vtkIdType id = 0; id < result.mesh->GetNumberOfPoints(); ++id) {
            double p[3] = {};
            result.mesh->GetPoint(id, p);
            const double d = planned.wrapField->At({p[0], p[1], p[2]});
            deepest = std::min(deepest, d);
            inside += d < -0.25 ? 1 : 0;
        }
        std::cout << result.report.toStdString() << " (" << seconds << " s)\n"
                  << "    bridged " << result.bridgedMm << " mm, vertices inside the bone " << inside
                  << ", deepest " << -deepest << " mm\n";
        auto writer = vtkSmartPointer<vtkSTLWriter>::New();
        writer->SetFileName(QDir(outDir).filePath(plate.name + QStringLiteral(".stl")).toUtf8().constData());
        writer->SetInputData(result.mesh);
        writer->Write();
        built.push_back(result.mesh);
        for (const PlateHole& hole : plate.holes) {
            for (int a = 0; a < 3; ++a)
                focal[static_cast<size_t>(a)] += hole.center[static_cast<size_t>(a)];
            ++holes;
        }
    }
    for (double& v : focal)
        v /= std::max(1, holes);
    std::vector<vtkPolyData*> bones{cranial, segmentPlanned};
    std::vector<vtkPolyData*> plateMeshes;
    for (const auto& mesh : built)
        plateMeshes.push_back(mesh);
    const Vec3 lateral{forward[1], -forward[0], 0.0};
    render(bones, plateMeshes, focal, forward, QDir(outDir).filePath(QStringLiteral("frontal.png")));
    const Vec3 oblique{0.7 * forward[0] + 0.7 * lateral[0], 0.7 * forward[1] + 0.7 * lateral[1], 0.15};
    render(bones, plateMeshes, focal, oblique, QDir(outDir).filePath(QStringLiteral("oblique.png")));
    render(bones, plateMeshes, focal, {lateral[0], lateral[1], 0.0}, QDir(outDir).filePath(QStringLiteral("lateral.png")));
    std::cout << "written to " << outDir.toStdString() << "\n";
    return 0;
}
