#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// ProjectSerializer
//
// Saves and loads a complete MaxilloApp project to/from a single
// .maxilloproject JSON file plus a sibling <name>_assets/ directory that
// holds the labelmap and STL meshes.
//
// Format:
//   MyProject.maxilloproject       ← JSON index
//   MyProject_assets/
//     labelmap.nrrd                ← segmentation labelmap
//     mesh_<label>.stl             ← per-label bone/tissue meshes (masks)
//     obj_<label>.stl              ← calculated objects (arches, composites)
//
// The JSON references all asset paths RELATIVE to the .maxilloproject file
// so the whole folder can be moved / shared.
// ─────────────────────────────────────────────────────────────────────────────

#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QVector>
#include <QVector3D>

#include <vtkImageData.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

// ── Per-entry descriptions for the mask / object tables ───────────────────────
struct ProjMaskEntry
{
    int     label   = -1;
    QString name;
    QColor  color;
    bool    visible = true;
};

struct ProjObjectEntry
{
    int     label   = -1;   // semantic label (kUpperArchLabel, etc.)
    QString name;
    QColor  color;
    bool    visible = true;
    double  opacity = -1.0; // Negative keeps the workspace default for older projects.
    bool    alwaysOnTop = false;
};

// ── Everything the serializer saves / loads ────────────────────────────────────
struct ProjMandibleMovement
{
    int targetLabel = -1;
    QVector<double> referenceCenter; // Same segment, before bite registration, in the oriented frame.
    QVector<double> registrationMatrix;
    QVector<double> currentMatrix;
};

struct ProjectState
{
    ProjMandibleMovement mandibleMovement;
    // ── Provenance ────────────────────────────────────────────────────────────
    QString dicomFolder;              // path used to load the volume

    // ── Window / level ────────────────────────────────────────────────────────
    double presetWindow = 2000.0;
    double presetLevel  =  500.0;

    // ── Tables ────────────────────────────────────────────────────────────────
    QVector<ProjMaskEntry>   masks;
    QVector<ProjObjectEntry> objects;
    QVector<int>             hiddenMaskLabels;

    // ── Registration points ───────────────────────────────────────────────────
    QVector<QVector3D> maxillaBonePoints;
    QVector<QVector3D> upperArchPoints;
    QVector<QVector3D> mandibleBonePoints;
    QVector<QVector3D> lowerArchPoints;

    // ── Volume data (populated by load, must be filled for save) ─────────────
    vtkSmartPointer<vtkImageData>              labelmap;
    QMap<int, vtkSmartPointer<vtkPolyData>>    maskMeshes;   // label → mesh
    QMap<int, vtkSmartPointer<vtkPolyData>>    objectMeshes; // label → mesh
    vtkSmartPointer<vtkPolyData>               upperArchOriginalMesh;
    vtkSmartPointer<vtkPolyData>               lowerArchOriginalMesh;
    vtkSmartPointer<vtkPolyData>               upperArchMesh;
    vtkSmartPointer<vtkPolyData>               lowerArchMesh;
    vtkSmartPointer<vtkPolyData>               upperCompositeMesh;
    vtkSmartPointer<vtkPolyData>               lowerCompositeMesh;

    QVector<double> upperRegistrationMatrix; // row-major 4x4
    QVector<double> lowerRegistrationMatrix; // row-major 4x4
    QString         upperRegistrationReport;
    QString         lowerRegistrationReport;
    bool            upperRegistrationCalculated = false;
    bool            lowerRegistrationCalculated = false;

    // ── Registration quality metrics (Phase 5) ────────────────────────────────
    double upperLandmarkRms  = 0.0;
    double upperMeanDist     = 0.0;
    double upperP95Dist      = 0.0;
    double lowerLandmarkRms  = 0.0;
    double lowerMeanDist     = 0.0;
    double lowerP95Dist      = 0.0;

    // ── Splint designs (SplintDesignCore JSON; empty in older projects) ───────
    QJsonArray splintDesigns;
    QString    activeSplintDesignId;

    // ── Composite cutting blocks {"upper": block, "lower": block} (optional) ───
    QJsonObject compositeBlocks;

    // ── Osteotomy plan: type, landmarks and properties (optional) ─────────────
    QJsonObject osteotomyPlan;
};

class ProjectSerializer
{
public:
    // Save state → projectFilePath (e.g. "C:/Projects/MyProject.maxilloproject")
    // Returns true on success; fills *error on failure.
    static bool save(const QString& projectFilePath,
                     const ProjectState& state,
                     QString* error = nullptr);

    // Load state ← projectFilePath
    // Returns true on success; fills *error on failure.
    static bool load(const QString& projectFilePath,
                     ProjectState& state,
                     QString* error = nullptr);

private:
    static QString assetsDir(const QString& projectFilePath);
};
