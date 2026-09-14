#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// SplintPreviewScheduler
//
// Runs SplintHeightmapGenerator in the background for the live splint preview.
// Requests are debounced; a newer request cancels the running job and stale
// results are never emitted. The last successful Prepare() is cached, so
// changes that only affect Build() (edge offset, fillet, clearance, contour…)
// skip the ray casting. Two Prepare() results are kept so a coarse preview and
// its refined version do not evict each other.
//
// The meshes passed to request() are shared with the worker: callers must not
// modify them in place (replace them instead). The cache key includes each
// mesh pointer and MTime.
// ─────────────────────────────────────────────────────────────────────────────

#include "SplintHeightmapGenerator.h"

#include <QFutureWatcher>
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QTimer>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

class SplintPreviewScheduler : public QObject
{
    Q_OBJECT

public:
    explicit SplintPreviewScheduler(QObject* parent = nullptr);
    ~SplintPreviewScheduler() override;

    void setDebounceMs(int ms);
    // inputs.upperTeeth / lowerTeeth / cancel are replaced by the scheduler.
    quint64 request(vtkSmartPointer<vtkPolyData> upperTeeth, vtkSmartPointer<vtkPolyData> lowerTeeth,
                    const SplintHeightmapInputs& inputs);
    void cancel();
    bool isBusy() const;

    int prepareRuns() const { return m_prepareRuns.load(); }
    int buildRuns() const { return m_buildRuns.load(); }

    static QString PrepareKey(vtkPolyData* upperTeeth, vtkPolyData* lowerTeeth, const SplintHeightmapInputs& inputs);

signals:
    void previewStarted(quint64 generation);
    void previewReady(quint64 generation, const SplintHeightmapResult& result);
    void previewFailed(quint64 generation, const QString& error);

private:
    struct Job
    {
        quint64 generation = 0;
        vtkSmartPointer<vtkPolyData> upper;
        vtkSmartPointer<vtkPolyData> lower;
        SplintHeightmapInputs inputs;
    };
    struct Outcome
    {
        quint64 generation = 0;
        bool cancelled = false;
        SplintHeightmapResult result;
    };

    void startPending();
    void onJobFinished();
    Outcome run(Job job, const std::shared_ptr<std::atomic<bool>>& cancelFlag);

    QTimer m_debounce;
    QFutureWatcher<Outcome> m_watcher;
    std::optional<Job> m_pending;
    quint64 m_nextGeneration = 0;
    quint64 m_latestGeneration = 0;
    bool m_running = false;
    std::shared_ptr<std::atomic<bool>> m_runningCancel;

    static constexpr size_t kPreparedCacheSize = 2;
    std::mutex m_cacheMutex;
    std::deque<std::pair<QString, std::shared_ptr<const SplintHeightmapPrepared>>> m_preparedCache; // newest first
    std::atomic<int> m_prepareRuns{0};
    std::atomic<int> m_buildRuns{0};
};

Q_DECLARE_METATYPE(SplintHeightmapResult)
