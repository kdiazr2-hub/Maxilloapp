#include "AISegmentationService.h"
#include "SegmentationProgressCore.h"

#include <QCoreApplication>
#include <QStringList>

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

// Exposes the run lifecycle of the base service (no process involved).
class FakeService : public AISegmentationService
{
public:
    FakeService() : AISegmentationService(nullptr) {}
    void segment(vtkSmartPointer<vtkImageData>, const QString&, SegmentationTarget) override { beginRun(); }
    void cancel() override
    {
        if (runActive())
            m_cancelRequested = true;
    }
    void output(const QByteArray& data, bool flush = false) { publishProcessProgress(data, false, flush); }
    void fail(const QString& error) { failRun(error); }
    void succeed(const QString& path) { succeedRun(path); }
    void cancelledNow() { finishCancelled(); }
};

void testParser()
{
    SegmentationProgressCore core;
    auto updates = core.feed(QByteArray("30% Convirtiendo NRRD"));
    require(updates.empty(), "an incomplete line was parsed");
    updates = core.feed(QByteArray(" a NIfTI\n45%|#####     | 5/10 [00:01<00:01]\n"
                                   "MAXILLO_PROGRESS {\"percent\": 60, \"stage\": \"Inferencia\"}\n"));
    require(updates.size() == 2, "expected the milestone and the JSON progress, not the progress bar");
    require(updates[0].percent == 30 && updates[0].stage == QStringLiteral("Convirtiendo NRRD a NIfTI"),
            "legacy milestone split across reads not joined");
    require(updates[1].percent == 60 && updates[1].stage == QStringLiteral("Inferencia"), "JSON progress not parsed");

    const QByteArray line = QStringLiteral("90% Remapeando etiquetas óseas\n").toUtf8();
    const int cut = line.indexOf(QStringLiteral("ó").toUtf8()) + 1; // inside the two-byte character
    require(core.feed(line.left(cut)).empty(), "half a UTF-8 character produced an update");
    updates = core.feed(line.mid(cut));
    require(updates.size() == 1 && updates[0].stage == QStringLiteral("Remapeando etiquetas óseas"),
            "UTF-8 character split across reads was corrupted");

    updates = core.feed(QByteArray("100% Labelmap guardado"), true);
    require(updates.size() == 1 && updates[0].percent == 100, "flush did not parse the last line");

    updates = core.feed(QByteArray("MAXILLO_PROGRESS {bad}\n150% nope\nERROR: algo\n"
                                   "MAXILLO_PROGRESS {\"percent\": 101, \"stage\": \"x\"}\n"));
    require(updates.empty(), "invalid progress lines were accepted");

    require(SegmentationProgressCore::elapsedText(3723000) == QStringLiteral("01:02:03"), "elapsed time format");
    require(SegmentationProgressCore::elapsedText(-5) == QStringLiteral("00:00:00"), "negative elapsed time");
}

void testServiceLifecycle()
{
    FakeService service;
    int finished = 0, failed = 0, cancelled = 0;
    std::vector<int> percents;
    QStringList stages;
    QObject::connect(&service, &AISegmentationService::segmentationFinished, [&](const QString&) { ++finished; });
    QObject::connect(&service, &AISegmentationService::errorOccurred, [&](const QString&) { ++failed; });
    QObject::connect(&service, &AISegmentationService::cancelled, [&] { ++cancelled; });
    QObject::connect(&service, &AISegmentationService::progressChanged, [&](int p) { percents.push_back(p); });
    QObject::connect(&service, &AISegmentationService::statusChanged, [&](const QString& s) { stages << s; });

    service.segment(vtkSmartPointer<vtkImageData>(), QString(), SegmentationTarget::Bone);
    require(service.runActive(), "run did not start");
    service.output(QByteArray("20% Exportando\n"));
    service.succeed(QStringLiteral("out.nrrd"));
    require(finished == 1 && !service.runActive(), "successful run not reported once");
    require(percents == std::vector<int>{20} && stages == QStringList{QStringLiteral("Exportando")},
            "milestone not forwarded as progress and stage");
    service.succeed(QStringLiteral("out.nrrd"));
    service.output(QByteArray("50% tarde\n"));
    require(finished == 1 && percents.size() == 1, "an inactive run still reported results");

    service.segment(vtkSmartPointer<vtkImageData>(), QString(), SegmentationTarget::Bone);
    service.cancel();
    service.output(QByteArray("50% ignorado\n"));
    service.fail(QStringLiteral("proceso detenido"));
    require(cancelled == 1 && failed == 0 && percents.size() == 1,
            "a cancelled run reported an error or progress instead of cancelled");

    service.segment(vtkSmartPointer<vtkImageData>(), QString(), SegmentationTarget::Bone);
    service.fail(QStringLiteral("boom"));
    require(failed == 1 && !service.runActive(), "failed run not reported");

    service.segment(vtkSmartPointer<vtkImageData>(), QString(), SegmentationTarget::Bone);
    service.cancel();
    service.cancelledNow();
    service.cancelledNow();
    require(cancelled == 2, "cancellation reported more than once");
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"progress parser", testParser},
        {"service lifecycle", testServiceLifecycle},
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
