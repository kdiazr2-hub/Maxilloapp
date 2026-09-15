cmake_minimum_required(VERSION 3.20)
project(DicomMPRViewer VERSION 1.0.0 LANGUAGES CXX)

# ─── Language standard ────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─── Qt auto-tools ────────────────────────────────────────────────────────────
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
# No .ui files in this project; AUTOUIC is left off to avoid spurious warnings.

# ─── Qt 6 ─────────────────────────────────────────────────────────────────────
# Set Qt6_DIR or CMAKE_PREFIX_PATH in your cmake configure call, e.g.:
#   cmake -DCMAKE_PREFIX_PATH="C:/Qt/6.x.x/msvc2022_64" ..
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent)

# ─── VTK 9 ────────────────────────────────────────────────────────────────────
# Set VTK_DIR in your cmake configure call, e.g.:
#   cmake -DVTK_DIR="C:/VTK/build" ..
#
# VTK must have been built with:
#   -DVTK_MODULE_ENABLE_VTK_GUISupportQt=YES
#   -DVTK_QT_VERSION=6
find_package(VTK REQUIRED COMPONENTS
    CommonCore            # vtkObject, vtkSmartPointer
    CommonDataModel       # vtkImageData
    CommonMath
    CommonTransforms
    FiltersCore
    FiltersGeneral
    FiltersSources
    GUISupportQt          # QVTKOpenGLNativeWidget
    IOImage               # vtkDICOMImageReader
    IOGeometry            # vtkSTLReader / vtkSTLWriter
    IOXML
    ImagingColor
    ImagingCore
    ImagingGeneral
    ImagingHybrid
    ImagingMath
    ImagingMorphological
    InteractionImage      # vtkResliceImageViewer (NOT InteractionWidgets)
    InteractionStyle      # vtkInteractorStyleImage
    InteractionWidgets    # vtkResliceCursor*
    RenderingAnnotation   # vtkCornerAnnotation
    RenderingCore
    RenderingFreeType
    RenderingOpenGL2
)

# ─── GDCM (optional) ──────────────────────────────────────────────────────────
# Enables compressed DICOM support (JPEG2000, JPEG-LS, RLE).
# Install via: vcpkg install gdcm:x64-windows
# Then pass: -DUSE_GDCM=ON -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
option(USE_GDCM "Enable GDCM backend for compressed DICOM support" OFF)
if(USE_GDCM)
    find_package(GDCM REQUIRED)
    include(${GDCM_USE_FILE})
    add_compile_definitions(HAVE_GDCM)
    message(STATUS "GDCM found: compressed DICOM (JPEG2000/JPEG-LS) enabled")
else()
    message(STATUS "GDCM not enabled: only uncompressed DICOM supported (USE_GDCM=OFF)")
endif()

# ─── Source files ─────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/MainWindow.cpp
    src/DicomVolumeLoader.cpp
    src/DicomSeriesIndexer.cpp
    src/AsyncDicomLoader.cpp
    src/VolumeCacheManager.cpp
    src/SeriesSelectionDialog.cpp
    src/MPRView.cpp
    src/Mesh3DView.cpp
    src/Measurement.cpp
    src/MeasurementManager.cpp
    src/MeasurementTool.cpp
    src/DistanceMeasurementTool.cpp
    src/AngleMeasurementTool.cpp
    src/AnnotationTool.cpp
    src/AreaMeasurementTool.cpp
    src/ROIStatisticsTool.cpp
    src/PerpendicularDistanceTool.cpp
    src/MeasurementOverlay.cpp
    src/MeasurementSerialization.cpp
    src/AISegmentationService.cpp
    src/SegmentationProgressCore.cpp
    src/SegmentationProgressDialog.cpp
    src/StandaloneDentalSegmentatorService.cpp
    src/SlicerDentalSegmentatorService.cpp
    src/NrrdVolumeExporter.cpp
    src/SegmentationImporter.cpp
    src/MeshGenerator.cpp
    src/MaskToObjectCore.cpp
    src/MainWindowSegmentation.cpp
    src/SegmentationOverlay.cpp
    src/SegmentationMaskEditor.cpp
    src/BoneSplitterService.cpp
    src/BoneCavityFill.cpp
    src/WindowLevelPresets.cpp
    src/ProjectSerializer.cpp
    src/TransformCore.cpp
    src/GeometryValidation.cpp
    src/GeometryValidationCore.cpp
    src/CompositeModelCore.cpp
    src/SplintGenerator.cpp
    src/SplintHeightmapGenerator.cpp
    src/SplintDesignCore.cpp
    src/SplintContourEditCore.cpp
    src/SplintPreviewScheduler.cpp
    src/SplintDesignPanel.cpp
    src/MainWindowSplint.cpp
    src/CompositeBlockCore.cpp
    src/MeshRepairCore.cpp
    src/OsteotomyCore.cpp
    src/OsteotomyWizardPanel.cpp
    src/MainWindowOsteotomy.cpp
    src/SurfaceContourOverlay.cpp
    src/MainWindowComposite.cpp
    src/MainWindowModels.cpp
    src/ModelWorkflowCore.cpp
    src/AppStateManager.cpp
    src/LoggerCore.cpp
    src/RegistrationResult.cpp
    src/LandmarkCore.cpp
)

set(HEADERS
    src/MainWindow.h
    src/DicomVolumeLoader.h
    src/DicomSeriesIndexer.h
    src/AsyncDicomLoader.h
    src/VolumeCacheManager.h
    src/SeriesSelectionDialog.h
    src/MPRView.h
    src/Mesh3DView.h
    src/Measurement.h
    src/MeasurementManager.h
    src/MeasurementTool.h
    src/DistanceMeasurementTool.h
    src/AngleMeasurementTool.h
    src/AnnotationTool.h
    src/AreaMeasurementTool.h
    src/ROIStatisticsTool.h
    src/PerpendicularDistanceTool.h
    src/MeasurementOverlay.h
    src/MeasurementSerialization.h
    src/AISegmentationService.h
    src/SegmentationProgressCore.h
    src/SegmentationProgressDialog.h
    src/StandaloneDentalSegmentatorService.h
    src/SlicerDentalSegmentatorService.h
    src/NrrdVolumeExporter.h
    src/SegmentationImporter.h
    src/MeshGenerator.h
    src/SegmentationOverlay.h
    src/SegmentationMaskEditor.h
    src/BoneSplitterService.h
    src/BoneCavityFill.h
    src/WindowLevelPresets.h
    src/ProjectSerializer.h
    src/TransformCore.h
    src/ValidationResult.h
    src/GeometryValidation.h
    src/GeometryValidationCore.h
    src/CompositeModelCore.h
    src/SplintGenerator.h
    src/SplintHeightmapGenerator.h
    src/SplintDesignCore.h
    src/SplintContourEditCore.h
    src/SplintPreviewScheduler.h
    src/SplintDesignPanel.h
    src/ObjectLabels.h
    src/CompositeBlockCore.h
    src/MeshRepairCore.h
    src/OsteotomyCore.h
    src/OsteotomyWizardPanel.h
    src/SurfaceContourOverlay.h
    src/AppStateManager.h
    src/LoggerCore.h
    src/RegistrationResult.h
    src/LandmarkCore.h
)

set(RESOURCES
    resources/branding.qrc
)

# ─── Executable ───────────────────────────────────────────────────────────────
# WIN32 hides the console window in release builds.
# Remove WIN32 during development if you want stdout/stderr in a terminal.
add_executable(DicomMPRViewer WIN32 ${SOURCES} ${HEADERS} ${RESOURCES})

target_include_directories(DicomMPRViewer PRIVATE src)
target_compile_definitions(DicomMPRViewer PRIVATE APP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
if(MSVC)
    # Source files contain Spanish UI text. Compile all literals as UTF-8.
    target_compile_options(DicomMPRViewer PRIVATE /utf-8)
endif()

target_link_libraries(DicomMPRViewer PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Concurrent        # QThread, QFuture (used by QThread indirectly)
    ${VTK_LIBRARIES}
)

if(USE_GDCM)
    # ${GDCM_LIBRARIES} is empty in vcpkg's GDCMConfig — link targets directly.
    # gdcmMSFF is the main reader/writer and pulls in gdcmIOD/gdcmDSED/gdcmDICT transitively.
    target_link_libraries(DicomMPRViewer PRIVATE gdcmMSFF gdcmDICT gdcmCommon)
endif()

# ─── VTK module auto-init ─────────────────────────────────────────────────────
# Required in VTK 9: registers factory overrides (OpenGL backend, etc.)
# Without this the render window is blank.
vtk_module_autoinit(
    TARGETS DicomMPRViewer
    MODULES ${VTK_LIBRARIES}
)

enable_testing()
add_executable(GeometryCoreTests
    tests/GeometryCoreTests.cpp
    src/TransformCore.cpp
    src/GeometryValidation.cpp
    src/GeometryValidationCore.cpp
    src/CompositeModelCore.cpp
    src/AppStateManager.cpp
    src/RegistrationResult.cpp
    src/LandmarkCore.cpp
)
target_include_directories(GeometryCoreTests PRIVATE src)
target_link_libraries(GeometryCoreTests PRIVATE
    Qt6::Core
    Qt6::Gui
    ${VTK_LIBRARIES}
)
vtk_module_autoinit(
    TARGETS GeometryCoreTests
    MODULES ${VTK_LIBRARIES}
)
add_test(NAME GeometryCoreTests COMMAND GeometryCoreTests)

add_executable(BoneCavityFillTests tests/BoneCavityFillTests.cpp src/BoneCavityFill.cpp)
target_include_directories(BoneCavityFillTests PRIVATE src)
target_link_libraries(BoneCavityFillTests PRIVATE Qt6::Core ${VTK_LIBRARIES})
if(MSVC)
    target_compile_options(BoneCavityFillTests PRIVATE /utf-8)
endif()
vtk_module_autoinit(TARGETS BoneCavityFillTests MODULES ${VTK_LIBRARIES})
add_test(NAME BoneCavityFillTests COMMAND BoneCavityFillTests)

add_executable(MeshGeneratorTests tests/MeshGeneratorTests.cpp src/MeshGenerator.cpp)
target_include_directories(MeshGeneratorTests PRIVATE src)
target_link_libraries(MeshGeneratorTests PRIVATE Qt6::Core ${VTK_LIBRARIES})
vtk_module_autoinit(TARGETS MeshGeneratorTests MODULES ${VTK_LIBRARIES})
add_test(NAME MeshGeneratorTests COMMAND MeshGeneratorTests)

# Core (no Qt Widgets) test executable registered with CTest.
function(add_core_test name)
    add_executable(${name} ${ARGN})
    target_include_directories(${name} PRIVATE src)
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Concurrent ${VTK_LIBRARIES})
    if(MSVC)
        target_compile_options(${name} PRIVATE /utf-8)
    endif()
    vtk_module_autoinit(TARGETS ${name} MODULES ${VTK_LIBRARIES})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_core_test(SplintHeightmapTests tests/SplintHeightmapTests.cpp src/SplintHeightmapGenerator.cpp)
add_core_test(SplintDesignTests tests/SplintDesignTests.cpp
    src/SplintDesignCore.cpp src/SplintHeightmapGenerator.cpp)
add_core_test(SplintContourEditTests tests/SplintContourEditTests.cpp src/SplintContourEditCore.cpp)
add_core_test(SplintPreviewSchedulerTests tests/SplintPreviewSchedulerTests.cpp
    src/SplintPreviewScheduler.cpp src/SplintHeightmapGenerator.cpp)
add_core_test(ProjectSerializerTests tests/ProjectSerializerTests.cpp
    src/ProjectSerializer.cpp src/NrrdVolumeExporter.cpp src/SegmentationImporter.cpp
    src/SplintDesignCore.cpp src/SplintHeightmapGenerator.cpp src/CompositeBlockCore.cpp)
add_core_test(CompositeBlockTests tests/CompositeBlockTests.cpp src/CompositeBlockCore.cpp)
add_core_test(MeshRepairTests tests/MeshRepairTests.cpp src/MeshRepairCore.cpp src/CompositeBlockCore.cpp)
add_core_test(SegmentationProgressTests tests/SegmentationProgressTests.cpp src/SegmentationProgressCore.cpp
    src/AISegmentationService.cpp src/AISegmentationService.h)
add_core_test(MaskToObjectTests tests/MaskToObjectTests.cpp src/MaskToObjectCore.cpp
    src/MeshGenerator.cpp src/BoneCavityFill.cpp)
add_core_test(ModelWorkflowTests tests/ModelWorkflowTests.cpp src/ModelWorkflowCore.cpp)
add_core_test(OsteotomyCoreTests tests/OsteotomyCoreTests.cpp src/OsteotomyCore.cpp src/CompositeBlockCore.cpp)
target_link_libraries(ProjectSerializerTests PRIVATE Qt6::Gui)

# Splint generator timing with real or synthetic STL (not a CTest by default).
add_executable(SplintBenchmark tools/SplintBenchmark.cpp
    src/SplintHeightmapGenerator.cpp src/SplintDesignCore.cpp)
target_include_directories(SplintBenchmark PRIVATE src tests)
target_link_libraries(SplintBenchmark PRIVATE Qt6::Core ${VTK_LIBRARIES})
if(WIN32)
    target_link_libraries(SplintBenchmark PRIVATE psapi)
endif()
if(MSVC)
    target_compile_options(SplintBenchmark PRIVATE /utf-8)
endif()
vtk_module_autoinit(TARGETS SplintBenchmark MODULES ${VTK_LIBRARIES})

option(BUILD_PERF_TESTS "Register slow splint performance smoke tests" OFF)
if(BUILD_PERF_TESTS)
    add_test(NAME SplintPerfSmoke300k
        COMMAND SplintBenchmark --synthetic-triangles 300000 --repeat 1 --max-total-ms 30000)
endif()

set(SEGMENTATION_TEST_PYTHON "$ENV{DENTALSEGMENTATOR_PYTHON}" CACHE FILEPATH
    "Python with numpy, scipy and SimpleITK for segmentation regression tests")
if(SEGMENTATION_TEST_PYTHON)
    add_test(NAME BoneSegmentationPythonTests
        COMMAND "${SEGMENTATION_TEST_PYTHON}" -m unittest discover
            -s "${CMAKE_SOURCE_DIR}/tests" -p test_bone_segmentation.py -v)
endif()

option(BUILD_WORKSPACE_TESTS "Build Qt/VTK workspace regression tests" OFF)
if(BUILD_WORKSPACE_TESTS)
    set(WORKSPACE_TEST_SOURCES ${SOURCES})
    list(REMOVE_ITEM WORKSPACE_TEST_SOURCES src/main.cpp)
    add_executable(RepositionWorkspaceTests
        tests/RepositionWorkspaceTests.cpp ${WORKSPACE_TEST_SOURCES} ${HEADERS} ${RESOURCES})
    target_include_directories(RepositionWorkspaceTests PRIVATE src)
    target_compile_definitions(RepositionWorkspaceTests PRIVATE APP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(RepositionWorkspaceTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent ${VTK_LIBRARIES})
    if(MSVC)
        target_compile_options(RepositionWorkspaceTests PRIVATE /utf-8)
    endif()
    if(USE_GDCM)
        target_link_libraries(RepositionWorkspaceTests PRIVATE gdcmMSFF gdcmDICT gdcmCommon)
    endif()
    vtk_module_autoinit(TARGETS RepositionWorkspaceTests MODULES ${VTK_LIBRARIES})
    add_test(NAME RepositionWorkspaceTests COMMAND RepositionWorkspaceTests)

    add_executable(Mesh3DViewInteractionTests
        tests/Mesh3DViewInteractionTests.cpp src/Mesh3DView.cpp src/Mesh3DView.h)
    target_include_directories(Mesh3DViewInteractionTests PRIVATE src)
    target_link_libraries(Mesh3DViewInteractionTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets ${VTK_LIBRARIES})
    if(MSVC)
        target_compile_options(Mesh3DViewInteractionTests PRIVATE /utf-8)
    endif()
    vtk_module_autoinit(TARGETS Mesh3DViewInteractionTests MODULES ${VTK_LIBRARIES})
    add_test(NAME Mesh3DViewInteractionTests COMMAND Mesh3DViewInteractionTests)

    add_executable(SplintWorkspaceTests
        tests/SplintWorkspaceTests.cpp ${WORKSPACE_TEST_SOURCES} ${HEADERS} ${RESOURCES})
    target_include_directories(SplintWorkspaceTests PRIVATE src tests)
    target_compile_definitions(SplintWorkspaceTests PRIVATE APP_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    target_link_libraries(SplintWorkspaceTests PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Concurrent ${VTK_LIBRARIES})
    if(MSVC)
        target_compile_options(SplintWorkspaceTests PRIVATE /utf-8)
    endif()
    if(USE_GDCM)
        target_link_libraries(SplintWorkspaceTests PRIVATE gdcmMSFF gdcmDICT gdcmCommon)
    endif()
    vtk_module_autoinit(TARGETS SplintWorkspaceTests MODULES ${VTK_LIBRARIES})
    add_test(NAME SplintWorkspaceTests COMMAND SplintWorkspaceTests)
endif()

add_custom_command(TARGET DicomMPRViewer POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:DicomMPRViewer>/scripts"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/scripts/run_dental_segmentator.py"
            "$<TARGET_FILE_DIR:DicomMPRViewer>/scripts/run_dental_segmentator.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/scripts/run_standalone_dental_segmentator.py"
            "$<TARGET_FILE_DIR:DicomMPRViewer>/scripts/run_standalone_dental_segmentator.py"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/scripts/split_maxilla_mandible.py"
            "$<TARGET_FILE_DIR:DicomMPRViewer>/scripts/split_maxilla_mandible.py"
)

# ─── Windows: copy Qt + VTK DLLs next to the executable ──────────────────────
# Requires windeployqt to be on PATH (ships with Qt).
# Run manually after build:
#   windeployqt DicomMPRViewer.exe
# VTK DLLs must also be copied from VTK/bin or added to PATH.
