#include "AnnotationTool.h"

#include <QInputDialog>

std::optional<Measurement> AnnotationTool::addPoint(
    const std::array<double, 3>& point,
    MeasurementViewOrientation view,
    int sliceIndex,
    QWidget* parent)
{
    appendPointOnSamePlane(point, view, sliceIndex);

    bool ok = false;
    const QString text = QInputDialog::getText(
        parent,
        "Anotacion",
        "Texto:",
        QLineEdit::Normal,
        QString(),
        &ok);

    if (!ok || text.trimmed().isEmpty()) {
        reset();
        return std::nullopt;
    }

    Measurement measurement = makeMeasurement(MeasurementType::Annotation);
    measurement.text = text.trimmed().toStdString();
    reset();
    return measurement;
}
