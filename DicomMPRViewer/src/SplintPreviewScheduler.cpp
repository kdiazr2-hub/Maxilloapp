#include "SplintPreviewScheduler.h"

#include <QtConcurrent/QtConcurrentRun>

#include <vtkPolyData.h>

SplintPreviewScheduler::SplintPreviewScheduler(QObject* parent)
    : QObject(parent)
{
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(250);
    connect(&m_debounce, &QTimer::timeout, this, &SplintPreviewScheduler::startPending);
    connect(&m_watcher, &QFutureWatcher<Outcome>::finished, this, &SplintPreviewScheduler::onJobFinished);
}

SplintPreviewScheduler::~SplintPreviewScheduler()
{
    cancel();
    if (m_running)
        m_watcher.waitForFinished();
}

void SplintPreviewScheduler::setDebounceMs(int ms)
{
    m_debounce.setInterval(std::max(0, ms));
}

quint64 SplintPreviewScheduler::request(vtkSmartPointer<vtkPolyData> upperTeeth,
                                        vtkSmartPointer<vtkPolyData> lowerTeeth,
                                        const SplintHeightmapInputs& inputs)
{
    Job job;
    job.generation = ++m_nextGeneration;
    job.upper = upperTeeth;
    job.lower = lowerTeeth;
    job.inputs = inputs;
    job.inputs.upperTeeth = upperTeeth;
    job.inputs.lowerTeeth = lowerTeeth;
    job.inputs.cancel = nullptr;

    m_latestGeneration = job.generation;
    m_pending = std::move(job);
    if (m_runningCancel)
        m_runningCancel->store(true);
    m_debounce.start();
    return m_latestGeneration;
}

void SplintPreviewScheduler::cancel()
{
    m_debounce.stop();
    m_pending.reset();
    if (m_runningCancel)
        m_runningCancel->store(true);
    m_latestGeneration = ++m_nextGeneration; // nothing issued so far may be emitted
}

bool SplintPreviewScheduler::isBusy() const
{
    return m_running || m_pending.has_value();
}

QString SplintPreviewScheduler::PrepareKey(vtkPolyData* upperTeeth, vtkPolyData* lowerTeeth,
                                           const SplintHeightmapInputs& inputs)
{
    QString key;
    const auto addMesh = [&key](vtkPolyData* mesh) {
        if (!mesh) {
            key += QStringLiteral("null;");
            return;
        }
        key += QStringLiteral("%1/%2/%3/%4;")
                   .arg(static_cast<quintptr>(reinterpret_cast<quintptr>(mesh)))
                   .arg(static_cast<qulonglong>(mesh->GetMTime()))
                   .arg(static_cast<qlonglong>(mesh->GetNumberOfPoints()))
                   .arg(static_cast<qlonglong>(mesh->GetNumberOfPolys()));
    };
    const auto addNumber = [&key](double value) {
        key += QString::number(value, 'g', 12);
        key += QLatin1Char(',');
    };
    const auto addPoints = [&](const std::vector<SplintPoint3>& points) {
        for (const SplintPoint3& p : points)
            for (double c : p)
                addNumber(c);
        key += QLatin1Char('|');
    };

    addMesh(upperTeeth);
    addMesh(lowerTeeth);
    addPoints(inputs.upperPoints);
    addPoints(inputs.lowerPoints);
    const SplintHeightmapParams& p = inputs.params;
    addNumber(p.gridResolutionMm);
    addNumber(p.undercutUpper ? 1.0 : 0.0);
    addNumber(p.undercutLower ? 1.0 : 0.0);
    addNumber(p.undercutAngleUpperDeg);
    addNumber(p.undercutAngleLowerDeg);
    addNumber(p.undercutDirectionUV[0]);
    addNumber(p.undercutDirectionUV[1]);
    if (inputs.anteriorDirectionWorld)
        for (double c : *inputs.anteriorDirectionWorld)
            addNumber(c);
    return key;
}

void SplintPreviewScheduler::startPending()
{
    if (!m_pending || m_running)
        return; // a running job restarts the queue when it finishes

    Job job = std::move(*m_pending);
    m_pending.reset();
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    m_runningCancel = cancelFlag;
    m_running = true;
    const quint64 generation = job.generation;
    m_watcher.setFuture(QtConcurrent::run([this, job = std::move(job), cancelFlag]() mutable {
        return run(std::move(job), cancelFlag);
    }));
    emit previewStarted(generation);
}

SplintPreviewScheduler::Outcome SplintPreviewScheduler::run(Job job, const std::shared_ptr<std::atomic<bool>>& cancelFlag)
{
    Outcome outcome;
    outcome.generation = job.generation;
    job.inputs.cancel = cancelFlag.get();
    if (cancelFlag->load()) {
        outcome.cancelled = true;
        return outcome;
    }

    const QString key = PrepareKey(job.upper, job.lower, job.inputs);
    std::shared_ptr<const SplintHeightmapPrepared> prepared;
    {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        for (const auto& [cachedKey, cached] : m_preparedCache) {
            if (cachedKey == key) {
                prepared = cached;
                break;
            }
        }
    }
    if (!prepared) {
        ++m_prepareRuns;
        auto fresh = std::make_shared<SplintHeightmapPrepared>(SplintHeightmapGenerator::Prepare(job.inputs));
        if (cancelFlag->load()) {
            outcome.cancelled = true;
            return outcome;
        }
        if (fresh->ok) {
            std::lock_guard<std::mutex> lock(m_cacheMutex);
            m_preparedCache.emplace_front(key, fresh);
            while (m_preparedCache.size() > kPreparedCacheSize)
                m_preparedCache.pop_back();
        }
        prepared = fresh;
    }
    if (!prepared->ok) {
        outcome.result.error = prepared->error;
        return outcome;
    }

    ++m_buildRuns;
    outcome.result = SplintHeightmapGenerator::Build(*prepared, job.inputs);
    outcome.cancelled = cancelFlag->load();
    return outcome;
}

void SplintPreviewScheduler::onJobFinished()
{
    m_running = false;
    m_runningCancel.reset();
    const Outcome outcome = m_watcher.result();
    if (!outcome.cancelled && outcome.generation == m_latestGeneration) {
        if (outcome.result.ok)
            emit previewReady(outcome.generation, outcome.result);
        else
            emit previewFailed(outcome.generation, outcome.result.error);
    }
    if (m_pending && !m_debounce.isActive())
        startPending();
}
