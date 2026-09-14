// SplintBenchmark — timing of SplintHeightmapGenerator outside the UI.
//
//   SplintBenchmark --upper sup.stl --lower inf.stl --points puntos.json [--grid 0.2]
//                   [--repeat 3] [--no-thickness] [--csv salida.csv]
//   SplintBenchmark --synthetic-triangles 300000 [--repeat 3] [--max-total-ms 30000]
//
// The points file is the one written by "Exportar puntos" in the splint panel.

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#endif

#include "SplintDesignCore.h"
#include "SplintHeightmapGenerator.h"
#include "SplintTestGeometry.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <vtkCellArray.h>
#include <vtkCellArrayIterator.h>
#include <vtkCleanPolyData.h>
#include <vtkPoints.h>
#include <vtkSTLReader.h>
#include <vtkTriangleFilter.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <map>
#include <utility>
#include <vector>

namespace
{
double peakMemoryMb()
{
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS counters;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#endif
    return 0.0;
}

vtkSmartPointer<vtkPolyData> readStl(const QString& path)
{
    auto reader = vtkSmartPointer<vtkSTLReader>::New();
    reader->SetFileName(QFile::encodeName(path).constData());
    auto clean = vtkSmartPointer<vtkCleanPolyData>::New();
    clean->SetInputConnection(reader->GetOutputPort());
    clean->Update();
    auto mesh = vtkSmartPointer<vtkPolyData>::New();
    mesh->DeepCopy(clean->GetOutput());
    return mesh->GetNumberOfPolys() > 0 ? mesh : nullptr;
}

// Midpoint subdivision: keeps the geometry, shares edge midpoints between
// neighbouring triangles and multiplies the triangle count by 4.
vtkSmartPointer<vtkPolyData> subdivideOnce(vtkPolyData* mesh)
{
    auto triangles = vtkSmartPointer<vtkTriangleFilter>::New();
    triangles->SetInputData(mesh);
    triangles->Update();
    vtkPolyData* input = triangles->GetOutput();

    auto points = vtkSmartPointer<vtkPoints>::New();
    points->DeepCopy(input->GetPoints());
    auto polys = vtkSmartPointer<vtkCellArray>::New();
    std::map<std::pair<vtkIdType, vtkIdType>, vtkIdType> midpoints;
    const auto midpoint = [&](vtkIdType a, vtkIdType b) {
        const auto key = std::minmax(a, b);
        const auto it = midpoints.find(key);
        if (it != midpoints.end())
            return it->second;
        double pa[3], pb[3];
        points->GetPoint(a, pa);
        points->GetPoint(b, pb);
        const vtkIdType id = points->InsertNextPoint(0.5 * (pa[0] + pb[0]), 0.5 * (pa[1] + pb[1]), 0.5 * (pa[2] + pb[2]));
        midpoints.emplace(key, id);
        return id;
    };

    auto it = vtk::TakeSmartPointer(input->GetPolys()->NewIterator());
    for (it->GoToFirstCell(); !it->IsDoneWithTraversal(); it->GoToNextCell()) {
        vtkIdType npts = 0;
        const vtkIdType* ids = nullptr;
        it->GetCurrentCell(npts, ids);
        if (npts != 3)
            continue;
        const vtkIdType a = ids[0], b = ids[1], c = ids[2];
        const vtkIdType ab = midpoint(a, b), bc = midpoint(b, c), ca = midpoint(c, a);
        for (const auto& t : {std::array<vtkIdType, 3>{a, ab, ca}, {ab, b, bc}, {ca, bc, c}, {ab, bc, ca}})
            polys->InsertNextCell(3, t.data());
    }
    auto out = vtkSmartPointer<vtkPolyData>::New();
    out->SetPoints(points);
    out->SetPolys(polys);
    return out;
}

vtkSmartPointer<vtkPolyData> subdivideTo(vtkPolyData* mesh, vtkIdType minTriangles)
{
    vtkSmartPointer<vtkPolyData> current = mesh;
    while (current->GetNumberOfPolys() < minTriangles)
        current = subdivideOnce(current);
    return current;
}

bool readPoints(const QString& path, std::vector<SplintPoint3>& upper, std::vector<SplintPoint3>& lower,
                SplintHeightmapParams& params, QString* error)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        *error = QStringLiteral("No se pudo abrir %1").arg(path);
        return false;
    }
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const auto parse = [](const QJsonValue& value) {
        std::vector<SplintPoint3> points;
        for (const QJsonValue& item : value.toArray()) {
            const QJsonArray p = item.toArray();
            if (p.size() == 3)
                points.push_back({p[0].toDouble(), p[1].toDouble(), p[2].toDouble()});
        }
        return points;
    };
    upper = parse(root.value(QStringLiteral("upperPoints")));
    lower = parse(root.value(QStringLiteral("lowerPoints")));
    params = SplintDesignCore::ParamsFromJson(root.value(QStringLiteral("params")).toObject());
    if (upper.size() < 3 || lower.size() < 3) {
        *error = QStringLiteral("%1 debe tener al menos 3 puntos superiores y 3 inferiores.").arg(path);
        return false;
    }
    return true;
}

struct Stats
{
    double median = 0.0;
    double min = 0.0;
    double max = 0.0;
};

Stats stats(std::vector<double> values)
{
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    return {n % 2 ? values[n / 2] : 0.5 * (values[n / 2 - 1] + values[n / 2]), values.front(), values.back()};
}
} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Mide los tiempos del generador de férulas por mapas de altura."));
    parser.addHelpOption();
    const QCommandLineOption upperOption(QStringLiteral("upper"), QStringLiteral("STL de los dientes superiores."), QStringLiteral("stl"));
    const QCommandLineOption lowerOption(QStringLiteral("lower"), QStringLiteral("STL de los dientes inferiores."), QStringLiteral("stl"));
    const QCommandLineOption pointsOption(QStringLiteral("points"), QStringLiteral("JSON de \"Exportar puntos\"."), QStringLiteral("json"));
    const QCommandLineOption syntheticOption(QStringLiteral("synthetic-triangles"),
                                             QStringLiteral("Arco sintético con al menos N triángulos por arcada."), QStringLiteral("n"));
    const QCommandLineOption gridOption(QStringLiteral("grid"), QStringLiteral("Resolución de la rejilla en mm."), QStringLiteral("mm"));
    const QCommandLineOption repeatOption(QStringLiteral("repeat"), QStringLiteral("Repeticiones (3)."), QStringLiteral("n"), QStringLiteral("3"));
    const QCommandLineOption noThicknessOption(QStringLiteral("no-thickness"), QStringLiteral("No calcula el grosor."));
    const QCommandLineOption csvOption(QStringLiteral("csv"), QStringLiteral("Escribe los tiempos en CSV."), QStringLiteral("archivo"));
    const QCommandLineOption maxTotalOption(QStringLiteral("max-total-ms"),
                                            QStringLiteral("Falla si la mediana total supera este valor."), QStringLiteral("ms"));
    parser.addOptions({upperOption, lowerOption, pointsOption, syntheticOption, gridOption, repeatOption,
                       noThicknessOption, csvOption, maxTotalOption});
    parser.process(app);

    vtkSmartPointer<vtkPolyData> upper;
    vtkSmartPointer<vtkPolyData> lower;
    SplintHeightmapInputs inputs;
    if (parser.isSet(syntheticOption)) {
        const vtkIdType triangles = parser.value(syntheticOption).toLongLong();
        const splinttest::Scene scene = splinttest::makeScene();
        upper = subdivideTo(scene.upper, triangles);
        lower = subdivideTo(scene.lower, triangles);
        inputs.upperPoints = scene.inputs.upperPoints;
        inputs.lowerPoints = scene.inputs.lowerPoints;
    } else {
        if (!parser.isSet(upperOption) || !parser.isSet(lowerOption) || !parser.isSet(pointsOption)) {
            std::cerr << "Indique --upper, --lower y --points, o --synthetic-triangles.\n";
            return 2;
        }
        upper = readStl(parser.value(upperOption));
        lower = readStl(parser.value(lowerOption));
        if (!upper || !lower) {
            std::cerr << "No se pudieron leer los STL.\n";
            return 2;
        }
        QString error;
        if (!readPoints(parser.value(pointsOption), inputs.upperPoints, inputs.lowerPoints, inputs.params, &error)) {
            std::cerr << error.toStdString() << '\n';
            return 2;
        }
    }
    inputs.upperTeeth = upper;
    inputs.lowerTeeth = lower;
    if (parser.isSet(gridOption))
        inputs.params.gridResolutionMm = parser.value(gridOption).toDouble();
    inputs.params.computeThickness = !parser.isSet(noThicknessOption);
    const int repeat = std::max(1, parser.value(repeatOption).toInt());

    std::cout << "Triangulos: superior " << upper->GetNumberOfPolys() << ", inferior " << lower->GetNumberOfPolys()
              << " | rejilla " << inputs.params.gridResolutionMm << " mm | grosor "
              << (inputs.params.computeThickness ? "si" : "no") << '\n';

    std::vector<double> prepareMs, buildMs, totalMs;
    for (int run = 1; run <= repeat; ++run) {
        const auto t0 = std::chrono::steady_clock::now();
        const SplintHeightmapPrepared prepared = SplintHeightmapGenerator::Prepare(inputs);
        const auto t1 = std::chrono::steady_clock::now();
        if (!prepared.ok) {
            std::cerr << "Prepare fallo: " << prepared.error.toStdString() << '\n';
            return 2;
        }
        const SplintHeightmapResult result = SplintHeightmapGenerator::Build(prepared, inputs);
        const auto t2 = std::chrono::steady_clock::now();
        if (!result.ok) {
            std::cerr << "Build fallo: " << result.error.toStdString() << '\n';
            return 2;
        }
        prepareMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        buildMs.push_back(std::chrono::duration<double, std::milli>(t2 - t1).count());
        totalMs.push_back(prepareMs.back() + buildMs.back());
        std::cout << "Corrida " << run << ": Prepare " << prepareMs.back() << " ms, Build " << buildMs.back()
                  << " ms, total " << totalMs.back() << " ms, " << result.mesh->GetNumberOfPolys() << " triangulos\n";
        if (run == 1)
            std::cout << result.report.toStdString() << '\n';
    }

    const Stats p = stats(prepareMs), b = stats(buildMs), t = stats(totalMs);
    std::cout << "Mediana (min-max): Prepare " << p.median << " (" << p.min << "-" << p.max << ") ms, Build " << b.median
              << " (" << b.min << "-" << b.max << ") ms, total " << t.median << " (" << t.min << "-" << t.max
              << ") ms | memoria pico " << peakMemoryMb() << " MB\n";

    if (parser.isSet(csvOption)) {
        QFile csv(parser.value(csvOption));
        if (!csv.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            std::cerr << "No se pudo escribir el CSV.\n";
            return 2;
        }
        QTextStream out(&csv);
        out << "corrida,prepare_ms,build_ms,total_ms,triangulos_sup,triangulos_inf,rejilla_mm,grosor,memoria_pico_mb\n";
        for (size_t i = 0; i < totalMs.size(); ++i)
            out << (i + 1) << ',' << prepareMs[i] << ',' << buildMs[i] << ',' << totalMs[i] << ','
                << upper->GetNumberOfPolys() << ',' << lower->GetNumberOfPolys() << ','
                << inputs.params.gridResolutionMm << ',' << (inputs.params.computeThickness ? 1 : 0) << ','
                << peakMemoryMb() << '\n';
    }

    if (parser.isSet(maxTotalOption) && t.median > parser.value(maxTotalOption).toDouble()) {
        std::cerr << "La mediana total " << t.median << " ms supera el limite de " << parser.value(maxTotalOption).toStdString()
                  << " ms.\n";
        return 1;
    }
    return 0;
}
