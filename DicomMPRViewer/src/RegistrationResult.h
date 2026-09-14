#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// RegistrationResult  (Phase 5)
//
// Carries the quality metrics produced by alignUpperArchToMaxilla() /
// alignLowerArchToMandible() and supports JSON round-tripping so the metrics
// can be persisted in the project file.
//
// Metric computation (computeMetrics) uses a vtkCellLocator to find the
// nearest point on the target surface for every vertex of the source mesh,
// then derives mean / max / p95 distances in millimetres.
// ─────────────────────────────────────────────────────────────────────────────

#include <QJsonObject>
#include <QString>
#include <QVector>

class vtkPolyData;

struct RegistrationResult
{
    // ── Landmark (rigid body) RMSE ────────────────────────────────────────────
    double landmarkRms  = 0.0;   // mm — from the landmark transform step

    // ── Point-to-surface distances (post ICP, source → target) ───────────────
    double meanDistance = 0.0;   // mm
    double maxDistance  = 0.0;   // mm
    double p95Distance  = 0.0;   // mm (95th-percentile)

    // ── Accept / reject decision ──────────────────────────────────────────────
    bool accepted = false;

    // ── Transform (row-major 4x4, from final ComposeTransforms) ──────────────
    QVector<double> matrix;      // 16 elements, empty if not yet computed

    // ── Human-readable report (same string stored in m_upperRegistrationReport)
    QString report;

    // ── Helpers ───────────────────────────────────────────────────────────────

    // Returns true if the result contains meaningful metrics
    bool isValid() const { return landmarkRms >= 0.0 && meanDistance >= 0.0; }

    // Builds a short summary line suitable for a status bar or dialog
    QString summaryLine() const;

    // ── JSON round-trip ───────────────────────────────────────────────────────
    QJsonObject toJson()                        const;
    static RegistrationResult fromJson(const QJsonObject& obj);

    // ── Distance metric computation ───────────────────────────────────────────
    // Computes mean/max/p95 from every vertex of 'source' to the nearest cell
    // on 'target'.  Returns false if either mesh is null/empty.
    static bool computeMetrics(vtkPolyData* source,
                               vtkPolyData* target,
                               double& outMean,
                               double& outMax,
                               double& outP95);
};
