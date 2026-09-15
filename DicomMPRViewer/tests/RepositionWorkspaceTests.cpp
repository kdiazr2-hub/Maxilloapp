#include "MainWindow.h"
#include "Mesh3DView.h"
#include "ProjectSerializer.h"
#include "TransformCore.h"
#include "MPRView.h"
#include "MeshGenerator.h"
#include "NrrdVolumeExporter.h"
#include "ObjectLabels.h"

#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDir>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFont>
#include <QImage>
#include <QLayout>
#include <QLabel>
#include <QListWidget>
#include <QLineEdit>
#include <QLocale>
#include <QMouseEvent>
#include <QMessageBox>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSlider>
#include <QStackedWidget>
#include <QStyle>
#include <QStyleOptionSpinBox>
#include <QSurfaceFormat>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QVTKOpenGLNativeWidget.h>

#include <vtkActor.h>
#include <vtkActorCollection.h>
#include <vtkAppendPolyData.h>
#include <vtkCamera.h>
#include <vtkDiscreteMarchingCubes.h>
#include <vtkFeatureEdges.h>
#include <vtkGenericOpenGLRenderWindow.h>
#include <vtkImageData.h>
#include <vtkMapper.h>
#include <vtkPolyData.h>
#include <vtkProperty.h>
#include <vtkRenderer.h>
#include <vtkRendererCollection.h>
#include <vtkSphereSource.h>
#include <vtkTransform.h>
#include <cmath>
#include <iostream>
#include <stdexcept>

class HideTestDialogs : public QObject
{
    bool eventFilter(QObject* object, QEvent* event) override
    {
        if (event->type() == QEvent::Polish)
            if (auto* dialog = qobject_cast<QDialog*>(object)) dialog->setAttribute(Qt::WA_DontShowOnScreen);
        return false;
    }
};

static void require(bool passed, const char* message)
{
    if (!passed) throw std::runtime_error(message);
}

static void settle()
{
    for (int i = 0; i < 5; ++i)
        QApplication::processEvents();
}

static void clickAt(QWidget* widget, const QPoint& point)
{
    auto* receiver = widget->childAt(point);
    if (!receiver) receiver = widget;
    const QPointF local = receiver->mapFrom(widget, point);
    const QPointF global = widget->mapToGlobal(point);
    QMouseEvent press(QEvent::MouseButtonPress, local, global,
                      Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(receiver, &press);
    QMouseEvent release(QEvent::MouseButtonRelease, local, global,
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(receiver, &release);
    settle();
}

static void checkStepButtons(QDoubleSpinBox* spin)
{
    const QLocale originalLocale = spin->locale();
    const double originalValue = spin->value();
    spin->setLocale(QLocale(QLocale::Spanish, QLocale::Colombia));
    spin->setValue(1.0);
    spin->setFocus();
    settle();
    QStyleOptionSpinBox option;
    option.initFrom(spin);
    option.frame = true;
    option.buttonSymbols = spin->buttonSymbols();
    option.stepEnabled = QAbstractSpinBox::StepUpEnabled | QAbstractSpinBox::StepDownEnabled;
    const QRect up = spin->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxUp, spin);
    const QRect down = spin->style()->subControlRect(QStyle::CC_SpinBox, &option, QStyle::SC_SpinBoxDown, spin);
    auto* editor = spin->findChild<QLineEdit*>();
    require(editor && !up.isEmpty() && !down.isEmpty(), "Missing spin box controls");
    require(!editor->geometry().intersects(up) && !editor->geometry().intersects(down),
            "Spin box editor overlaps a step button");
    clickAt(spin, up.center());
    require(std::abs(spin->value() - (1.0 + spin->singleStep())) < 1e-6,
            "Clicking increase did not increase the step size");
    clickAt(spin, down.center());
    require(std::abs(spin->value() - 1.0) < 1e-6,
            "Clicking decrease did not decrease the step size");
    editor->setText("2,00" + spin->suffix());
    clickAt(spin, up.center());
    require(std::abs(spin->value() - (2.0 + spin->singleStep())) < 1e-6,
            "Increase did not commit the value typed with a decimal comma");
    spin->setValue(spin->maximum());
    clickAt(spin, up.center());
    require(spin->value() == spin->maximum(), "Increase exceeded the maximum");
    clickAt(spin, down.center());
    require(spin->value() < spin->maximum(), "Decrease stopped working at the maximum");
    spin->setValue(spin->minimum());
    clickAt(spin, down.center());
    require(spin->value() == spin->minimum(), "Decrease exceeded the minimum");
    clickAt(spin, up.center());
    require(spin->value() > spin->minimum(), "Increase stopped working at the minimum");
    spin->setValue(originalValue);
    spin->setLocale(originalLocale);
}

static void checkLayeredDisplay()
{
    Mesh3DView view;
    view.setAttribute(Qt::WA_DontShowOnScreen);
    view.resize(640, 480);
    view.show();
    auto sphere = vtkSmartPointer<vtkSphereSource>::New();
    sphere->SetRadius(8.0);
    sphere->SetThetaResolution(48);
    sphere->SetPhiResolution(48);
    sphere->SetCenter(0.0, -12.0, 0.0);
    sphere->Update();
    auto front = vtkSmartPointer<vtkPolyData>::New();
    front->DeepCopy(sphere->GetOutput());
    sphere->SetCenter(0.0, 12.0, 0.0);
    sphere->Update();
    auto back = vtkSmartPointer<vtkPolyData>::New();
    back->DeepCopy(sphere->GetOutput());
    view.addMesh(1, front, "Front");
    view.addMesh(2, back, "Back");
    view.setMeshColor(1, QColor(240, 40, 40));
    view.setMeshColor(2, QColor(40, 240, 40));
    view.setStandardView(0);
    auto* vtkWidget = view.findChild<QVTKOpenGLNativeWidget*>();
    auto centerPixel = [&] {
        view.render();
        settle();
        const QImage image = vtkWidget->grabFramebuffer();
        return image.pixelColor(image.width() / 2, image.height() / 2);
    };
    QColor pixel = centerPixel();
    require(pixel.red() > pixel.green() * 2, "Front object should initially hide the back object");
    view.setMeshDisplayOptions(1, 0.0, false);
    pixel = centerPixel();
    require(pixel.green() > pixel.red() * 2, "Zero opacity did not reveal the underlying object");
    view.setMeshDisplayOptions(1, 0.35, false);
    pixel = centerPixel();
    require(pixel.red() > 60 && pixel.green() > 60, "Translucent objects were not blended");
    view.setMeshDisplayOptions(1, 1.0, false);
    view.setMeshDisplayOptions(2, 1.0, true);
    pixel = centerPixel();
    require(pixel.green() > pixel.red() * 2, "Always-on-top object stayed occluded");
    int picked = -1;
    QObject::connect(&view, &Mesh3DView::pointPicked, [&picked](int label, double, double, double) { picked = label; });
    view.setPointPickMode(true);
    clickAt(vtkWidget, vtkWidget->rect().center());
    require(picked == 2, "Picking ignored the foreground display order");
    view.setPointPickMode(false);
    view.setMeshDisplayOptions(2, 0.35, true);
    pixel = centerPixel();
    require(pixel.red() > 60 && pixel.green() > 60, "Foreground opacity was not blended");
    view.setMeshVisible(2, false);
    pixel = centerPixel();
    require(pixel.red() > pixel.green() * 2, "Hidden foreground object remained visible");
    view.setMeshVisible(2, true);
    view.setMeshDisplayOptions(2, 1.0, false);
    pixel = centerPixel();
    require(pixel.red() > pixel.green() * 2, "Turning off on-top did not restore depth order");
    view.setMeshDisplayOptions(2, 1.0, true);
    view.removeMesh(1);
    view.setStandardView(0);
    pixel = centerPixel();
    require(pixel.green() > pixel.red() * 2, "Camera fitting lost an all-foreground scene");
    view.removeMesh(2);
    pixel = centerPixel();
    require(std::abs(pixel.green() - pixel.red()) < 20, "Removed object left a foreground ghost");
    std::cout << "Opacity, foreground compositing and picking OK\n";
}

class RepositionWorkspaceTests
{
public:
    static void runSegmentationMeshSync()
    {
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1366, 768);
        window.show();
        auto mask = vtkSmartPointer<vtkImageData>::New();
        mask->SetDimensions(48, 24, 21);
        mask->SetSpacing(0.6, 0.6, 0.8);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        for (int z = 0; z < 21; ++z)
            for (int y = 0; y < 24; ++y)
                for (int x = 0; x < 48; ++x) {
                    const int label = (y >= 4 && y <= 19)
                        ? ((x >= 4 && x <= 19) ? 5 : (x >= 28 && x <= 43) ? 6 : 0) : 0;
                    mask->SetScalarComponentFromDouble(x, y, z, 0, label);
                }
        window.m_volume = mask;
        window.m_segmentationLabelmap = mask;
        const QVector<Mesh3DView*> views = {
            window.m_mesh3DView, window.m_modelMaxillaView, window.m_modelMandibleView,
            window.m_modelMatchView, window.m_orientationView, window.m_osteotomyView,
            window.m_biteSegmentView, window.m_biteRegistrationView, window.m_repositionView,
            window.m_splintUpperView, window.m_splintLowerView, window.m_splintView};
        auto isClosed = [](vtkPolyData* mesh) {
            if (!mesh) return false;
            auto edges = vtkSmartPointer<vtkFeatureEdges>::New();
            edges->SetInputData(mesh);
            edges->BoundaryEdgesOn();
            edges->NonManifoldEdgesOn();
            edges->FeatureEdgesOff();
            edges->ManifoldEdgesOff();
            edges->Update();
            return edges->GetOutput()->GetNumberOfCells() == 0;
        };
        for (int label : {5, 6}) {
            window.addMaskEntry("Bone", QColor(200, 190, 180), label);
            window.addObjectEntry("Bone object", QColor(190, 180, 170), label);
            auto old = vtkSmartPointer<vtkDiscreteMarchingCubes>::New();
            old->SetInputData(mask);
            old->SetValue(0, label);
            old->Update();
            require(!isClosed(old->GetOutput()), "Fixture must reproduce the old open surface");
            for (auto* view : views) {
                view->addMesh(label, old->GetOutput(), "Old mask");
                view->addMesh(1000 + label, old->GetOutput(), "Old object");
            }
        }
        window.setObjectDisplayOptions(5, 0.4, true);
        window.setObjectEntryVisible(6, false);
        auto* matchWidget = window.m_modelMatchView->findChild<QVTKOpenGLNativeWidget*>();
        vtkRenderer* matchRenderer = nullptr;
        auto* renderers = matchWidget->renderWindow()->GetRenderers();
        renderers->InitTraversal();
        while (auto* renderer = renderers->GetNextItem())
            if (renderer->GetLayer() == 1) matchRenderer = renderer;
        require(matchRenderer != nullptr, "Missing match renderer");
        matchRenderer->GetActiveCamera()->Azimuth(31);
        matchRenderer->GetActiveCamera()->Elevation(12);
        auto cameraBefore = vtkSmartPointer<vtkCamera>::New();
        cameraBefore->DeepCopy(matchRenderer->GetActiveCamera());
        QTemporaryDir temp;
        QString error;
        const QString path = temp.filePath("segmentation.nrrd");
        require(NrrdVolumeExporter::exportToFile(mask, path, &error), "Cannot export segmentation fixture");
        window.m_pendingAutoMeshLabel = -3;
        window.onSegmentationFinished(path);
        for (int axis = 0; axis < 3; ++axis) {
            require(std::abs(cameraBefore->GetPosition()[axis]
                             - matchRenderer->GetActiveCamera()->GetPosition()[axis]) < 1e-6,
                    "Synchronizing the model mesh reset the camera position");
            require(std::abs(cameraBefore->GetFocalPoint()[axis]
                             - matchRenderer->GetActiveCamera()->GetFocalPoint()[axis]) < 1e-6,
                    "Synchronizing the model mesh reset the camera target");
        }
        for (int label : {5, 6}) {
            auto latest = window.m_mesh3DView->meshData(label);
            require(isClosed(latest), "Segmentation did not generate the closed surface");
            require(window.meshForAnatomicLabel(label) == latest,
                    "Model lookup preferred the stale object instead of the latest segmentation");
            for (auto* view : views) {
                for (int key : {label, 1000 + label})
                    if (auto mesh = view->meshData(key))
                        require(mesh == latest, "A workspace retained an old bone surface");
            }
        }
        for (auto* view : {window.m_mesh3DView, window.m_modelMaxillaView, window.m_modelMatchView}) {
            int visibleCopies = 0;
            auto* widget = view->findChild<QVTKOpenGLNativeWidget*>();
            auto* layers = widget->renderWindow()->GetRenderers();
            layers->InitTraversal();
            while (auto* renderer = layers->GetNextItem()) {
                auto* actors = renderer->GetActors();
                actors->InitTraversal();
                while (auto* actor = actors->GetNextActor()) {
                    if (!actor->GetVisibility() || !actor->GetMapper()
                        || actor->GetMapper()->GetInput() != window.meshForAnatomicLabel(5)) continue;
                    ++visibleCopies;
                    require(std::abs(actor->GetProperty()->GetOpacity() - 0.4) < 1e-6,
                            "Mesh synchronization lost object opacity");
                    require(renderer->GetLayer() == 2, "Mesh synchronization lost foreground display");
                }
            }
            require(visibleCopies == 1, "Mask and object were rendered twice on top of each other");
        }

        auto segmentSource = vtkSmartPointer<vtkSphereSource>::New();
        segmentSource->SetCenter(100, -45, 70);
        segmentSource->Update();
        window.m_leFortSegmentMesh = segmentSource->GetOutput();
        window.m_repositionTranslationMm[206] = QVector3D(3, -2, 1);
        window.m_repositionRotationDeg[206] = QVector3D(1, 2, 3);
        window.m_repositionOriginalMeshes[206] = segmentSource->GetOutput();
        window.m_repositionView->addMesh(1206, segmentSource->GetOutput(), "Moved Le Fort");
        const auto originalMaskSurface = window.m_mesh3DView->meshData(5);
        window.calculateObjectFromMask(5);
        auto recalculated = window.meshForAnatomicLabel(5);
        require(originalMaskSurface == window.m_mesh3DView->meshData(5), "Conversion changed the mask surface");
        for (auto* view : views) {
                if (auto mesh = view->meshData(1005))
                    require(mesh == recalculated, "Recalculation did not reach every source-bone view");
        }
        require(window.m_leFortSegmentMesh == segmentSource->GetOutput()
                    && window.m_repositionView->meshData(1206) == segmentSource->GetOutput(),
                "Updating a source bone replaced an already moved surgical segment");
        require(window.m_repositionTranslationMm[206] == QVector3D(3, -2, 1)
                    && window.m_repositionRotationDeg[206] == QVector3D(1, 2, 3),
                "Updating a bone reset planned movements");

        ProjectState saved;
        saved.labelmap = window.m_segmentationLabelmap;
        for (int label : {5, 6}) {
            saved.masks.append({label, "Bone", QColor(200, 190, 180), true});
            saved.objects.append({label, "Bone object", QColor(190, 180, 170), label == 5,
                                  label == 5 ? 0.4 : 1.0, label == 5});
            saved.maskMeshes[label] = window.m_mesh3DView->meshData(label);
            saved.objectMeshes[label] = window.m_mesh3DView->meshData(1000 + label);
        }
        require(saved.maskMeshes.value(5) == originalMaskSurface && saved.objectMeshes.value(5) == recalculated,
                "Saving did not preserve separate mask and converted object surfaces");
        const QString projectPath = temp.filePath("sync.maxilloproject");
        require(ProjectSerializer::save(projectPath, saved, &error), "Cannot save updated bone geometry");
        ProjectState restored;
        require(ProjectSerializer::load(projectPath, restored, &error), "Cannot reopen updated bone geometry");
        window.applyProjectState(restored);
        window.syncModelViews();
        require(isClosed(window.meshForAnatomicLabel(5)), "Reopening restored the obsolete surface");
        require(!window.objectEntryVisible(6), "Bone synchronization changed object visibility");
        window.m_viewModeStack->setCurrentIndex(1);
        settle();
        QDir().mkpath("workspace-test-artifacts");
        window.grab().save("workspace-test-artifacts/models-updated-bone.png");
        window.continueToOrientationWithoutMatch();
        require(isClosed(window.m_upperCompositeMesh) && isClosed(window.m_lowerCompositeMesh),
                "New composites were built from stale source bones");
        require(isClosed(window.m_orientationView->meshData(1203)),
                "Orientation did not receive the updated composite");
        window.setOsteotomyWorkspace(true);
        require(isClosed(window.m_osteotomyView->meshData(1203)),
                "Osteotomy did not receive the updated composite");
        window.setRepositionWorkspace(true);
        require(isClosed(window.m_repositionView->meshData(1203)),
                "Reposition did not receive the updated composite");
    }

    // Masks, their slice overlays and 3D surfaces before and after converting them to objects.
    static void runMaskConversionDisplay()
    {
        HideTestDialogs hideDialogs;
        qApp->installEventFilter(&hideDialogs);
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1600, 1000);
        window.show();
        settle();
        const int nx = 64, ny = 64, nz = 48;
        auto volume = vtkSmartPointer<vtkImageData>::New();
        volume->SetDimensions(nx, ny, nz);
        volume->SetSpacing(0.5, 0.5, 0.6);
        volume->SetOrigin(-10.0, -20.0, -30.0);
        volume->AllocateScalars(VTK_SHORT, 1);
        auto mask = vtkSmartPointer<vtkImageData>::New();
        mask->CopyStructure(volume);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        for (int z = 0; z < nz; ++z)
            for (int y = 0; y < ny; ++y)
                for (int x = 0; x < nx; ++x) {
                    const bool inside = x >= 10 && x <= 54 && y >= 10 && y <= 54;
                    const int label = inside && z >= 20 && z <= 40 ? 5 : inside && z >= 4 && z <= 18 ? 6 : 0;
                    mask->SetScalarComponentFromDouble(x, y, z, 0, label);
                    volume->SetScalarComponentFromDouble(x, y, z, 0, label ? 1200 : 0);
                }
        window.m_volume = volume;
        window.m_segmentationLabelmap = mask;
        window.distributeVolume();
        settle();
        for (int label : {5, 6}) {
            window.addMaskEntry(label == 5 ? "Maxilar" : "Mandibula", QColor(220, 200, 180), label);
            window.refreshEditedSegmentationMesh(label);
        }
        window.refreshSegmentationOverlays();
        settle();

        const auto overlayPixels = [](MPRView* view) {
            view->render();
            const QImage image = view->findChild<QVTKOpenGLNativeWidget*>()->grabFramebuffer();
            int pink = 0, blue = 0;
            for (int y = 0; y < image.height(); ++y)
                for (int x = 0; x < image.width(); ++x) {
                    const QColor c = image.pixelColor(x, y);
                    if (c.red() - c.green() > 25 && std::abs(c.green() - c.blue()) < 25 && c.green() > 60) ++pink;
                    else if (c.blue() - c.red() > 25 && c.blue() > c.green() && c.green() > 60) ++blue;
                }
            return std::pair<int, int>(pink, blue);
        };
        const auto actorVisible = [&](int key) -> int {
            const auto mesh = window.m_mesh3DView->meshData(key);
            if (!mesh) return -1;
            auto* renderers = window.m_mesh3DView->findChild<QVTKOpenGLNativeWidget*>()->renderWindow()->GetRenderers();
            renderers->InitTraversal();
            while (auto* renderer = renderers->GetNextItem()) {
                auto* actors = renderer->GetActors();
                actors->InitTraversal();
                while (auto* actor = actors->GetNextActor())
                    if (actor->GetMapper() && actor->GetMapper()->GetInput() == mesh) return actor->GetVisibility();
            }
            return -2;
        };
        struct Display
        {
            std::array<std::pair<int, int>, 3> overlays; // axial, coronal, sagittal: pink (5), blue (6)
            int mask5 = 0, mask6 = 0, object5 = 0, object6 = 0;
        };
        const auto capture = [&](const char* stage) {
            Display display;
            int index = 0;
            for (auto* view : {window.m_axialView, window.m_coronalView, window.m_sagittalView})
                display.overlays[static_cast<size_t>(index++)] = overlayPixels(view);
            display.mask5 = actorVisible(5);
            display.mask6 = actorVisible(6);
            display.object5 = actorVisible(1005);
            display.object6 = actorVisible(1006);
            QDir().mkpath("workspace-test-artifacts");
            window.grab().save(QString("workspace-test-artifacts/mask-conversion-%1.png").arg(stage));
            return display;
        };
        const Display before = capture("before");
        require(before.mask5 == 1 && before.mask6 == 1, "Mask surfaces were not shown before conversion");
        for (const auto& overlay : before.overlays)
            require(overlay.first > 1000, "Maxilla overlay missing on a slice view");
        require(before.overlays[1].second > 1000 && before.overlays[2].second > 1000,
                "Mandible overlay missing on the coronal or sagittal view");

        window.calculateObjectFromMask(5);
        window.calculateObjectFromMask(6);
        settle();
        const Display converted = capture("converted");
        require(converted.overlays == before.overlays, "Converting masks changed their slice overlays");
        require(converted.object5 == 1 && converted.object6 == 1 && converted.mask5 == 0 && converted.mask6 == 0,
                "Visible objects did not stand in for their mask surfaces");

        for (int row = 0; row < window.m_objectTable->rowCount(); ++row)
            if (auto* visible = window.m_objectTable->item(row, 2)) visible->setCheckState(Qt::Unchecked);
        settle();
        const Display hidden = capture("objects-hidden");
        require(hidden.overlays == before.overlays, "Hiding objects changed the slice overlays");
        require(hidden.object5 == 0 && hidden.object6 == 0 && hidden.mask5 == 1 && hidden.mask6 == 1,
                "Hiding the objects left their masks hidden in 3D");

        // MODELOS registration: the CT bone stays next to the scan under the gizmo, even with the mask and
        // the object hidden in the lists.
        window.setMaskVisible(5, false);
        window.setMaskVisible(6, false);
        auto scan = vtkSmartPointer<vtkSphereSource>::New();
        scan->SetRadius(5.0);
        scan->Update();
        window.m_upperArchMesh = scan->GetOutput();
        if (window.m_modelStepStack) window.m_modelStepStack->setCurrentIndex(0);
        window.syncModelViews();
        settle();
        const auto visibleIn = [](Mesh3DView* view, int key) -> int {
            const auto mesh = view->meshData(key);
            if (!mesh) return -1;
            auto* renderers = view->findChild<QVTKOpenGLNativeWidget*>()->renderWindow()->GetRenderers();
            renderers->InitTraversal();
            while (auto* renderer = renderers->GetNextItem()) {
                auto* actors = renderer->GetActors();
                actors->InitTraversal();
                while (auto* actor = actors->GetNextActor())
                    if (actor->GetMapper() && actor->GetMapper()->GetInput() == mesh) return actor->GetVisibility();
            }
            return -2;
        };
        require(visibleIn(window.m_modelMatchView, 5) == 1, "The CT maxilla is hidden in the registration view");
        require(visibleIn(window.m_modelMatchView, objectActorKey(kUpperArchLabel)) == 1,
                "The scan is missing in the registration view");
    }

    static void runBoneCavityFill()
    {
        HideTestDialogs hideDialogs;
        qApp->installEventFilter(&hideDialogs);
        MainWindow window;
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.resize(1366, 768);
        window.show();
        auto mask = vtkSmartPointer<vtkImageData>::New();
        mask->SetDimensions(41, 41, 41);
        mask->SetSpacing(0.5, 0.5, 0.8);
        mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
        auto volume = vtkSmartPointer<vtkImageData>::New();
        volume->CopyStructure(mask);
        volume->AllocateScalars(VTK_SHORT, 1);
        for (int z = 0; z < 41; ++z)
            for (int y = 0; y < 41; ++y)
                for (int x = 0; x < 41; ++x) {
                    const double r = std::sqrt(std::pow(x - 20.0, 2) + std::pow(y - 20.0, 2) + std::pow(z - 20.0, 2));
                    const int label = r > 15 ? 0 : r >= 11 ? 6 : 2;
                    mask->SetScalarComponentFromDouble(x, y, z, 0, label);
                    volume->SetScalarComponentFromDouble(x, y, z, 0, label == 6 ? 1200 : label == 2 ? 100 : -1000);
                }
        window.m_volume = volume;
        window.m_segmentationLabelmap = mask;
        for (auto* view : {window.m_axialView, window.m_coronalView, window.m_sagittalView})
            view->setVolume(volume);
        window.addMaskEntry("Mandibula", QColor(210, 190, 170), 6);
        window.addMaskEntry("Tejido", QColor(200, 150, 150), 2);
        window.m_maskTable->selectRow(0);
        window.m_maskSmoothingIterations[6] = 0;
        window.refreshEditedSegmentationMesh(6);
        window.updateButtonStates();
        require(window.m_fillBoneCavityAct->isEnabled(), "Cavity fill tool is not available with a mask");
        const double before = mask->GetScalarComponentAsDouble(18, 20, 20, 0);
        auto preview = [&](bool apply) {
            bool reviewed = false, compared = false;
            QString failure;
            QTimer response;
            QTimer watchdog;
            watchdog.setSingleShot(true);
            QObject::connect(&watchdog, &QTimer::timeout, [&] {
                failure = "Timed out waiting for cavity preview";
                if (auto* dialog = qobject_cast<QDialog*>(qApp->activeModalWidget())) dialog->reject();
            });
            QObject::connect(&response, &QTimer::timeout, [&] {
                if (auto* box = qobject_cast<QMessageBox*>(qApp->activeModalWidget())) {
                    failure = box->text();
                    box->reject();
                    return;
                }
                auto* dialog = window.findChild<QDialog*>("BoneCavityPreview");
                if (!dialog || !dialog->isVisible()) return;
                response.stop();
                watchdog.stop();
                reviewed = true;
                auto* compare = dialog->findChild<QCheckBox*>("BoneCavityCompare");
                if (compare) {
                    auto* view = dialog->findChild<MPRView*>();
                    auto* surface = view->findChild<QVTKOpenGLNativeWidget*>();
                    compare->click();
                    compared = !compare->isChecked();
                    view->render();
                    const QImage beforeFill = surface->grabFramebuffer();
                    compare->click();
                    view->render();
                    const QImage afterFill = surface->grabFramebuffer();
                    int changedPixels = 0;
                    if (beforeFill.size() == afterFill.size())
                        for (int y = 0; y < beforeFill.height(); ++y)
                            for (int x = 0; x < beforeFill.width(); ++x)
                                changedPixels += beforeFill.pixel(x, y) != afterFill.pixel(x, y);
                    if (changedPixels < 20) failure = "Before/after comparison did not change the slice overlay";
                }
                if (window.m_segmentationLabelmap != mask || mask->GetScalarComponentAsDouble(18, 20, 20, 0) != before)
                    failure = "Preview modified the original mask";
                for (const auto size : {QSize(1100, 700), QSize(800, 600)}) {
                    dialog->resize(size);
                    settle();
                    for (auto* view : dialog->findChildren<MPRView*>()) view->render();
                    for (auto* view : dialog->findChildren<Mesh3DView*>()) view->render();
                    settle();
                    for (auto* view : dialog->findChildren<MPRView*>()) {
                        auto* renderWidget = view->findChild<QVTKOpenGLNativeWidget*>();
                        auto* slider = view->findChild<QSlider*>();
                        if (!view->rect().contains(renderWidget->geometry()) ||
                            renderWidget->geometry().intersects(slider->geometry()))
                            failure = "Preview render area overlaps slice controls";
                    }
                    QDir().mkpath("workspace-test-artifacts");
                    if (!dialog->grab().save(QString("workspace-test-artifacts/cavity-preview-%1.png").arg(size.width())))
                        failure = "Could not save cavity preview screenshot";
                }
                auto* buttons = dialog->findChild<QDialogButtonBox*>();
                buttons->button(apply ? QDialogButtonBox::Apply : QDialogButtonBox::Cancel)->click();
            });
            response.start(20);
            watchdog.start(15000);
            window.m_fillBoneCavityAct->trigger();
            require(window.m_fillBoneCavityLabel == 6, "Fill button did not enable seed picking");
            window.m_axialView->physicalPointClicked(window.m_axialView, 9.0, 10.0, 16.0);
            require(failure.isEmpty(), qPrintable(failure));
            require(reviewed && compared, "Cavity preview was not reviewable");
        };
        preview(false);
        require(window.m_segmentationLabelmap == mask && window.m_labelmapUndoStack.isEmpty(),
                "Cancel changed the mask or undo stack");
        preview(true);
        require(window.m_segmentationLabelmap->GetScalarComponentAsDouble(18, 20, 20, 0) == 6,
                "Apply did not commit the proposed fill");
        require(window.m_labelmapUndoStack.size() == 1, "Fill did not create one undo snapshot");
        const auto filledMask = window.m_segmentationLabelmap;
        const auto filledSurface = window.m_mesh3DView->meshData(6);
        const auto filledTime = filledMask->GetMTime();
        const auto smoothingBefore = window.m_maskSmoothingIterations.value(6);
        window.calculateObjectFromMask(6);
        require(window.m_mesh3DView->meshData(6) == filledSurface,
                "Converting to an object replaced the source mask surface");
        require(window.m_segmentationLabelmap == filledMask && filledMask->GetMTime() == filledTime,
                "Converting to an object mutated the filled mask");
        require(window.m_maskSmoothingIterations.value(6) == smoothingBefore,
                "Converting to an object changed mask smoothing");
        require(!window.m_hiddenMaskLabels.count(6), "Converting hid the source mask");
        window.undoLastEdit();
        require(window.m_segmentationLabelmap->GetScalarComponentAsDouble(18, 20, 20, 0) == before,
                "Undo did not restore the cavity");
        require(window.m_labelmapUndoStack.isEmpty(), "Undo snapshot was not consumed");
        window.m_fillBoneCavityAct->trigger();
        window.setMeasurementTool(MainWindow::MeasurementToolMode::Cursor);
        require(window.m_fillBoneCavityLabel == -1 && !window.m_fillBoneCavityAct->isChecked(),
                "Changing tools did not cancel cavity picking");
        window.addObjectEntry("Existing Le Fort", QColor(210, 190, 170), 206);
        require(window.hasDerivedBonePlanning(), "Loaded osteotomy was not protected from base-mask repair");
        qApp->removeEventFilter(&hideDialogs);
    }

    static void run()
    {
        MainWindow window;
        require(!window.m_objectOpacitySlider->isEnabled() && !window.m_objectOnTopCheck->isEnabled(),
                "Object controls should be disabled without a selection");
        window.setAttribute(Qt::WA_DontShowOnScreen);
        window.show();
        settle();

        // Reproduce projects whose segments live in the object scene, not the cached members.
        const QList<int> labels{206, 210, 211, 212, 213, 214};
        for (int i = 0; i < labels.size(); ++i) {
            auto source = vtkSmartPointer<vtkSphereSource>::New();
            source->SetCenter(120.0 + (i % 3) * 18.0, 210.0, 80.0 + (i / 3) * 18.0);
            source->SetRadius(7.0);
            source->SetThetaResolution(24);
            source->SetPhiResolution(24);
            source->Update();
            window.m_mesh3DView->addMesh(1000 + labels[i], source->GetOutput(), "Fixture");
            window.addObjectEntry(QString("Structure %1").arg(labels[i]), QColor(220, 120, 80), labels[i]);
            window.setObjectEntryVisible(labels[i], false);
        }
        require(!window.m_leFortSegmentMesh, "Fixture must exercise missing cached segment");

        for (auto* tab : window.findChildren<QToolButton*>("MT")) {
            if (tab->text() == QString::fromUtf8("REPOSICI\xc3\x93N")) tab->click();
        }
        settle();
        require(window.m_viewModeStack->currentIndex() == 5, "Reposition tab did not open");

        auto* list = window.m_repositionObjectList;
        require(list && list->count() == labels.size(), "Structure list lost loaded project objects");
        auto* vtkWidget = window.m_repositionView->findChild<QVTKOpenGLNativeWidget*>();
        require(vtkWidget != nullptr, "Missing 3D widget");
        auto* renderers = vtkWidget->renderWindow()->GetRenderers();
        renderers->InitTraversal();
        vtkRenderer* renderer = nullptr;
        while (auto* candidate = renderers->GetNextItem()) {
            if (candidate->GetLayer() == 1) renderer = candidate;
        }
        require(renderer != nullptr, "Missing model renderer");
        auto actorFor = [&](int label) -> vtkActor* {
            const auto mesh = window.m_repositionView->meshData(1000 + label);
            renderers->InitTraversal();
            while (auto* layer = renderers->GetNextItem()) {
                auto* actors = layer->GetActors();
                actors->InitTraversal();
                while (auto* actor = actors->GetNextActor()) {
                    if (actor->GetMapper() && actor->GetMapper()->GetInput() == mesh.GetPointer())
                        return actor;
                }
            }
            return nullptr;
        };
        for (int label : labels) {
            require(window.m_repositionView->meshData(1000 + label) != nullptr,
                    "Listed segment is missing from the 3D view");
            require(actorFor(label) != nullptr, "Listed segment has no render actor");
        }
        for (int row = 0; row < list->count(); ++row) {
            list->item(row)->setCheckState(Qt::Checked);
        }
        window.syncVisibilityPanelToAllViews();
        for (int label : labels) {
            require(actorFor(label)->GetVisibility() == 1, "Global visibility hid a checked structure");
            require(!window.objectEntryVisible(label), "Local selection changed global visibility");
        }

        auto cameraBefore = vtkSmartPointer<vtkCamera>::New();
        renderer->GetActiveCamera()->Azimuth(23.0);
        cameraBefore->DeepCopy(renderer->GetActiveCamera());
        window.setRepositionWorkspace(false);
        window.setRepositionWorkspace(true);
        require(window.selectedRepositionTargetLabels().size() == labels.size(), "Reentry lost checks");
        for (int axis = 0; axis < 3; ++axis) {
            require(std::abs(cameraBefore->GetPosition()[axis] -
                             renderer->GetActiveCamera()->GetPosition()[axis]) < 1e-6,
                    "Reentering reposition moved the camera");
        }

        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setCheckState(Qt::Unchecked);
        window.syncRepositionView();
        require(window.selectedRepositionTargetLabels().isEmpty(), "Refresh selected an unchecked item");
        for (int label : labels)
            require(actorFor(label)->GetVisibility() == 0, "Unchecked hidden structure stayed visible");

        list->setCurrentRow(0);
        list->item(1)->setCheckState(Qt::Checked);
        window.startRepositionGizmo();
        require(window.m_repositionView->hasGizmo(), "Cannot start gizmo on checked structure");
        require(window.m_repositionGizmoGroupLabels == QList<int>{list->item(1)->data(Qt::UserRole).toInt()},
                "Unchecked active row leaked into gizmo group");
        list->item(1)->setCheckState(Qt::Unchecked);
        require(!window.m_repositionView->hasGizmo(), "Unchecking the last structure left a gizmo active");

        for (int row = 0; row < list->count(); ++row)
            list->item(row)->setCheckState(Qt::Checked);
        double pointBefore[3];
        window.repositionMeshForLabel(212)->GetPoint(0, pointBefore);
        window.translateRepositionTarget(1.0, 0.0, 0.0);
        double pointAfter[3];
        window.repositionMeshForLabel(212)->GetPoint(0, pointAfter);
        require(std::abs(pointAfter[0] - pointBefore[0] - 1.0) < 1e-4, "Selected structure did not move");
        require(actorFor(212)->GetVisibility() == 1, "Moving a checked structure hid it");

        auto* pivotButton = window.findChild<QPushButton*>("RepositionChangeRotationCenter");
        require(pivotButton && pivotButton->isEnabled(), "Missing change rotation center button");
        const int pivotLabel = window.currentRepositionTargetLabel();
        auto pivotMesh = window.repositionMeshForLabel(pivotLabel);
        const double* center = pivotMesh->GetCenter();
        const QVector3D oldPivot(center[0], center[1], center[2]);
        window.m_repositionPivotWorld[pivotLabel] = oldPivot;
        window.refreshRepositionPivotMarker();
        cameraBefore->DeepCopy(renderer->GetActiveCamera());
        QMap<int, QVector3D> beforePivotChange;
        for (int label : labels) {
            double point[3];
            window.repositionMeshForLabel(label)->GetPoint(0, point);
            beforePivotChange[label] = QVector3D(point[0], point[1], point[2]);
        }
        auto clickModelPoint = [&](const QVector3D& point) {
            window.m_repositionView->render();
            settle();
            renderer->SetWorldPoint(point.x(), point.y(), point.z(), 1.0);
            renderer->WorldToDisplay();
            const double* display = renderer->GetDisplayPoint();
            const double dpr = vtkWidget->devicePixelRatioF();
            clickAt(vtkWidget, QPoint(qRound(display[0] / dpr),
                                     qRound(vtkWidget->height() - display[1] / dpr)));
        };
        pivotButton->click();
        require(window.m_repositionPickingPivot && pivotButton->isChecked(), "Pivot picking did not activate");
        clickModelPoint(oldPivot);
        require(!window.m_repositionPickingPivot && !pivotButton->isChecked(), "3D click did not finish pivot picking");
        const QVector3D firstPivot = window.m_repositionPivotWorld.value(pivotLabel);
        require((firstPivot - oldPivot).length() > 1.0, "3D click did not replace the old center");
        pivotButton->click();
        clickModelPoint(oldPivot + QVector3D(3.0, 0.0, 0.0));
        require(!window.m_repositionPickingPivot, "Cannot change the rotation center a second time");
        const QVector3D newPivot = window.m_repositionPivotWorld.value(pivotLabel);
        require((newPivot - firstPivot).length() > 1.0, "Repeated picking retained the previous center");
        pivotButton->click();
        pivotButton->click();
        require(!window.m_repositionPickingPivot && !pivotButton->isChecked(), "Cannot cancel changing the center");
        require(window.m_repositionPivotWorld.value(pivotLabel) == newPivot, "Cancel discarded the previous center");
        for (int axis = 0; axis < 3; ++axis)
            require(std::abs(cameraBefore->GetPosition()[axis] - renderer->GetActiveCamera()->GetPosition()[axis]) < 1e-6,
                    "Changing the center moved the camera");
        for (int label : labels) {
            double point[3];
            window.repositionMeshForLabel(label)->GetPoint(0, point);
            require(beforePivotChange[label] == QVector3D(point[0], point[1], point[2]),
                    "Changing the center moved a structure");
        }
        window.rotateRepositionTarget(0.0, 0.0, 1.0, 90.0);
        for (int label : labels) {
            const QVector3D before = beforePivotChange[label];
            const QVector3D expected(newPivot.x() - (before.y() - newPivot.y()),
                                     newPivot.y() + (before.x() - newPivot.x()), before.z());
            double actual[3];
            window.repositionMeshForLabel(label)->GetPoint(0, actual);
            require((QVector3D(actual[0], actual[1], actual[2]) - expected).length() < 1e-4,
                    "Rotation did not use the newly picked center for the selected group");
        }
        window.rotateRepositionTarget(0.0, 0.0, 1.0, -90.0);
        pivotButton->click();
        list->setCurrentRow(1);
        require(!window.m_repositionPickingPivot && !pivotButton->isChecked(), "Changing target left pivot picking active");
        list->setCurrentRow(0);

        window.m_objectTable->selectRow(0);
        require(window.m_objectOpacitySlider->isEnabled(), "Selecting an object did not enable opacity");
        cameraBefore->DeepCopy(renderer->GetActiveCamera());
        auto originalMesh = vtkSmartPointer<vtkPolyData>::New();
        originalMesh->DeepCopy(window.repositionMeshForLabel(206));
        window.m_objectOpacitySlider->setValue(35);
        window.m_objectOnTopCheck->setChecked(true);
        require(std::abs(actorFor(206)->GetProperty()->GetOpacity() - 0.35) < 1e-6,
                "Object opacity did not reach the 3D actor");
        require(!renderer->HasViewProp(actorFor(206)), "On-top actor remained in the normal layer");
        window.m_objectTable->selectRow(1);
        require(window.m_objectOpacitySlider->value() == 100 && !window.m_objectOnTopCheck->isChecked(),
                "Display options leaked to another object");
        window.m_objectTable->selectRow(0);
        require(window.m_objectOpacitySlider->value() == 35 && window.m_objectOnTopCheck->isChecked(),
                "Reselecting an object lost its display options");
        window.syncRepositionView();
        require(std::abs(actorFor(206)->GetProperty()->GetOpacity() - 0.35) < 1e-6 &&
                !renderer->HasViewProp(actorFor(206)), "Workspace rebuild lost display options");
        window.m_repositionView->setMeshOpacity(1206, 1.0);
        require(std::abs(actorFor(206)->GetProperty()->GetOpacity() - 0.35) < 1e-6,
                "Workspace default overrode the user's opacity");
        for (int axis = 0; axis < 3; ++axis)
            require(std::abs(cameraBefore->GetPosition()[axis] - renderer->GetActiveCamera()->GetPosition()[axis]) < 1e-6,
                    "Changing display options moved the camera");
        for (vtkIdType i = 0; i < originalMesh->GetNumberOfPoints(); ++i) {
            double before[3], after[3];
            originalMesh->GetPoint(i, before);
            window.repositionMeshForLabel(206)->GetPoint(i, after);
            for (int axis = 0; axis < 3; ++axis)
                require(before[axis] == after[axis], "Changing display options changed geometry");
        }

        require(!window.m_mandibleMovementTable->isVisible(), "Missing movement history must not display zeros");
        auto initialMandible = vtkSmartPointer<vtkPolyData>::New();
        initialMandible->DeepCopy(window.repositionMeshForLabel(212));
        auto composite = vtkSmartPointer<vtkAppendPolyData>::New();
        composite->AddInputData(initialMandible);
        composite->AddInputData(window.repositionMeshForLabel(210));
        composite->Update();
        window.m_lowerCompositeMesh = vtkSmartPointer<vtkPolyData>::New();
        window.m_lowerCompositeMesh->DeepCopy(composite->GetOutput());
        auto displaced = TransformCore::IdentityMatrix();
        displaced->SetElement(0, 3, 40.0);
        window.captureMandibleMovementReference(212, TransformCore::ApplyTransformToPolyData(initialMandible, displaced));
        require(window.m_mandibleMovement.targetLabel < 0, "A moved legacy mesh was mistaken for the oriented reference");
        window.captureMandibleMovementReference(212, initialMandible);
        require(window.m_mandibleMovement.targetLabel == 212, "Could not preserve the initial mandibular reference");
        const auto movementCenter = window.m_mandibleMovement.referenceCenter;
        auto bite = vtkSmartPointer<vtkTransform>::New();
        bite->Translate(movementCenter[0] + 3.0, movementCenter[1] - 2.0, movementCenter[2] + 4.0);
        bite->RotateZ(10.0);
        bite->Translate(-movementCenter[0], -movementCenter[1], -movementCenter[2]);
        window.recordMandibleMovement(212, bite->GetMatrix(), true);
        window.setRepositionMeshForLabel(212, TransformCore::ApplyTransformToPolyData(initialMandible, bite->GetMatrix()));
        auto manual = TransformCore::IdentityMatrix();
        manual->SetElement(1, 3, 1.0);
        window.recordMandibleMovement(212, manual, true);
        window.setRepositionMeshForLabel(212,
            TransformCore::ApplyTransformToPolyData(window.repositionMeshForLabel(212), manual));
        window.m_repositionOriginalMeshes.erase(212);
        window.m_mandibleMovementResetMatrix.clear();
        window.updateMandibleMovementSummary();
        auto* movementTable = window.m_mandibleMovementTable;
        require(movementTable->isVisible(), "Accepted bite movement is not displayed in Reposition");
        require(movementTable->item(0, 1)->text() == "+3,00" && movementTable->item(1, 1)->text() == "-1,00" &&
                movementTable->item(2, 1)->text() == "+4,00" && movementTable->item(5, 1)->text() == "+10,00",
                "Registration summary has incorrect units, signs or cumulative manual adjustment");
        const auto registeredMatrix = window.m_mandibleMovement.registrationMatrix;
        for (int row = 0; row < list->count(); ++row) {
            const bool mandible = list->item(row)->data(Qt::UserRole).toInt() == 212;
            list->item(row)->setCheckState(mandible ? Qt::Checked : Qt::Unchecked);
            if (mandible) list->setCurrentRow(row);
        }
        window.translateRepositionTarget(1.5, 0.0, 0.0);
        require(movementTable->item(0, 2)->text() == "+4,50" &&
                window.m_mandibleMovement.registrationMatrix == registeredMatrix,
                "Reposition must update current movement without overwriting the bite result");
        window.resetRepositionTarget();
        require(movementTable->item(0, 2)->text() == "+3,00", "Reset did not restore the movement summary");
        window.translateRepositionTarget(1.5, 0.0, 0.0);
        window.recordMandibleMovement(213, manual, true);
        require(window.m_mandibleMovement.registrationMatrix == registeredMatrix, "Chin movement changed the body report");
        for (int row = 0; row < list->count(); ++row) list->item(row)->setCheckState(Qt::Checked);
        list->setCurrentRow(0);

        auto* scroll = window.findChild<QScrollArea*>("RepositionScrollArea");
        require(scroll != nullptr, "Reposition panel needs a scroll container");
        QDir().mkpath("workspace-test-artifacts");
        for (const QSize size : {QSize(1920, 1080), QSize(1366, 768)}) {
            window.resize(size);
            scroll->verticalScrollBar()->setValue(0);
            settle();
            checkStepButtons(window.m_repositionStepSpin);
            checkStepButtons(window.m_repositionRotStepSpin);
            scroll->verticalScrollBar()->setValue(0);
            settle();
            auto* panel = scroll->widget();
            const QRect listRect(list->mapTo(panel, QPoint()), list->size());
            for (auto* button : panel->findChildren<QPushButton*>()) {
                const QRect buttonRect(button->mapTo(panel, QPoint()), button->size());
                require(!listRect.intersects(buttonRect), "Structure list overlaps a control");
                require(buttonRect.right() < panel->width(), "Button clipped by the control panel");
            }
            require(scroll->horizontalScrollBar()->maximum() == 0, "Control panel overflows horizontally");
            if (size.height() == 768)
                require(scroll->verticalScrollBar()->maximum() > 0, "Short window needs vertical scrolling");
            require(window.grab().save(QString("workspace-test-artifacts/reposition-%1.png").arg(size.width())),
                    "Could not save workspace screenshot");
            auto* studyScroll = window.findChild<QScrollArea*>("StudyInfoScrollArea");
            require(studyScroll && studyScroll->horizontalScrollBar()->maximum() == 0,
                    "Movement summary made the study panel overflow horizontally");
            studyScroll->ensureWidgetVisible(window.m_mandibleMovementPanel, 0, 0);
            settle();
            require(window.m_mandibleMovementTable->verticalScrollBar()->maximum() == 0 &&
                    window.m_mandibleMovementTable->horizontalScrollBar()->maximum() == 0,
                    "Movement table must show all six rows without internal scrolling");
            require(window.grab().save(QString("workspace-test-artifacts/movement-%1.png").arg(size.width())),
                    "Could not save movement summary screenshot");
        }
        window.m_repositionView->setStandardView(0);
        window.m_repositionView->render();
        settle();
        const QImage framebuffer = vtkWidget->grabFramebuffer();
        int modelPixels = 0;
        for (int y = 0; y < framebuffer.height(); ++y) {
            for (int x = 0; x < framebuffer.width(); ++x) {
                const QColor c = framebuffer.pixelColor(x, y);
                if (c.red() > 80 && c.red() > c.green() * 1.3 && c.green() > c.blue() * 1.1)
                    ++modelPixels;
            }
        }
        require(modelPixels > 500, "3D framebuffer is blank: selected structures did not render");
        std::cout << "Rendered model pixels: " << modelPixels << '\n';

        QTemporaryDir projectDir;
        require(projectDir.isValid(), "Cannot create display settings fixture");
        ProjectState state;
        state.mandibleMovement = window.m_mandibleMovement;
        ProjObjectEntry entry;
        entry.label = 206;
        entry.name = "Saved appearance";
        entry.color = QColor(220, 120, 80);
        entry.opacity = 0.35;
        entry.alwaysOnTop = true;
        state.objects.append(entry);
        state.objectMeshes[206] = originalMesh;
        entry.label = 212;
        entry.opacity = -1.0;
        entry.alwaysOnTop = false;
        state.objects.append(entry);
        state.objectMeshes[212] = window.repositionMeshForLabel(212);
        QString error;
        const QString path = projectDir.filePath("display.maxilloproject");
        require(ProjectSerializer::save(path, state, &error), "Cannot save display settings fixture");
        ProjectState restored;
        require(ProjectSerializer::load(path, restored, &error), "Cannot reload display settings fixture");
        require(restored.objects[0].opacity == 0.35 && restored.objects[0].alwaysOnTop,
                "Project round trip lost display settings");
        require(restored.objects[1].opacity == -1.0 && !restored.objects[1].alwaysOnTop,
                "Legacy objects did not retain workspace defaults");
        require(restored.mandibleMovement.currentMatrix == state.mandibleMovement.currentMatrix &&
                restored.mandibleMovement.registrationMatrix == registeredMatrix &&
                restored.mandibleMovement.referenceCenter == movementCenter,
                "Project round trip lost mandibular movement history");
        window.applyProjectState(restored);
        window.setRepositionWorkspace(true);
        require(window.m_mandibleMovementTable->item(0, 2)->text() == "+4,50" &&
                window.m_mandibleMovementTable->item(0, 1)->text() == "+3,00",
                "Opening a project did not restore the movement summary");
        window.m_viewModeStack->setCurrentIndex(4);
        require(!window.m_mandibleMovementPanel->isVisible(), "Reposition report leaked to another module");
        window.setRepositionWorkspace(true);
        window.m_objectTable->selectRow(0);
        require(window.m_objectOpacitySlider->value() == 35 && window.m_objectOnTopCheck->isChecked(),
                "Opening a project did not restore object display controls");
        restored.mandibleMovement = {};
        window.applyProjectState(restored);
        window.setRepositionWorkspace(true);
        require(!window.m_mandibleMovementTable->isVisible() && window.m_mandibleMovementStatus->isVisible(),
                "A legacy project displayed movement values from a previous project");
        window.m_objectTable->setRowCount(0);
        window.m_repositionView->clearMeshes();
        window.m_repositionView->addMesh(1206, originalMesh, "New project object");
        require(actorFor(206)->GetProperty()->GetOpacity() == 1.0 && renderer->HasViewProp(actorFor(206)),
                "Display settings leaked into a new project");
    }
};

int main(int argc, char** argv)
{
    QSurfaceFormat::setDefaultFormat(QVTKOpenGLNativeWidget::defaultFormat());
    QApplication app(argc, argv);
    app.setFont(QFont("Segoe UI", 10));
    try {
        RepositionWorkspaceTests::run();
        RepositionWorkspaceTests::runBoneCavityFill();
        RepositionWorkspaceTests::runSegmentationMeshSync();
        RepositionWorkspaceTests::runMaskConversionDisplay();
        checkLayeredDisplay();
        std::cout << "RepositionWorkspaceTests OK\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RepositionWorkspaceTests FAILED: " << error.what() << '\n';
        return 1;
    }
}
