// PlateProbe — builds custom Le Fort plates on a real project's bones, the way GUIAS does, and reports and
// renders them. A development tool (not a CTest): real anatomy is what the synthetic tests cannot reproduce.
//
//   PlateProbe.exe --project proyecto.maxilloproject --out carpeta [--advance 6] [--down 3] [--detail 0.3]
//                  [--template paranasal|splintless]
//
// It takes the bones before repositioning and the Le Fort landmarks from the project, moves the segment by
// the given advancement (forward) and descent, puts a paranasal plate on each side of the aperture (two
// holes above the cut, two below), builds them with PlateCore, and writes the plates as STL, a report, and
// frontal / oblique / lateral renderings of the planned bone with the plates on it.
//
// It then lays out the cutting guide those plates imply (LeFortGuideCore, on the bone BEFORE the cut) and
// renders it the same way, so both halves of the workflow can be judged on real anatomy.
//
// It then lays out the cutting guide those plates imply (LeFortGuideCore on the bone BEFORE the cut) and
// renders it the same way, so both halves of the workflow can be judged on real anatomy.

#include "GuideDesignCore.h"
#include "LeFortGuideCore.h"
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
#include <vtkImplicitPolyDataDistance.h>
#include <vtkPolyDataConnectivityFilter.h>
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

// How far a built part reaches into the bone, measured on the bone MESHES themselves. A wrap is no use for
// this: closed tightly it is a shredded film, closed loosely it stands proud of every concavity, and either
// way it answers a question about itself rather than about the bone.
struct Penetration
{
    int inside = 0;
    int total = 0;
    double deepestMm = 0.0;
};

// The bone as its rasterised shell with whatever is enclosed filled in: "inside" means inside bone MATERIAL,
// not inside the cranial cavity or the maxillary sinus, which a mesh's own signed distance calls inside by
// tens of millimetres. Validated on the spot: every vertex of the bone must read about zero.
Penetration intoBone(vtkPolyData* part, const std::shared_ptr<const ImplicitCore::BakedField>& bone)
{
    Penetration out;
    if (!bone)
        return out;
    for (vtkIdType id = 0; id < part->GetNumberOfPoints(); ++id) {
        double q[3] = {};
        part->GetPoint(id, q);
        const double d = bone->At({q[0], q[1], q[2]});
        out.deepestMm = std::min(out.deepestMm, d);
        out.inside += d < -0.25 ? 1 : 0;
        ++out.total;
    }
    return out;
}

// Says so out loud if the instrument itself is wrong.
void checkBoneField(const char* what, const std::shared_ptr<const ImplicitCore::BakedField>& bone,
                    const std::vector<vtkPolyData*>& meshes)
{
    double worst = 0.0;
    for (vtkPolyData* mesh : meshes)
        for (vtkIdType id = 0; id < mesh->GetNumberOfPoints(); id += 331) {
            double q[3] = {};
            mesh->GetPoint(id, q);
            worst = std::max(worst, std::abs(bone->At({q[0], q[1], q[2]})));
        }
    if (worst > 1.0)
        std::cerr << "  WARNING: the " << what << " field is " << worst << " mm off its own surface" << std::endl;
}

int shells(vtkPolyData* mesh)
{
    auto regions = vtkSmartPointer<vtkPolyDataConnectivityFilter>::New();
    regions->SetInputData(mesh);
    regions->SetExtractionModeToAllRegions();
    regions->Update();
    return regions->GetNumberOfExtractedRegions();
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
    double advance = 6.0, down = 3.0, detail = 0.3, closing = 3.0, guideClosing = 1.5;
    QString plateTemplate = QStringLiteral("paranasal");
    for (int i = 1; i + 1 < argc; ++i) {
        const QString key = QString::fromLocal8Bit(argv[i]);
        const QString value = QString::fromLocal8Bit(argv[i + 1]);
        if (key == QStringLiteral("--project")) projectPath = value, ++i;
        else if (key == QStringLiteral("--out")) outDir = value, ++i;
        else if (key == QStringLiteral("--advance")) advance = value.toDouble(), ++i;
        else if (key == QStringLiteral("--down")) down = value.toDouble(), ++i;
        else if (key == QStringLiteral("--detail")) detail = value.toDouble(), ++i;
        else if (key == QStringLiteral("--closing")) closing = value.toDouble(), ++i;
        else if (key == QStringLiteral("--template")) plateTemplate = value, ++i;
        else if (key == QStringLiteral("--guide-closing")) guideClosing = value.toDouble(), ++i;
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
    // Penetration is judged on the real bone: a tight wrap (0.5 mm closing), not the planning wrap that fills
    // the corner of the step at the cut. The same bones with the segment also where it was before the movement
    // close the osteotomy gap: the arms are pulled taut over that, so they ramp across the step.
    // The keep-out is the same envelope the plate is laid on, plus the Le Fort segment where it was before the
    // movement, so the union also fills the space the movement vacated. It has to be a CLOSED bone: segmented
    // maxilla is a perforated shell around an open sinus, and a field made from the meshes themselves lets an
    // arm pass straight through the sinus without ever reporting bone.
    PlateKeepOut keepOut;
    keepOut.bone = ImplicitCore::BakeMeshField({cranial.Get(), segmentPlanned.Get()}, detail, 8.0);
    const WrapResult gapWrap = WrapCore::Wrap({cranial.Get(), segmentPlanned.Get(), segmentBefore.Get()}, wrapParams);
    const GuidePreparation withGap = gapWrap.ok ? GuideDesignCore::Prepare(gapWrap.mesh, prepareParams) : GuidePreparation{};
    keepOut.boneAndGap = withGap.ok ? withGap.wrapField : keepOut.bone;
    std::cout << "planned bone wrapped in "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() << " s\n";

    // Where a hole lands: from `landmark`, `lateralMm` sideways and `dz` up, shot at the bone along `forward`.
    // Holes below the cut are taken on the segment BEFORE the move and carried along the motion, which is how
    // the surgeon clicks them on the planned bone.
    std::vector<PlateDesign> plates;
    const auto holeOn = [&](PlateDesign& plate, const Vec3& landmark, double lateralMm, double dz) {
        const Vec3 target{landmark[0] + lateralMm, landmark[1], landmark[2] + dz};
        const Vec3 from{target[0] + 30.0 * forward[0], target[1] + 30.0 * forward[1], target[2]};
        const Vec3 to{target[0] - 30.0 * forward[0], target[1] - 30.0 * forward[1], target[2]};
        Vec3 at{};
        const bool onSegment = dz < 0.0;
        vtkPolyData* bone = onSegment ? segmentBefore.Get() : cranial.Get();
        if (!hit(bone, from, to, at)) {
            // The surgeon clicks on the surface he can see. When the straight shot misses — the buttress falls
            // away laterally — take the nearest point of that bone instead, as a click would.
            auto locator = vtkSmartPointer<vtkStaticCellLocator>::New();
            locator->SetDataSet(bone);
            locator->BuildLocator();
            double closest[3] = {};
            vtkIdType cell = 0;
            int sub = 0;
            double squared = 0.0;
            locator->FindClosestPoint(const_cast<double*>(target.data()), closest, cell, sub, squared);
            if (squared > 400.0) {
                std::cerr << plate.name.toStdString() << ": no bone for a hole at " << dz << " mm" << std::endl;
                return false;
            }
            at = {closest[0], closest[1], closest[2]};
        }
        const Vec3 center = onSegment ? PlateCore::TransformPoint(motion, at) : at;
        plate.holes.push_back({center, forward, PlateBone::Unknown});
        return true;
    };
    if (plateTemplate == QStringLiteral("splintless")) {
        // One plate over four pillars, as the surgeon builds it: nasomaxillary right, zygomaticomaxillary
        // right, nasomaxillary left, zygomaticomaxillary left, each two holes above the cut and two below,
        // and a bar joining the four lowest holes across the midline.
        PlateDesign plate;
        plate.name = QStringLiteral("Placa splintless");
        plate.kind = PlateTemplate::Splintless;
        std::array<int, 4> pillarHoles{};
        int pillar = 0;
        for (const auto& [landmark, lateralMm] : {std::pair{l[1], -4.0}, std::pair{l[0], 2.0},
                                                  std::pair{l[2], 4.0}, std::pair{l[3], -2.0}}) {
            int placed = 0;
            for (const double dz : {10.0, 5.0, -5.0, -10.0})
                placed += holeOn(plate, landmark, lateralMm, dz) ? 1 : 0;
            pillarHoles[static_cast<size_t>(pillar++)] = placed;
        }
        plate.struts = PlateCore::SplintlessStruts(pillarHoles);
        plates.push_back(plate);
    } else {
        // A paranasal plate each side: 5 mm lateral to the piriform landmark, holes 11 and 5.5 mm above the
        // cut (on the cranial base) and 5.5 and 11 mm below it (on the segment).
        for (const int side : {1, 2}) {
            PlateDesign plate;
            plate.name = side == 1 ? QStringLiteral("Placa derecha") : QStringLiteral("Placa izquierda");
            plate.side = side == 1 ? PlateSide::Right : PlateSide::Left;
            const double outward = side == 1 ? -5.0 : 5.0; // x grows to the patient's left
            for (const double dz : {11.0, 5.5, -5.5, -11.0})
                holeOn(plate, l[static_cast<size_t>(side)], outward, dz);
            plate.struts = PlateCore::TemplateStruts(PlateTemplate::Paranasal, static_cast<int>(plate.holes.size()), 0);
            plates.push_back(plate);
        }
    }
    // The screw axis is the bone's own normal, as the app takes it at the click.
    for (PlateDesign& plate : plates)
        for (PlateHole& hole : plate.holes)
            hole.axis = PlateCore::BoneNormalAt(*keepOut.bone, hole.center, forward);
    PlateCore::AssignBones(plates, cranial, segmentPlanned);
    const PlateBoneQuery boneAt = PlateCore::MakeBoneQuery(cranial, segmentPlanned, motion, path);

    QDir().mkpath(outDir);
    const auto& plannedSolid = keepOut.bone;
    checkBoneField("planned bone", plannedSolid, {cranial, segmentPlanned});
    std::vector<vtkSmartPointer<vtkPolyData>> built;
    Vec3 focal{0.0, 0.0, 0.0};
    int holes = 0;
    for (const PlateDesign& plate : plates) {
        PlateParams params;
        const auto t0 = std::chrono::steady_clock::now();
        const PlateBuildResult result =
            PlateCore::Build(planned, plate, params, boneAt, nullptr, keepOut);
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (!result.ok) {
            std::cerr << plate.name.toStdString() << ": " << result.error.toStdString() << "\n";
            continue;
        }
        const Penetration deep = intoBone(result.mesh, plannedSolid);
        std::cout << result.report.toStdString() << " (" << seconds << " s)" << std::endl
                  << "    bridged " << result.bridgedMm << " mm, vertices inside the bone " << deep.inside << "/"
                  << deep.total << ", deepest " << -deep.deepestMm << " mm" << std::endl;
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
    const Vec3 oblique{0.7 * forward[0] + 0.7 * lateral[0], 0.7 * forward[1] + 0.7 * lateral[1], 0.15};
    const Vec3 side{lateral[0], lateral[1], 0.0};
    render(bones, plateMeshes, focal, forward, QDir(outDir).filePath(QStringLiteral("frontal.png")));
    render(bones, plateMeshes, focal, oblique, QDir(outDir).filePath(QStringLiteral("oblique.png")));
    render(bones, plateMeshes, focal, side, QDir(outDir).filePath(QStringLiteral("lateral.png")));

    // ── The cutting guide those plates imply, on the bone BEFORE the cut ─────────────────────────────────
    const Vec3 segmentMiddle{0.5 * (b[0] + b[1]), 0.5 * (b[2] + b[3]), 0.5 * (b[4] + b[5])};
    const std::vector<PredictiveHole> predictive = PlateCore::PredictHoles(plates, motion, path, segmentMiddle);
    WrapParams guideWrapParams;
    guideWrapParams.gapClosingMm = guideClosing;
    guideWrapParams.smallestDetailMm = detail;
    guideWrapParams.smoothingIterations = 30;
    GuideDesignParams guideDesign;
    guideDesign.base.smallestDetailMm = detail;
    guideDesign.bone = ImplicitCore::BakeMeshField({cranial.Get(), segmentBefore.Get()}, detail, 8.0);
    const WrapResult preopWrap = WrapCore::Wrap({cranial.Get(), segmentBefore.Get()}, guideWrapParams);
    const GuidePreparation preopPrepared =
        preopWrap.ok ? GuideDesignCore::Prepare(preopWrap.mesh, guideDesign) : GuidePreparation{};
    if (!preopPrepared.ok) {
        std::cerr << "the pre-operative envelope failed: "
                  << (preopWrap.ok ? preopPrepared.error : preopWrap.error).toStdString() << std::endl;
        return 1;
    }
    LeFortGuideParams guideParams;
    const SleeveParams sleeveParams;
    guideParams.sleeveOuterDiameterMm = sleeveParams.outerDiameterMm;
    const LeFortGuideLayout layout =
        LeFortGuideCore::Layout(preopPrepared, preopWrap.mesh, path, predictive, guideParams);
    if (!layout.ok) {
        std::cerr << "the guide layout failed: " << layout.error.toStdString() << std::endl;
        return 1;
    }
    std::cout << layout.report.toStdString() << std::endl;
    const GuideRegion region = GuideBaseCore::MakeBrushRegion(preopPrepared.wrapField, layout.paint, guideDesign.base);
    std::vector<GuideFigure> sleeves = PlateCore::SleeveFigures(predictive, sleeveParams);
    sleeves.insert(sleeves.end(), layout.figures.begin(), layout.figures.end());
    const auto guideStarted = std::chrono::steady_clock::now();
    const GuideDesignResult guide =
        GuideDesignCore::Build(preopPrepared, region, layout.slotPlan, layout.fixation, sleeves, guideDesign);
    if (!guide.ok) {
        std::cerr << "the guide was not built: " << guide.error.toStdString() << std::endl;
        return 1;
    }
    std::cout << "guide: " << guide.mesh->GetNumberOfPolys() << " triangles, " << shells(guide.mesh) << " piece(s), "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - guideStarted).count() << " s"
              << std::endl;
    {
        checkBoneField("pre-operative bone", guideDesign.bone, {cranial, segmentBefore});
        const Penetration deep = intoBone(guide.mesh, guideDesign.bone);
        std::cout << "    guide vertices inside the bone " << deep.inside << "/" << deep.total << ", deepest "
                  << -deep.deepestMm << " mm" << std::endl;
    }
    auto guideWriter = vtkSmartPointer<vtkSTLWriter>::New();
    guideWriter->SetFileName(QDir(outDir).filePath(QStringLiteral("guia.stl")).toUtf8().constData());
    guideWriter->SetInputData(guide.mesh);
    guideWriter->Write();
    std::vector<vtkPolyData*> preopBones{cranial, segmentBefore};
    std::vector<vtkPolyData*> guideMesh{guide.mesh};
    render(preopBones, guideMesh, focal, forward, QDir(outDir).filePath(QStringLiteral("guia_frontal.png")));
    render(preopBones, guideMesh, focal, oblique, QDir(outDir).filePath(QStringLiteral("guia_oblicua.png")));
    render(preopBones, guideMesh, focal, side, QDir(outDir).filePath(QStringLiteral("guia_lateral.png")));
    render({}, guideMesh, focal, forward, QDir(outDir).filePath(QStringLiteral("guia_sola.png")));

    std::cout << "written to " << outDir.toStdString() << "\n";
    return 0;
}
