# Linux build of the core (no Qt Widgets) tests, for cloud sessions without the Windows toolchain.
# It does not replace the Windows build: the app, the workspace tests and the merge bar
# (constitution, principle 4) stay on DicomMPRViewer/CMakeLists.txt with MSVC, Qt 6.11 and VTK 9.5.2.
#
#   cmake -S DicomMPRViewer/tools/linux-core-tests -B build-linux-core -DVTK_DIR=<vtk 9.5.2>/lib/cmake/vtk-9.5
#   cmake --build build-linux-core --parallel
#   ctest --test-dir build-linux-core --output-on-failure
#
# Use the project's VTK, 9.5.2, built from source (git tag v9.5.2; only the modules below, no rendering).
# Ubuntu's libvtk9-dev is 9.1: the geometry differs by tenths of a millimetre and PlateTests and
# LeFortGuideTests fail on it while they pass on 9.5.2. Qt can be Ubuntu's qt6-base-dev (6.4): the cores
# only use QtCore and QtConcurrent.
cmake_minimum_required(VERSION 3.20)
project(MaxilloCoreTestsLinux LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Concurrent)
find_package(VTK 9.5 REQUIRED COMPONENTS
    CommonCore CommonDataModel CommonMath CommonTransforms
    FiltersCore FiltersGeneral FiltersSources FiltersModeling FiltersGeometry
    IOGeometry IOXML IOImage
    ImagingCore ImagingGeneral ImagingMath ImagingMorphological ImagingStencil)

set(APP ${CMAKE_CURRENT_SOURCE_DIR}/../..)

enable_testing()

# Same shape as add_core_test() in DicomMPRViewer/CMakeLists.txt; sources are relative to DicomMPRViewer/.
function(add_core_test name)
    set(files)
    foreach(f ${ARGN})
        list(APPEND files ${APP}/${f})
    endforeach()
    add_executable(${name} ${files})
    target_include_directories(${name} PRIVATE ${APP}/src)
    # Qt's `emit` macro breaks oneTBB's headers, which libstdc++ pulls in through <execution>.
    target_compile_definitions(${name} PRIVATE QT_NO_EMIT)
    target_link_libraries(${name} PRIVATE Qt6::Core Qt6::Concurrent ${VTK_LIBRARIES})
    vtk_module_autoinit(TARGETS ${name} MODULES ${VTK_LIBRARIES})
    add_test(NAME ${name} COMMAND ${name})
endfunction()

set(GUIDE_CORE src/GuideDesignCore.cpp src/GuideBaseCore.cpp src/CutSlotCore.cpp src/WrapCore.cpp
    src/OsteotomyCore.cpp src/ImplicitCore.cpp src/MeshRepairCore.cpp src/CompositeBlockCore.cpp)

add_core_test(ImplicitCoreTests tests/ImplicitCoreTests.cpp src/ImplicitCore.cpp
    src/MeshRepairCore.cpp src/CompositeBlockCore.cpp)
add_core_test(OsteotomyCoreTests tests/OsteotomyCoreTests.cpp src/OsteotomyCore.cpp src/CompositeBlockCore.cpp)
add_core_test(GuideDesignTests tests/GuideDesignTests.cpp ${GUIDE_CORE})
add_core_test(PlateTests tests/PlateTests.cpp src/PlateCore.cpp ${GUIDE_CORE})
add_core_test(LeFortGuideTests tests/LeFortGuideTests.cpp src/LeFortGuideCore.cpp src/PlateCore.cpp ${GUIDE_CORE})
add_core_test(LeFortMotionTests tests/LeFortMotionTests.cpp src/LeFortMotionCore.cpp src/OsteotomyCore.cpp
    src/CompositeBlockCore.cpp)
add_core_test(LeFortHoleTests tests/LeFortHoleTests.cpp src/LeFortHoleCore.cpp src/LeFortMotionCore.cpp
    src/PlateCore.cpp ${GUIDE_CORE})
add_core_test(GuidePlanTests tests/GuidePlanTests.cpp src/GuidePlanCore.cpp src/PlateCore.cpp ${GUIDE_CORE})
