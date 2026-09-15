#include "ModelWorkflowCore.h"
#include <algorithm>

ModelWorkflowCore::State ModelWorkflowCore::Evaluate(const Input& input)
{
    State out;
    for (int j = 0; j < 2; ++j) {
        const auto& jaw = input.jaws[j];
        const bool paired = jaw.bonePoints >= 3 && jaw.bonePoints == jaw.dentalPoints;
        out.done[j * 4] = jaw.accepted || jaw.dental;
        out.done[j * 4 + 1] = jaw.accepted || jaw.registered || paired;
        out.done[j * 4 + 2] = jaw.accepted || (jaw.registered && !(input.jaw == j && input.adjusting));
        out.done[j * 4 + 3] = jaw.accepted;
    }
    if (input.jaws[0].accepted && input.jaws[1].accepted) {
        out.current = -1;
        out.instruction = QStringLiteral("Ambos compuestos aceptados. Continúe con Orientación.");
        return out;
    }
    const int j = std::clamp(input.jaw, 0, 1);
    const auto& jaw = input.jaws[j];
    const QString bone = j == 0 ? QStringLiteral("maxilar") : QStringLiteral("mandíbula");
    const QString arch = j == 0 ? QStringLiteral("STL superior") : QStringLiteral("STL inferior");
    out.current = j * 4;
    out.points = QStringLiteral("Puntos: %1 %2 · %3 %4")
        .arg(bone).arg(jaw.bonePoints).arg(arch).arg(jaw.dentalPoints);
    if (jaw.accepted) {
        out.current = -1;
        out.instruction = QStringLiteral("Compuesto aceptado. Continúe con la otra arcada.");
        return out;
    }
    if (!jaw.bone) {
        out.instruction = QStringLiteral("Falta el %1 del TAC. Genere su máscara u objeto en Segmentación.").arg(bone);
        return out;
    }
    if (!jaw.dental) {
        out.instruction = QStringLiteral("Cargue el %1, o use Continuar sin match si no dispone de escaneo.").arg(arch);
        return out;
    }
    const bool idle = input.phase == Phase::Registration && !input.adjusting;
    const bool paired = jaw.bonePoints >= 3 && jaw.bonePoints == jaw.dentalPoints;
    out.canCapture = idle && !jaw.registered;
    out.canRegister = out.canCapture && paired;
    out.canAdjust = idle && jaw.registered;
    out.canBuild = idle && jaw.registered;
    if (input.phase != Phase::Registration) {
        out.current += 3;
        switch (input.phase) {
        case Phase::Block: out.instruction = QStringLiteral("Rodee el escaneo con puntos (o ajuste el bloque) y pulse Calcular compuesto."); break;
        case Phase::Computing: out.instruction = QStringLiteral("Calculando compuesto. Todavía no está aceptado."); break;
        case Phase::Review: out.instruction = QStringLiteral("Revise el resultado. Aceptar compuesto lo guarda; Atrás permite corregir el bloque."); break;
        default: break;
        }
    } else if (input.adjusting) {
        out.current += 2;
        out.instruction = QStringLiteral("Ajuste el %1 y pulse Aceptar ajuste antes de continuar.").arg(arch);
    } else if (jaw.registered) {
        out.current += 3;
        out.instruction = QStringLiteral("Registro listo. Puede hacer un ajuste fino o crear el compuesto para definir el bloque.");
    } else {
        out.current += paired ? 2 : 1;
        out.nextTarget = jaw.bonePoints <= jaw.dentalPoints ? PointTarget::Bone : PointTarget::Dental;
        const auto active = input.capture == PointTarget::None ? out.nextTarget : input.capture;
        const int number = (active == PointTarget::Bone ? jaw.bonePoints : jaw.dentalPoints) + 1;
        out.points += QStringLiteral(" · %1punto %2 en %3")
            .arg(input.capture == PointTarget::None ? QStringLiteral("Siguiente: ") : QStringLiteral("Capturando: "))
            .arg(number).arg(active == PointTarget::Bone ? bone : arch);
        out.instruction = paired
            ? QStringLiteral("Pares completos. Pulse Registrar; después podrá ajustar la posición.")
            : QStringLiteral("Coloque puntos homólogos en el mismo orden: mínimo 3 por superficie y cantidades iguales.");
    }
    return out;
}
