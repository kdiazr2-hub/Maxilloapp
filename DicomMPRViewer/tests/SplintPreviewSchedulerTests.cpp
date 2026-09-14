#include "SplintPreviewScheduler.h"
#include "SplintTestGeometry.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>

#include <algorithm>
#include <functional>
#include <iostream>
#include <vector>

namespace
{
using namespace splinttest;

bool waitFor(const std::function<bool()>& condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    return true;
}

void settle(int ms)
{
    waitFor([] { return false; }, ms);
}

struct Recorder
{
    std::vector<quint64> started;
    std::vector<quint64> ready;
    std::vector<quint64> failed;
    QStringList errors;
    bool lastReadyHasMesh = false;

    void attach(SplintPreviewScheduler& scheduler)
    {
        QObject::connect(&scheduler, &SplintPreviewScheduler::previewStarted,
                         [this](quint64 g) { started.push_back(g); });
        QObject::connect(&scheduler, &SplintPreviewScheduler::previewReady,
                         [this](quint64 g, const SplintHeightmapResult& r) {
                             ready.push_back(g);
                             lastReadyHasMesh = r.ok && r.mesh && r.mesh->GetNumberOfPolys() > 0;
                         });
        QObject::connect(&scheduler, &SplintPreviewScheduler::previewFailed,
                         [this](quint64 g, const QString& e) {
                             failed.push_back(g);
                             errors.push_back(e);
                         });
    }
};

void waitIdle(SplintPreviewScheduler& scheduler, const std::string& what)
{
    require(waitFor([&] { return !scheduler.isBusy(); }, 60000), what + ": scheduler never became idle");
    settle(150);
}

void testBurstCacheAndRebuild()
{
    Scene scene = makeScene();
    SplintPreviewScheduler scheduler;
    scheduler.setDebounceMs(60);
    Recorder rec;
    rec.attach(scheduler);

    quint64 last = 0;
    for (int i = 0; i < 5; ++i) {
        scene.inputs.params.edgeOffsetMm = 1.0 + 0.1 * i;
        last = scheduler.request(scene.upper, scene.lower, scene.inputs);
    }
    waitIdle(scheduler, "burst");
    require(rec.ready.size() == 1 && rec.ready[0] == last, "a burst did not produce exactly the last preview");
    require(rec.lastReadyHasMesh, "preview has no mesh");
    require(scheduler.prepareRuns() == 1 && scheduler.buildRuns() == 1, "burst ran more than one job");

    scene.inputs.params.filletMm = 0.5; // Build-only change
    last = scheduler.request(scene.upper, scene.lower, scene.inputs);
    waitIdle(scheduler, "fillet change");
    require(rec.ready.size() == 2 && rec.ready.back() == last, "fillet change produced no preview");
    require(scheduler.prepareRuns() == 1 && scheduler.buildRuns() == 2, "fillet change repeated Prepare");

    scene.inputs.upperPoints[0][2] += 0.5; // Prepare change
    last = scheduler.request(scene.upper, scene.lower, scene.inputs);
    waitIdle(scheduler, "point change");
    require(rec.ready.size() == 3 && rec.ready.back() == last, "point change produced no preview");
    require(scheduler.prepareRuns() == 2, "point change reused a stale Prepare");
    require(rec.failed.empty(), "unexpected failure: " + (rec.errors.isEmpty() ? std::string() : rec.errors.front().toStdString()));
}

void testCacheKeepsCoarseAndFinePrepare()
{
    Scene scene = makeScene();
    SplintPreviewScheduler scheduler;
    scheduler.setDebounceMs(0);
    Recorder rec;
    rec.attach(scheduler);
    for (int round = 0; round < 2; ++round) {
        for (double grid : {0.4, 0.3}) {
            scene.inputs.params.gridResolutionMm = grid;
            scheduler.request(scene.upper, scene.lower, scene.inputs);
            waitIdle(scheduler, "alternating resolution");
        }
    }
    require(rec.ready.size() == 4, "alternating resolutions did not produce four previews");
    require(scheduler.prepareRuns() == 2 && scheduler.buildRuns() == 4,
            "coarse and fine Prepare evicted each other: " + std::to_string(scheduler.prepareRuns()));
}

void testCancelBeforeStart()
{
    Scene scene = makeScene();
    SplintPreviewScheduler scheduler;
    scheduler.setDebounceMs(100);
    Recorder rec;
    rec.attach(scheduler);
    scheduler.request(scene.upper, scene.lower, scene.inputs);
    scheduler.cancel();
    settle(500);
    require(!scheduler.isBusy() && rec.started.empty() && rec.ready.empty() && rec.failed.empty(),
            "cancelled request still ran");
}

void testNewerRequestSupersedesRunningJob()
{
    Scene scene = makeScene();
    scene.inputs.params.gridResolutionMm = 0.2;
    SplintPreviewScheduler scheduler;
    scheduler.setDebounceMs(0);
    Recorder rec;
    rec.attach(scheduler);

    SplintHeightmapInputs changed = scene.inputs;
    changed.params.filletMm = 0.5;
    quint64 second = 0;
    QObject::connect(&scheduler, &SplintPreviewScheduler::previewStarted, [&](quint64) {
        if (second == 0)
            second = scheduler.request(scene.upper, scene.lower, changed);
    });
    const quint64 first = scheduler.request(scene.upper, scene.lower, scene.inputs);
    require(waitFor([&] { return !rec.ready.empty(); }, 60000), "superseding request produced no preview");
    waitIdle(scheduler, "supersede");

    require(std::find(rec.started.begin(), rec.started.end(), first) != rec.started.end(), "first job never started");
    require(rec.ready.size() == 1 && rec.ready[0] == second, "stale result was emitted");
}

void testFailureReported()
{
    Scene scene = makeScene();
    scene.inputs.upperPoints.pop_back();
    SplintPreviewScheduler scheduler;
    scheduler.setDebounceMs(0);
    Recorder rec;
    rec.attach(scheduler);
    const quint64 generation = scheduler.request(scene.upper, scene.lower, scene.inputs);
    waitIdle(scheduler, "failure");
    require(rec.failed.size() == 1 && rec.failed[0] == generation && rec.ready.empty(), "failure not emitted");
    require(rec.errors.front().contains(QStringLiteral("Faltan puntos superiores")), "wrong failure message");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"burst, cache and rebuild", testBurstCacheAndRebuild},
        {"cache keeps coarse and fine prepare", testCacheKeepsCoarseAndFinePrepare},
        {"cancel before start", testCancelBeforeStart},
        {"newer request supersedes running job", testNewerRequestSupersedesRunningJob},
        {"failure reported", testFailureReported},
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
