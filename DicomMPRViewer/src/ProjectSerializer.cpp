#include "ProjectSerializer.h"

#include "NrrdVolumeExporter.h"
#include "SegmentationImporter.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <limits>

#include <vtkPolyData.h>
#include <vtkSTLReader.h>
#include <vtkSTLWriter.h>

// ─────────────────────────────────────────────────────────────────────────────
// Helpers
// ─────────────────────────────────────────────────────────────────────────────

static QJsonArray vec3ToJson(const QVector<QVector3D>& pts)
{
    QJsonArray arr;
    for (const auto& p : pts) {
        QJsonArray pt;
        pt.append(p.x()); pt.append(p.y()); pt.append(p.z());
        arr.append(pt);
    }
    return arr;
}

static QVector<QVector3D> jsonToVec3(const QJsonArray& arr)
{
    QVector<QVector3D> pts;
    pts.reserve(arr.size());
    for (const auto& v : arr) {
        const auto pt = v.toArray();
        if (pt.size() >= 3)
            pts.append({ float(pt[0].toDouble()),
                         float(pt[1].toDouble()),
                         float(pt[2].toDouble()) });
    }
    return pts;
}

static QJsonArray matrixToJson(const QVector<double>& values)
{
    QJsonArray arr;
    for (double v : values) arr.append(v);
    return arr;
}

static QVector<double> jsonToMatrixVector(const QJsonArray& arr)
{
    QVector<double> values;
    values.reserve(16);
    for (const auto& v : arr) {
        if (!v.isDouble()) return {};
        values.append(v.toDouble());
    }
    if (values.size() != 16)
        values.clear();
    return values;
}

static bool saveStl(vtkPolyData* mesh, const QString& path, QString* err)
{
    if (!mesh) return false;
    auto w = vtkSmartPointer<vtkSTLWriter>::New();
    w->SetFileName(path.toLocal8Bit().constData());
    w->SetInputData(mesh);
    w->SetFileTypeToBinary();
    if (w->Write() == 0) {
        if (err) *err = QStringLiteral("STL write failed: ") + path;
        return false;
    }
    return true;
}

static vtkSmartPointer<vtkPolyData> loadStl(const QString& path)
{
    auto r = vtkSmartPointer<vtkSTLReader>::New();
    r->SetFileName(path.toLocal8Bit().constData());
    r->Update();
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->DeepCopy(r->GetOutput());
    return out->GetNumberOfPoints() > 0 ? out : nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────

QString ProjectSerializer::assetsDir(const QString& projectFilePath)
{
    const QFileInfo fi(projectFilePath);
    return fi.absolutePath() + QDir::separator() + fi.completeBaseName() + QStringLiteral("_assets");
}

// ─────────────────────────────────────────────────────────────────────────────
// save
// ─────────────────────────────────────────────────────────────────────────────
bool ProjectSerializer::save(const QString& projectFilePath,
                              const ProjectState& state,
                              QString* error)
{
    const QString aDir = assetsDir(projectFilePath);
    if (!QDir().mkpath(aDir)) {
        if (error) *error = QStringLiteral("No se pudo crear el directorio de assets: ") + aDir;
        return false;
    }

    // Helper: relative path from the .maxilloproject file's directory to an asset
    const QFileInfo fi(projectFilePath);
    auto rel = [&](const QString& absPath) {
        return QDir(fi.absolutePath()).relativeFilePath(absPath);
    };

    QJsonObject root;
    root[QStringLiteral("version")] = 1;
    root[QStringLiteral("dicomFolder")] = state.dicomFolder;

    // Window / level
    QJsonObject preset;
    preset[QStringLiteral("window")] = state.presetWindow;
    preset[QStringLiteral("level")]  = state.presetLevel;
    root[QStringLiteral("preset")]   = preset;

    // Masks table
    QJsonArray masksArr;
    for (const auto& m : state.masks) {
        QJsonObject o;
        o[QStringLiteral("label")]   = m.label;
        o[QStringLiteral("name")]    = m.name;
        o[QStringLiteral("color")]   = m.color.name(QColor::HexRgb);
        o[QStringLiteral("visible")] = m.visible;
        masksArr.append(o);
    }
    root[QStringLiteral("masks")] = masksArr;

    // Objects table
    QJsonArray objsArr;
    for (const auto& obj : state.objects) {
        QJsonObject o;
        o[QStringLiteral("label")] = obj.label;
        o[QStringLiteral("name")]  = obj.name;
        o[QStringLiteral("color")] = obj.color.name(QColor::HexRgb);
        o[QStringLiteral("visible")] = obj.visible;
        if (obj.opacity >= 0.0) {
            o[QStringLiteral("opacity")] = obj.opacity;
            o[QStringLiteral("alwaysOnTop")] = obj.alwaysOnTop;
        }
        objsArr.append(o);
    }
    root[QStringLiteral("objects")] = objsArr;
    if (state.mandibleMovement.targetLabel > 0) {
        QJsonObject movement;
        movement["targetLabel"] = state.mandibleMovement.targetLabel;
        movement["referenceCenter"] = matrixToJson(state.mandibleMovement.referenceCenter);
        movement["registrationMatrix"] = matrixToJson(state.mandibleMovement.registrationMatrix);
        movement["currentMatrix"] = matrixToJson(state.mandibleMovement.currentMatrix);
        root["mandibleMovement"] = movement;
    }

    // Hidden labels
    QJsonArray hiddenArr;
    for (int lbl : state.hiddenMaskLabels) hiddenArr.append(lbl);
    root[QStringLiteral("hiddenMaskLabels")] = hiddenArr;

    // Dental points
    QJsonObject ptsObj;
    ptsObj[QStringLiteral("maxillaBone")]  = vec3ToJson(state.maxillaBonePoints);
    ptsObj[QStringLiteral("upperArch")]    = vec3ToJson(state.upperArchPoints);
    ptsObj[QStringLiteral("mandibleBone")] = vec3ToJson(state.mandibleBonePoints);
    ptsObj[QStringLiteral("lowerArch")]    = vec3ToJson(state.lowerArchPoints);
    root[QStringLiteral("dentalPoints")] = ptsObj;

    QJsonObject registrationObj;
    auto saveRegistration = [](const QVector<double>& matrix,
                               const QString& report,
                               bool calculated,
                               double landmarkRms,
                               double meanDist,
                               double p95Dist) {
        QJsonObject o;
        o[QStringLiteral("calculated")] = calculated;
        o[QStringLiteral("matrix")] = matrixToJson(matrix);
        o[QStringLiteral("report")] = report;
        QJsonObject metrics;
        metrics[QStringLiteral("landmarkRms")]  = landmarkRms;
        metrics[QStringLiteral("meanDistance")] = meanDist;
        metrics[QStringLiteral("p95Distance")]  = p95Dist;
        o[QStringLiteral("metrics")] = metrics;
        return o;
    };
    registrationObj[QStringLiteral("upper")] = saveRegistration(
        state.upperRegistrationMatrix,
        state.upperRegistrationReport,
        state.upperRegistrationCalculated,
        state.upperLandmarkRms,
        state.upperMeanDist,
        state.upperP95Dist);
    registrationObj[QStringLiteral("lower")] = saveRegistration(
        state.lowerRegistrationMatrix,
        state.lowerRegistrationReport,
        state.lowerRegistrationCalculated,
        state.lowerLandmarkRms,
        state.lowerMeanDist,
        state.lowerP95Dist);
    root[QStringLiteral("registrations")] = registrationObj;

    // ── Assets ────────────────────────────────────────────────────────────────
    QJsonObject assets;

    // Labelmap
    if (state.labelmap) {
        const QString labelmapPath = aDir + QStringLiteral("/labelmap.nrrd");
        QString nrrdErr;
        if (!NrrdVolumeExporter::exportToFile(state.labelmap, labelmapPath, &nrrdErr)) {
            if (error) *error = QStringLiteral("Error guardando labelmap: ") + nrrdErr;
            return false;
        }
        assets[QStringLiteral("labelmap")] = rel(labelmapPath);
    }

    // Mask meshes (keyed by label)
    QJsonObject maskMeshFiles;
    for (auto it = state.maskMeshes.cbegin(); it != state.maskMeshes.cend(); ++it) {
        const QString meshPath = aDir + QStringLiteral("/mesh_%1.stl").arg(it.key());
        QString stlErr;
        if (saveStl(it.value(), meshPath, &stlErr))
            maskMeshFiles[QString::number(it.key())] = rel(meshPath);
    }
    assets[QStringLiteral("maskMeshes")] = maskMeshFiles;

    // Object meshes (keyed by label)
    QJsonObject objMeshFiles;
    for (auto it = state.objectMeshes.cbegin(); it != state.objectMeshes.cend(); ++it) {
        const QString meshPath = aDir + QStringLiteral("/obj_%1.stl").arg(it.key());
        QString stlErr;
        if (saveStl(it.value(), meshPath, &stlErr))
            objMeshFiles[QString::number(it.key())] = rel(meshPath);
    }
    assets[QStringLiteral("objectMeshes")] = objMeshFiles;

    // Special arch / composite meshes
    auto saveMeshAsset = [&](vtkPolyData* mesh, const QString& key, const QString& filename) {
        if (!mesh) return;
        const QString path = aDir + QDir::separator() + filename;
        QString stlErr;
        if (saveStl(mesh, path, &stlErr))
            assets[key] = rel(path);
    };
    saveMeshAsset(state.upperArchOriginalMesh.Get(), QStringLiteral("upperArchOriginal"), QStringLiteral("upper_arch_original.stl"));
    saveMeshAsset(state.lowerArchOriginalMesh.Get(), QStringLiteral("lowerArchOriginal"), QStringLiteral("lower_arch_original.stl"));
    saveMeshAsset(state.upperArchMesh.Get(),      QStringLiteral("upperArch"),      QStringLiteral("upper_arch.stl"));
    saveMeshAsset(state.lowerArchMesh.Get(),      QStringLiteral("lowerArch"),      QStringLiteral("lower_arch.stl"));
    saveMeshAsset(state.upperCompositeMesh.Get(), QStringLiteral("upperComposite"), QStringLiteral("upper_composite.stl"));
    saveMeshAsset(state.lowerCompositeMesh.Get(), QStringLiteral("lowerComposite"), QStringLiteral("lower_composite.stl"));

    root[QStringLiteral("assets")] = assets;

    // ── Write JSON file ───────────────────────────────────────────────────────
    QFile f(projectFilePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("No se pudo escribir: ") + projectFilePath;
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// load
// ─────────────────────────────────────────────────────────────────────────────
bool ProjectSerializer::load(const QString& projectFilePath,
                              ProjectState& state,
                              QString* error)
{
    QFile f(projectFilePath);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("No se pudo abrir: ") + projectFilePath;
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("Archivo JSON inválido.");
        return false;
    }
    const QJsonObject root = doc.object();

    // Helper: resolve relative path against the project file's directory
    const QFileInfo fi(projectFilePath);
    auto abs = [&](const QString& relPath) {
        return fi.absolutePath() + QDir::separator() + relPath;
    };

    state.dicomFolder = root[QStringLiteral("dicomFolder")].toString();

    // Window / level
    const QJsonObject preset = root[QStringLiteral("preset")].toObject();
    state.presetWindow = preset[QStringLiteral("window")].toDouble(2000.0);
    state.presetLevel  = preset[QStringLiteral("level")].toDouble(500.0);

    // Masks
    state.masks.clear();
    for (const auto& v : root[QStringLiteral("masks")].toArray()) {
        const QJsonObject o = v.toObject();
        ProjMaskEntry e;
        e.label   = o[QStringLiteral("label")].toInt();
        e.name    = o[QStringLiteral("name")].toString();
        e.color   = QColor(o[QStringLiteral("color")].toString());
        e.visible = o[QStringLiteral("visible")].toBool(true);
        state.masks.append(e);
    }

    // Objects
    state.mandibleMovement = {};
    const auto movement = root["mandibleMovement"].toObject();
    const auto reference = movement["referenceCenter"].toArray();
    if (reference.size() == 3) {
        state.mandibleMovement.targetLabel = movement["targetLabel"].toInt(-1);
        for (const auto& coordinate : reference)
            state.mandibleMovement.referenceCenter.append(coordinate.isDouble()
                ? coordinate.toDouble() : std::numeric_limits<double>::quiet_NaN());
        state.mandibleMovement.registrationMatrix = jsonToMatrixVector(movement["registrationMatrix"].toArray());
        state.mandibleMovement.currentMatrix = jsonToMatrixVector(movement["currentMatrix"].toArray());
    }
    state.objects.clear();
    for (const auto& v : root[QStringLiteral("objects")].toArray()) {
        const QJsonObject o = v.toObject();
        ProjObjectEntry e;
        e.label = o[QStringLiteral("label")].toInt();
        e.name  = o[QStringLiteral("name")].toString();
        e.color = QColor(o[QStringLiteral("color")].toString());
        e.visible = o[QStringLiteral("visible")].toBool(true);
        e.opacity = o[QStringLiteral("opacity")].toDouble(-1.0);
        if (e.opacity > 1.0) e.opacity = 1.0;
        e.alwaysOnTop = o[QStringLiteral("alwaysOnTop")].toBool(false);
        state.objects.append(e);
    }

    // Hidden labels
    state.hiddenMaskLabels.clear();
    for (const auto& v : root[QStringLiteral("hiddenMaskLabels")].toArray())
        state.hiddenMaskLabels.append(v.toInt());

    // Dental points
    const QJsonObject ptsObj = root[QStringLiteral("dentalPoints")].toObject();
    state.maxillaBonePoints  = jsonToVec3(ptsObj[QStringLiteral("maxillaBone")].toArray());
    state.upperArchPoints    = jsonToVec3(ptsObj[QStringLiteral("upperArch")].toArray());
    state.mandibleBonePoints = jsonToVec3(ptsObj[QStringLiteral("mandibleBone")].toArray());
    state.lowerArchPoints    = jsonToVec3(ptsObj[QStringLiteral("lowerArch")].toArray());

    const QJsonObject registrationObj = root[QStringLiteral("registrations")].toObject();
    auto loadRegistration = [](const QJsonObject& o,
                               QVector<double>& matrix,
                               QString& report,
                               bool& calculated,
                               double& landmarkRms,
                               double& meanDist,
                               double& p95Dist) {
        calculated  = o[QStringLiteral("calculated")].toBool(false);
        matrix      = jsonToMatrixVector(o[QStringLiteral("matrix")].toArray());
        report      = o[QStringLiteral("report")].toString();
        const QJsonObject metrics = o[QStringLiteral("metrics")].toObject();
        landmarkRms = metrics[QStringLiteral("landmarkRms")].toDouble(0.0);
        meanDist    = metrics[QStringLiteral("meanDistance")].toDouble(0.0);
        p95Dist     = metrics[QStringLiteral("p95Distance")].toDouble(0.0);
    };
    loadRegistration(registrationObj[QStringLiteral("upper")].toObject(),
                     state.upperRegistrationMatrix,
                     state.upperRegistrationReport,
                     state.upperRegistrationCalculated,
                     state.upperLandmarkRms,
                     state.upperMeanDist,
                     state.upperP95Dist);
    loadRegistration(registrationObj[QStringLiteral("lower")].toObject(),
                     state.lowerRegistrationMatrix,
                     state.lowerRegistrationReport,
                     state.lowerRegistrationCalculated,
                     state.lowerLandmarkRms,
                     state.lowerMeanDist,
                     state.lowerP95Dist);

    // ── Assets ────────────────────────────────────────────────────────────────
    const QJsonObject assets = root[QStringLiteral("assets")].toObject();

    // Labelmap
    state.labelmap = nullptr;
    if (assets.contains(QStringLiteral("labelmap"))) {
        const QString labelmapPath = abs(assets[QStringLiteral("labelmap")].toString());
        QString nrrdErr;
        state.labelmap = SegmentationImporter::importLabelmap(labelmapPath, &nrrdErr);
        if (!state.labelmap && error)
            *error = QStringLiteral("Error cargando labelmap: ") + nrrdErr;
    }

    // Mask meshes
    state.maskMeshes.clear();
    const QJsonObject maskMeshFiles = assets[QStringLiteral("maskMeshes")].toObject();
    for (auto it = maskMeshFiles.begin(); it != maskMeshFiles.end(); ++it) {
        const int label = it.key().toInt();
        const QString meshPath = abs(it.value().toString());
        if (auto mesh = loadStl(meshPath))
            state.maskMeshes[label] = mesh;
    }

    // Object meshes
    state.objectMeshes.clear();
    const QJsonObject objMeshFiles = assets[QStringLiteral("objectMeshes")].toObject();
    for (auto it = objMeshFiles.begin(); it != objMeshFiles.end(); ++it) {
        const int label = it.key().toInt();
        const QString meshPath = abs(it.value().toString());
        if (auto mesh = loadStl(meshPath))
            state.objectMeshes[label] = mesh;
    }

    // Special meshes
    auto loadAsset = [&](const QString& key) -> vtkSmartPointer<vtkPolyData> {
        if (!assets.contains(key)) return nullptr;
        return loadStl(abs(assets[key].toString()));
    };
    state.upperArchOriginalMesh = loadAsset(QStringLiteral("upperArchOriginal"));
    state.lowerArchOriginalMesh = loadAsset(QStringLiteral("lowerArchOriginal"));
    state.upperArchMesh      = loadAsset(QStringLiteral("upperArch"));
    state.lowerArchMesh      = loadAsset(QStringLiteral("lowerArch"));
    state.upperCompositeMesh = loadAsset(QStringLiteral("upperComposite"));
    state.lowerCompositeMesh = loadAsset(QStringLiteral("lowerComposite"));

    return true;
}
