#include <QApplication>
#include <QColor>
#include <QFont>
#include <QIcon>
#include <QMetaType>
#include <QPainter>
#include <QPixmap>
#include <QSplashScreen>
#include <QSurfaceFormat>
#include <QTimer>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkImageData.h>
#include <vtkSmartPointer.h>

#include "AsyncDicomLoader.h"    // Q_DECLARE_METATYPE guards
#include "DicomSeriesIndexer.h"
#include "DicomVolumeLoader.h"
#include "MainWindow.h"

static QPixmap buildSplashPixmap()
{
    QPixmap splash(620, 360);
    splash.fill(QColor(30, 30, 30)); // macOS Dark Mode background

    QPainter painter(&splash);
    painter.setRenderHint(QPainter::Antialiasing, true);

    // Premium clean text
    painter.setPen(QColor(255, 255, 255));
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 22, QFont::Bold));
    painter.drawText(QRect(40, 130, splash.width() - 80, 50),
                     Qt::AlignCenter,
                     QString::fromUtf8("Planificación Maxilofacial"));

    painter.setPen(QColor(142, 142, 147)); // Apple muted gray
    painter.setFont(QFont(QStringLiteral("Segoe UI"), 11, QFont::Medium));
    painter.drawText(QRect(24, splash.height() - 50, splash.width() - 48, 28),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QString::fromUtf8("Creado por Kevin Díaz"));

    painter.end();
    return splash;
}

int main(int argc, char* argv[])
{
    // ── VTK / Qt OpenGL bootstrap ──────────────────────────────────────────
    // This MUST happen before QApplication is constructed.
    // QVTKOpenGLNativeWidget requires OpenGL 3.2 Core Profile.
    // Calling defaultFormat() also enables multisampling (4x MSAA) and a
    // 24-bit depth buffer, which VTK's renderer expects.
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());

    // ── Qt application ─────────────────────────────────────────────────────
    QApplication app(argc, argv);
    QFont defaultFont(QStringLiteral("Segoe UI"), 10);
    app.setFont(defaultFont);
    app.setApplicationName("PlanificacionMaxilofacial");
    app.setApplicationDisplayName("Planificación Maxilofacial");
    app.setApplicationVersion("1.0.0");
    app.setOrganizationName("PlanificacionMaxilofacial");
    app.setOrganizationDomain("planificacionmaxilofacial.local");

    QSplashScreen splash(buildSplashPixmap());
    splash.show();
    app.processEvents();

    // ── Meta-type registration for cross-thread signals ─────────────────────
    // Must be done after QApplication construction and before any threads start.
    qRegisterMetaType<vtkSmartPointer<vtkImageData>>("vtkSmartPointer<vtkImageData>");
    qRegisterMetaType<DicomVolumeLoader::VolumeMetadata>("DicomVolumeLoader::VolumeMetadata");
    qRegisterMetaType<SeriesInfo>("SeriesInfo");
    qRegisterMetaType<QVector<SeriesInfo>>("QVector<SeriesInfo>");

    // ── Main window ────────────────────────────────────────────────────────
    MainWindow window;
    QTimer::singleShot(1100, &window, [&splash, &window] {
        window.show();
        splash.finish(&window);
    });

    return app.exec();
}
