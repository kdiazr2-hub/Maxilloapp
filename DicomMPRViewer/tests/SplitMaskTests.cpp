#include "SplitMaskCore.h"

#include <atomic>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include <vtkImageData.h>

namespace
{
void require(bool condition, const std::string& message)
{
    if (!condition)
        throw std::runtime_error(message);
}

struct Phantom
{
    vtkSmartPointer<vtkImageData> mask;
    vtkSmartPointer<vtkImageData> hu;
};

Phantom makePhantom(int nx, int ny, int nz, const std::vector<short>& hu, double sx, double sy, double sz)
{
    Phantom phantom;
    phantom.hu = vtkSmartPointer<vtkImageData>::New();
    phantom.hu->SetDimensions(nx, ny, nz);
    phantom.hu->SetSpacing(sx, sy, sz);
    phantom.hu->AllocateScalars(VTK_SHORT, 1);
    std::memcpy(phantom.hu->GetScalarPointer(), hu.data(), hu.size() * sizeof(short));
    phantom.mask = vtkSmartPointer<vtkImageData>::New();
    phantom.mask->SetDimensions(nx, ny, nz);
    phantom.mask->SetSpacing(sx, sy, sz);
    phantom.mask->AllocateScalars(VTK_UNSIGNED_CHAR, 1);
    auto* labels = static_cast<unsigned char*>(phantom.mask->GetScalarPointer());
    const auto* values = static_cast<const short*>(phantom.hu->GetScalarPointer());
    for (size_t i = 0; i < hu.size(); ++i)
        labels[i] = values[i] > 0 ? 1 : 0;
    return phantom;
}

unsigned char at(vtkImageData* image, int x, int y, int z)
{
    int extent[6] = {};
    image->GetExtent(extent);
    const int nx = extent[1] - extent[0] + 1;
    const int ny = extent[3] - extent[2] + 1;
    const auto* data = static_cast<const unsigned char*>(image->GetScalarPointer());
    return data[static_cast<size_t>(x + nx * (y + ny * z))];
}

Phantom twoBlocks(double sx, double sy, double sz)
{
    const int nx = 16, ny = 6, nz = 6;
    std::vector<short> hu(static_cast<size_t>(nx * ny * nz), 0);
    auto fill = [&](int x0, int x1, short value) {
        for (int z = 1; z <= 4; ++z)
            for (int y = 1; y <= 4; ++y)
                for (int x = x0; x <= x1; ++x)
                    hu[static_cast<size_t>(x + nx * (y + ny * z))] = value;
    };
    fill(2, 6, 1500);
    fill(8, 12, 1500);
    hu[static_cast<size_t>(7 + nx * (3 + ny * 3))] = 400;
    return makePhantom(nx, ny, nz, hu, sx, sy, sz);
}

SplitMarker markAt(int region, double x, double y, double z)
{
    SplitMarker marker;
    marker.region = region;
    marker.centerMm[0] = x;
    marker.centerMm[1] = y;
    marker.centerMm[2] = z;
    marker.radiusMm = 0.6;
    return marker;
}

void requireBlocksSeparated(vtkImageData* split, const std::string& where)
{
    require(at(split, 3, 2, 2) == 1, where + ": the first block changed region");
    require(at(split, 10, 2, 2) == 2, where + ": the second block changed region");
    require(at(split, 4, 3, 3) == 1, where + ": the first block was mixed");
    require(at(split, 11, 3, 3) == 2, where + ": the second block was mixed");
    const unsigned char bridge = at(split, 7, 3, 3);
    require(bridge == 1 || bridge == 2, where + ": the cut did not fall on the bridge");
    require(at(split, 0, 0, 0) == 0, where + ": air was labelled");
}

void testTheCutFallsOnTheBridge()
{
    auto phantom = twoBlocks(1.0, 1.0, 1.0);
    const SplitMarker markers[] = {markAt(0, 4, 3, 3), markAt(1, 10, 3, 3)};
    SplitRequest request;
    request.markers = markers;
    request.markerCount = 2;
    auto split = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(split != nullptr, "the bridge case produced nothing");
    requireBlocksSeparated(split, "isotropic");
}

void testTheSameCutAtAnisotropicSpacing()
{
    auto phantom = twoBlocks(0.4, 0.4, 1.0);
    const SplitMarker markers[] = {
        markAt(0, 4 * 0.4, 3 * 0.4, 3.0),
        markAt(1, 10 * 0.4, 3 * 0.4, 3.0)};
    SplitRequest request;
    request.markers = markers;
    request.markerCount = 2;
    auto split = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(split != nullptr, "the anisotropic case produced nothing");
    requireBlocksSeparated(split, "anisotropic");
}

void testAMarkOutsideTheMaskDoesNothing()
{
    auto phantom = twoBlocks(1.0, 1.0, 1.0);
    const SplitMarker inside[] = {markAt(0, 4, 3, 3)};
    SplitRequest onlyInside;
    onlyInside.regionCount = 2;
    onlyInside.markers = inside;
    onlyInside.markerCount = 1;
    auto expected = SplitMaskCore::split(phantom.mask, phantom.hu, onlyInside);

    SplitMarker both[2] = {markAt(0, 4, 3, 3), markAt(1, 0, 0, 0)};
    both[1].radiusMm = 0.4;
    SplitRequest request;
    request.markers = both;
    request.markerCount = 2;
    auto split = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(expected && split, "the outside-mark case produced nothing");
    require(std::memcmp(expected->GetScalarPointer(), split->GetScalarPointer(),
                        static_cast<size_t>(split->GetNumberOfPoints())) == 0,
            "a mark outside the mask changed the split");
}

void testAnUnmarkedIslandStaysUnassigned()
{
    const int n = 24;
    std::vector<short> hu(static_cast<size_t>(n * n * 2), 0);
    auto fill = [&](int x0, int x1) {
        for (int x = x0; x <= x1; ++x)
            hu[static_cast<size_t>(x + n * (2 + n * 1))] = 1500;
    };
    fill(1, 3);
    fill(8, 9);
    fill(20, 22);
    auto phantom = makePhantom(n, n, 2, hu, 1, 1, 1);
    const SplitMarker markers[] = {markAt(0, 2, 2, 1), markAt(1, 21, 2, 1)};
    SplitRequest request;
    request.markers = markers;
    request.markerCount = 2;
    auto split = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(split && at(split, 8, 2, 1) == 3, "an unmarked island was assigned");

    request.assignIslandsToNearest = true;
    auto nearest = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(nearest && at(nearest, 8, 2, 1) == 1, "the island did not join the nearest region");
    require(at(nearest, 21, 2, 1) == 2, "the far block changed when the island was assigned");
}

void testTheOriginMaskIsNotWritten()
{
    auto phantom = twoBlocks(1.0, 1.0, 1.0);
    std::vector<unsigned char> before(static_cast<size_t>(phantom.mask->GetNumberOfPoints()));
    std::memcpy(before.data(), phantom.mask->GetScalarPointer(), before.size());
    const SplitMarker markers[] = {markAt(0, 4, 3, 3), markAt(1, 10, 3, 3)};
    SplitRequest request;
    request.markers = markers;
    request.markerCount = 2;
    auto split = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(split != nullptr, "origin check produced nothing");
    require(std::memcmp(before.data(), phantom.mask->GetScalarPointer(), before.size()) == 0,
            "the origin mask was modified");
}

void testAddingAMarkChangesOnlyItsZoneAndUndoRestores()
{
    const int nx = 32, ny = 3, nz = 3;
    std::vector<short> hu(static_cast<size_t>(nx * ny * nz), 0);
    for (int x = 0; x < nx; ++x)
        hu[static_cast<size_t>(x + nx * (1 + ny * 1))] = 1500;
    auto phantom = makePhantom(nx, ny, nz, hu, 1, 1, 1);
    const SplitMarker original[] = {markAt(0, 1, 1, 1), markAt(1, 30, 1, 1)};
    SplitRequest request;
    request.markers = original;
    request.markerCount = 2;
    auto first = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(first && at(first, 1, 1, 1) == 1 && at(first, 30, 1, 1) == 2, "the bar did not split");
    require(at(first, 8, 1, 1) == 1, "the left side was not the first region");

    const SplitMarker extra[] = {markAt(0, 1, 1, 1), markAt(1, 30, 1, 1), markAt(1, 8, 1, 1)};
    request.markers = extra;
    request.markerCount = 3;
    auto second = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(second && at(second, 8, 1, 1) == 2, "the new mark did not take its voxel");
    require(at(second, 1, 1, 1) == 1 && at(second, 30, 1, 1) == 2, "the new mark changed the ends");

    request.markers = original;
    request.markerCount = 2;
    auto restored = SplitMaskCore::split(phantom.mask, phantom.hu, request);
    require(restored && std::memcmp(first->GetScalarPointer(), restored->GetScalarPointer(),
                                    static_cast<size_t>(first->GetNumberOfPoints())) == 0,
            "removing the mark did not restore the split");
}

void testCancelReturnsNothingAndKeepsTheOrigin()
{
    auto phantom = twoBlocks(1.0, 1.0, 1.0);
    std::vector<unsigned char> before(static_cast<size_t>(phantom.mask->GetNumberOfPoints()));
    std::memcpy(before.data(), phantom.mask->GetScalarPointer(), before.size());
    const SplitMarker markers[] = {markAt(0, 4, 3, 3), markAt(1, 10, 3, 3)};
    SplitRequest request;
    request.markers = markers;
    request.markerCount = 2;
    std::atomic<bool> cancel{true};
    auto split = SplitMaskCore::split(phantom.mask, phantom.hu, request, &cancel);
    require(!split, "a cancelled split returned a mask");
    require(std::memcmp(before.data(), phantom.mask->GetScalarPointer(), before.size()) == 0,
            "cancel wrote the origin mask");
}
}

int main()
{
    try {
        testTheCutFallsOnTheBridge();
        testTheSameCutAtAnisotropicSpacing();
        testAMarkOutsideTheMaskDoesNothing();
        testAnUnmarkedIslandStaysUnassigned();
        testTheOriginMaskIsNotWritten();
        testAddingAMarkChangesOnlyItsZoneAndUndoRestores();
        testCancelReturnsNothingAndKeepsTheOrigin();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "SplitMaskTests ok\n";
    return 0;
}
