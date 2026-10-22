#include "ThresholdMaskCore.h"

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

vtkSmartPointer<vtkImageData> volumeFrom(int nx, int ny, int nz, const std::vector<short>& values)
{
    auto image = vtkSmartPointer<vtkImageData>::New();
    image->SetDimensions(nx, ny, nz);
    image->SetSpacing(0.5, 0.4, 0.7);
    image->AllocateScalars(VTK_SHORT, 1);
    auto* ptr = static_cast<short*>(image->GetScalarPointer());
    for (size_t i = 0; i < values.size(); ++i)
        ptr[i] = values[i];
    return image;
}

unsigned char at(vtkImageData* image, int x, int y, int z)
{
    return static_cast<unsigned char>(image->GetScalarComponentAsDouble(x, y, z, 0));
}

void testInclusiveWindow()
{
    auto volume = volumeFrom(3, 1, 1, {225, 226, 3071});
    ThresholdRequest request;
    request.minHu = 226;
    request.maxHu = 3071;
    request.label = 1;
    std::int64_t voxels = 0;
    auto mask = ThresholdMaskCore::build(volume, request, &voxels);
    require(mask && voxels == 2, "the window did not keep its two endpoints");
    require(at(mask, 0, 0, 0) == 0, "a voxel below the window was labelled");
    require(at(mask, 1, 0, 0) == 1 && at(mask, 2, 0, 0) == 1, "an endpoint was left out");
}

void testHistogramSpansTheStudy()
{
    auto volume = volumeFrom(4, 1, 1, {-100, 0, 0, 100});
    const auto histogram = ThresholdMaskCore::histogram(volume, 4);
    require(histogram.voxels == 4, "the histogram dropped a voxel");
    require(histogram.minimum == -100.0 && histogram.maximum == 100.0, "the histogram range is not the study");
    std::int64_t sum = 0;
    for (auto count : histogram.counts) sum += count;
    require(sum == 4, "the bins do not add up to the volume");
}

void testPaintSliceDoesNotTouchTheOthers()
{
    std::vector<short> values(27, 0);
    values[1 * 9 + 1 * 3 + 1] = 150; // z=1, y=1, x=1
    values[0 * 9 + 1 * 3 + 1] = 150; // z=0
    auto volume = volumeFrom(3, 3, 3, values);
    auto preview = ThresholdMaskCore::blank(volume);
    ThresholdMaskCore::paintSlice(volume, preview, 2, 1, 100, 200, 4);
    require(at(preview, 1, 1, 1) == 4, "the voxel on the current slice was not painted");
    require(at(preview, 1, 1, 0) == 0, "a voxel on another slice was painted");
    require(at(preview, 0, 0, 1) == 0, "a voxel outside the window was painted");
}

void testKeepLargestThenFillTheHole()
{
    // 7³. A hollow 3³ shell around the centre, plus one stray voxel on the corner.
    std::vector<short> values(7 * 7 * 7, -1000);
    for (int z = 2; z <= 4; ++z)
        for (int y = 2; y <= 4; ++y)
            for (int x = 2; x <= 4; ++x)
                values[z * 49 + y * 7 + x] = 800;
    values[3 * 49 + 3 * 7 + 3] = -1000; // enclosed hole
    values[0] = 800;                    // island
    auto volume = volumeFrom(7, 7, 7, values);

    ThresholdRequest request;
    request.minHu = 226;
    request.maxHu = 3071;
    request.keepLargest = true;
    request.fillHoles = true;
    auto mask = ThresholdMaskCore::build(volume, request);
    require(mask, "the shell produced no mask");
    require(at(mask, 0, 0, 0) == 0, "the island survived keep-largest");
    require(at(mask, 3, 3, 3) != 0, "the enclosed hole was left open");
    require(at(mask, 2, 2, 2) != 0, "the shell itself was removed");
    require(at(mask, 1, 1, 1) == 0, "outside air was filled");
}

void testStampReplacesBoneAndKeepsTeeth()
{
    auto destination = volumeFrom(2, 1, 1, {0, 0});
    auto* labels = static_cast<unsigned char*>(ThresholdMaskCore::unsignedLabelmap(destination)->GetScalarPointer());
    // unsignedLabelmap of a short image is a copy. Build the destination directly.
    auto existing = ThresholdMaskCore::blank(destination);
    auto* dst = static_cast<unsigned char*>(existing->GetScalarPointer());
    dst[0] = 5; // old maxilla, outside the new window
    dst[1] = 3; // a tooth that also reads as bone
    auto mask = ThresholdMaskCore::blank(destination);
    auto* src = static_cast<unsigned char*>(mask->GetScalarPointer());
    src[0] = 0;
    src[1] = 1;
    const int clear[] = {1, 5, 6};
    const auto written = ThresholdMaskCore::stamp(existing, mask, 1, clear, 3);
    require(written == 0, "the tooth voxel was overwritten");
    require(dst[0] == 0, "old bone outside the window was kept");
    require(dst[1] == 3, "the tooth label was cleared");
    (void)labels;
}

void testStampMaskKeepsBothJaws()
{
    auto geometry = volumeFrom(3, 1, 1, {0, 0, 0});
    auto existing = ThresholdMaskCore::blank(geometry);
    auto* dst = static_cast<unsigned char*>(existing->GetScalarPointer());
    dst[0] = 5;
    dst[1] = 6;
    dst[2] = 3;
    auto mask = ThresholdMaskCore::blank(geometry);
    auto* src = static_cast<unsigned char*>(mask->GetScalarPointer());
    src[0] = 5;
    src[1] = 6;
    const int clear[] = {1, 5, 6};
    const auto written = ThresholdMaskCore::stampMask(existing, mask, clear, 3);
    require(written == 2, "both jaws were not written");
    require(dst[0] == 5 && dst[1] == 6, "the jaw labels were flattened");
    require(dst[2] == 3, "the tooth was cleared");
}

void testSeparateMandibleBreaksTheOcclusion()
{
    const int nx = 50, ny = 75, nz = 50;
    std::vector<short> values(static_cast<size_t>(nx * ny * nz), 0);
    auto set = [&](int x, int y, int z) {
        values[static_cast<size_t>(z * ny * nx + y * nx + x)] = 800;
    };
    auto fill = [&](int x0, int x1, int y0, int y1, int z0, int z1) {
        for (int z = z0; z <= z1; ++z)
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x)
                    set(x, y, z);
    };
    fill(6, 44, 30, 55, 22, 45); // cranium: posterior and superior
    fill(8, 42, 4, 14, 4, 16);   // mandible: anterior and inferior
    fill(18, 34, 60, 72, 2, 18); // cervical column, behind and below the skull
    for (int y = 56; y <= 59; ++y) set(26, y, 22); // thin neck up to the cranium
    for (int y = 15; y <= 30; ++y) set(22, y, 16);
    for (int z = 17; z <= 22; ++z) set(22, 30, z); // one-voxel bridge, the occlusion
    fill(0, 1, 0, ny - 1, 0, nz - 1); // a sheet that never touches the skull

    auto volume = volumeFrom(nx, ny, nz, values);
    volume->SetSpacing(1.0, 1.0, 1.0);
    ThresholdRequest request;
    request.minHu = 200;
    request.maxHu = 3000;
    request.label = 1;
    auto mask = ThresholdMaskCore::build(volume, request, nullptr);
    require(mask && ThresholdMaskCore::separateMandible(mask), "the mandible was not separated");
    require(at(mask, 22, 10, 10) == 6, "the mandible body is not the mandible");
    require(at(mask, 22, 40, 30) == 5, "the cranium is not the maxilla");
    require(at(mask, 26, 66, 10) == 0, "the cervical column stayed in the skull");
    require(at(mask, 0, 10, 10) == 1, "a disconnected sheet was absorbed");
}

void testSealClosesABoneGapAndLeavesAir()
{
    auto volume = volumeFrom(5, 1, 1, {800, 200, 800, -1000, 800});
    volume->SetSpacing(1.0, 1.0, 1.0);
    auto mask = ThresholdMaskCore::blank(volume);
    auto* labels = static_cast<unsigned char*>(mask->GetScalarPointer());
    labels[0] = 5;
    labels[2] = 5;
    labels[4] = 5;
    ThresholdMaskCore::sealThinBone(mask, volume, 1.2, 120.0);
    require(labels[1] == 5, "a one-voxel bone gap stayed open");
    require(labels[3] == 0, "air between bone was filled");
}

void testASingleBoneDoesNotSplit()
{
    const int n = 20;
    std::vector<short> values(static_cast<size_t>(n * n * n), 0);
    for (int z = 2; z < 18; ++z)
        for (int y = 2; y < 18; ++y)
            for (int x = 2; x < 18; ++x)
                values[static_cast<size_t>(z * n * n + y * n + x)] = 800;
    auto volume = volumeFrom(n, n, n, values);
    volume->SetSpacing(1.0, 1.0, 1.0);
    ThresholdRequest request;
    request.minHu = 200;
    request.maxHu = 3000;
    request.label = 1;
    auto mask = ThresholdMaskCore::build(volume, request, nullptr);
    require(mask && !ThresholdMaskCore::separateMandible(mask), "one bone was split in two");
    require(at(mask, 10, 10, 10) == 1, "the bone label changed");
}

void testStampWritesOntoEmptiedBone()
{
    auto geometry = volumeFrom(1, 1, 1, {0});
    auto existing = ThresholdMaskCore::blank(geometry);
    static_cast<unsigned char*>(existing->GetScalarPointer())[0] = 6;
    auto mask = ThresholdMaskCore::blank(geometry);
    static_cast<unsigned char*>(mask->GetScalarPointer())[0] = 1;
    const int clear[] = {1, 5, 6};
    const auto written = ThresholdMaskCore::stamp(existing, mask, 1, clear, 3);
    require(written == 1, "the new window did not replace the old mandible");
    require(static_cast<unsigned char*>(existing->GetScalarPointer())[0] == 1, "the voxel is not bone");
}
}

int main()
{
    try {
        testInclusiveWindow();
        testHistogramSpansTheStudy();
        testPaintSliceDoesNotTouchTheOthers();
        testKeepLargestThenFillTheHole();
        testStampReplacesBoneAndKeepsTeeth();
        testStampWritesOntoEmptiedBone();
        testStampMaskKeepsBothJaws();
        testSeparateMandibleBreaksTheOcclusion();
        testSealClosesABoneGapAndLeavesAir();
        testASingleBoneDoesNotSplit();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << "ThresholdMaskTests ok\n";
    return 0;
}
