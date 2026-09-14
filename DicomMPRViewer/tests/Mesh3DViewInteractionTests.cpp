#include "Mesh3DView.h"

#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkCellArray.h>
#include <vtkCubeSource.h>
#include <vtkPoints.h>
#include <vtkPolyData.h>
#include <vtkSmartPointer.h>

#include <array>
#include <cmath>
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

void settle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}

void mouse(QWidget* widget, QEvent::Type type, const QPointF& position, Qt::MouseButton button,
           Qt::MouseButtons buttons, Qt::KeyboardModifiers modifiers = Qt::NoModifier)
{
    QMouseEvent event(type, position, widget->mapToGlobal(position), button, buttons, modifiers);
    QApplication::sendEvent(widget, &event);
    settle();
}

void click(QWidget* widget, const QPointF& position, Qt::MouseButton button)
{
    mouse(widget, QEvent::MouseButtonPress, position, button, button);
    mouse(widget, QEvent::MouseButtonRelease, position, button, Qt::NoButton);
}

struct PointEvent
{
    int group = -1;
    int index = -1;
    double x = 0.0, y = 0.0, z = 0.0;
};

struct PlaneEvent
{
    double x = 0.0, y = 0.0, z = 0.0, deltaY = 0.0;
    Qt::KeyboardModifiers modifiers;
};

void testRayPlaneIntersection()
{
    std::array<double, 3> hit{};
    require(Mesh3DView::RayPlaneIntersection({1, 2, 10}, {1, 2, -10}, {0, 0, 3}, {0, 0, 1}, hit) &&
                std::abs(hit[0] - 1) < 1e-12 && std::abs(hit[1] - 2) < 1e-12 && std::abs(hit[2] - 3) < 1e-12,
            "vertical ray did not hit the plane at the right point");
    require(Mesh3DView::RayPlaneIntersection({0, 0, 0}, {1, 0, 1}, {0, 0, 2}, {0, 0, 1}, hit) &&
                std::abs(hit[0] - 2) < 1e-12 && std::abs(hit[2] - 2) < 1e-12,
            "oblique ray did not hit the plane at the right point");
    require(!Mesh3DView::RayPlaneIntersection({0, 0, 0}, {1, 0, 0}, {0, 0, 2}, {0, 0, 1}, hit),
            "ray parallel to the plane reported a hit");
}

void testInteraction()
{
    Mesh3DView view;
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(520, 520);
    view.show();
    settle();

    auto cube = vtkSmartPointer<vtkCubeSource>::New();
    cube->SetXLength(60.0);
    cube->SetYLength(60.0);
    cube->SetZLength(10.0);
    cube->Update();
    auto box = vtkSmartPointer<vtkPolyData>::New();
    box->DeepCopy(cube->GetOutput());
    view.addMesh(1, box, "Caja");
    view.setMeshColor(1, QColor(150, 150, 150));
    const auto lookDown = [&] {
        view.setViewAlongDirection({0, 0, 0}, {0, 0, -1}, {0, 1, 0}, 40.0);
        settle();
    };
    lookDown();

    auto* vtkWidget = view.findChild<QVTKOpenGLNativeWidget*>();
    require(vtkWidget != nullptr, "missing VTK widget");
    const QPointF center(vtkWidget->width() / 2.0, vtkWidget->height() / 2.0);

    std::vector<PointEvent> added, moved, finished, removed;
    std::vector<PlaneEvent> planeStarted, planeMoved, planeFinished;
    QObject::connect(&view, &Mesh3DView::editablePointAdded, [&](int g, double x, double y, double z) {
        added.push_back({g, -1, x, y, z});
    });
    QObject::connect(&view, &Mesh3DView::editablePointMoved, [&](int g, int i, double x, double y, double z) {
        moved.push_back({g, i, x, y, z});
    });
    QObject::connect(&view, &Mesh3DView::editablePointDragFinished, [&](int g, int i) {
        finished.push_back({g, i});
    });
    QObject::connect(&view, &Mesh3DView::editablePointRemoved, [&](int g, int i) {
        removed.push_back({g, i});
    });
    QObject::connect(&view, &Mesh3DView::planeDragStarted, [&](double x, double y, double z, Qt::KeyboardModifiers m) {
        planeStarted.push_back({x, y, z, 0.0, m});
    });
    QObject::connect(&view, &Mesh3DView::planeDragMoved, [&](double x, double y, double z, double dy, Qt::KeyboardModifiers m) {
        planeMoved.push_back({x, y, z, dy, m});
    });
    QObject::connect(&view, &Mesh3DView::planeDragFinished, [&](double x, double y, double z) {
        planeFinished.push_back({x, y, z, 0.0, {}});
    });

    // Left click on the surface adds a point to the active group.
    view.setPointEditMode(true, 0);
    click(vtkWidget, center, Qt::LeftButton);
    require(added.size() == 1 && added[0].group == 0, "click on the surface did not add a point");
    require(std::abs(added[0].x) < 0.5 && std::abs(added[0].y) < 0.5 && std::abs(added[0].z - 5.0) < 0.1,
            "added point is not on the picked surface");

    // Left drag on the marker moves it over the surface without adding points.
    view.setEditablePoints(0, {{added[0].x, added[0].y, added[0].z}}, QColor(255, 128, 0), 0.6);
    mouse(vtkWidget, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    for (int step = 1; step <= 5; ++step)
        mouse(vtkWidget, QEvent::MouseMove, center + QPointF(10.0 * step, 0.0), Qt::NoButton, Qt::LeftButton);
    mouse(vtkWidget, QEvent::MouseButtonRelease, center + QPointF(50.0, 0.0), Qt::LeftButton, Qt::NoButton);
    require(added.size() == 1, "dragging a marker added a point");
    require(moved.size() >= 5 && moved.back().group == 0 && moved.back().index == 0, "marker drag not reported");
    require(moved.back().x > 5.0 && std::abs(moved.back().y) < 0.5 && std::abs(moved.back().z - 5.0) < 0.1,
            "dragged marker did not follow the surface under the mouse");
    require(finished.size() == 1 && finished[0].group == 0 && finished[0].index == 0, "drag end not reported");

    // Right click without drag on the marker removes it; a right drag does not.
    const QPointF markerPixel = center + QPointF(50.0, 0.0);
    view.setEditablePoints(0, {{moved.back().x, moved.back().y, moved.back().z}}, QColor(255, 128, 0), 0.6);
    click(vtkWidget, markerPixel, Qt::RightButton);
    require(removed.size() == 1 && removed[0].group == 0 && removed[0].index == 0, "right click did not remove the marker");
    mouse(vtkWidget, QEvent::MouseButtonPress, markerPixel, Qt::RightButton, Qt::RightButton);
    mouse(vtkWidget, QEvent::MouseMove, markerPixel + QPointF(40.0, 0.0), Qt::NoButton, Qt::RightButton);
    mouse(vtkWidget, QEvent::MouseButtonRelease, markerPixel + QPointF(40.0, 0.0), Qt::RightButton, Qt::NoButton);
    require(removed.size() == 1, "right drag removed a marker");
    lookDown(); // the right drag rotated the camera

    // Clicking empty space adds nothing.
    click(vtkWidget, QPointF(4.0, 4.0), Qt::LeftButton);
    require(added.size() == 1, "click outside the meshes added a point");

    // Plane drag reports points on the plane and Alt + vertical movement.
    view.setPointEditMode(false);
    view.clearEditablePoints();
    view.setPlaneDragMode(true, {0, 0, 0}, {0, 0, 1});
    mouse(vtkWidget, QEvent::MouseButtonPress, center, Qt::LeftButton, Qt::LeftButton);
    mouse(vtkWidget, QEvent::MouseMove, center + QPointF(30.0, -20.0), Qt::NoButton, Qt::LeftButton, Qt::AltModifier);
    mouse(vtkWidget, QEvent::MouseButtonRelease, center + QPointF(30.0, -20.0), Qt::LeftButton, Qt::NoButton);
    require(planeStarted.size() == 1 && std::abs(planeStarted[0].x) < 0.5 && std::abs(planeStarted[0].y) < 0.5 &&
                std::abs(planeStarted[0].z) < 1e-6,
            "plane drag did not start on the plane under the mouse");
    require(planeMoved.size() == 1 && planeMoved[0].x > 3.0 && planeMoved[0].y > 2.0 && std::abs(planeMoved[0].z) < 1e-6,
            "plane drag did not follow the mouse on the plane");
    require(std::abs(planeMoved[0].deltaY + 20.0) < 1e-9 && (planeMoved[0].modifiers & Qt::AltModifier),
            "plane drag lost the vertical delta or the Alt modifier");
    require(planeFinished.size() == 1, "plane drag end not reported");
    view.setPlaneDragMode(false);

    // Overlay polylines are drawn on top of the meshes and can be removed.
    const auto redPixels = [&] {
        view.render();
        settle();
        const QImage image = vtkWidget->grabFramebuffer();
        int count = 0;
        for (int y = 0; y < image.height(); ++y)
            for (int x = 0; x < image.width(); ++x) {
                const QColor c = image.pixelColor(x, y);
                if (c.red() > 200 && c.green() < 90 && c.blue() < 90)
                    ++count;
            }
        return count;
    };
    const int baseline = redPixels();
    auto points = vtkSmartPointer<vtkPoints>::New();
    for (const auto& p : {std::array<double, 2>{-10, -10}, {10, -10}, {10, 10}, {-10, 10}})
        points->InsertNextPoint(p[0], p[1], 0.0); // inside the box: must still be visible
    auto lines = vtkSmartPointer<vtkCellArray>::New();
    const vtkIdType ids[5] = {0, 1, 2, 3, 0};
    lines->InsertNextCell(5, ids);
    auto square = vtkSmartPointer<vtkPolyData>::New();
    square->SetPoints(points);
    square->SetLines(lines);
    view.setOverlayPolyline(7, square, QColor(255, 0, 0), 3.0);
    const int withOverlay = redPixels();
    view.removeOverlay(7);
    const int afterRemoval = redPixels();
    std::cout << "  red pixels: baseline " << baseline << ", overlay " << withOverlay << ", removed " << afterRemoval << '\n';
    require(withOverlay > baseline + 200, "overlay contour was not drawn on top of the mesh");
    require(afterRemoval <= baseline + 10, "removed overlay is still visible");
}
} // namespace

int main(int argc, char** argv)
{
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    try {
        testRayPlaneIntersection();
        std::cout << "PASS ray-plane intersection\n";
        testInteraction();
        std::cout << "PASS point editing, plane drag and overlays\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
