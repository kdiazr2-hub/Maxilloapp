import os
import sys
import traceback


def log(message):
    print(message, flush=True)


def main():
    if len(sys.argv) < 3:
        raise RuntimeError(
            "Uso: run_dental_segmentator.py input.nrrd output_segmentation.nrrd"
        )

    input_path = os.path.abspath(sys.argv[-2])
    output_path = os.path.abspath(sys.argv[-1])

    if not os.path.exists(input_path):
        raise RuntimeError(f"No existe input: {input_path}")

    log("10% Cargando volumen NRRD en 3D Slicer...")
    loaded = slicer.util.loadVolume(input_path, returnNode=True)
    if isinstance(loaded, tuple):
        ok, volume_node = loaded
    else:
        ok, volume_node = True, loaded
    if not ok or volume_node is None:
        raise RuntimeError("No se pudo cargar el volumen en Slicer.")

    log("25% Preparando DentalSegmentator...")
    if not hasattr(slicer.modules, "dentalsegmentator"):
        raise RuntimeError(
            "DentalSegmentator no esta instalado o no esta habilitado en 3D Slicer."
        )

    segmentation_node = slicer.mrmlScene.AddNewNodeByClass(
        "vtkMRMLSegmentationNode", "DentalSegmentator_AI"
    )
    segmentation_node.CreateDefaultDisplayNodes()

    logic = slicer.modules.dentalsegmentator.logic()

    log("35% Ejecutando DentalSegmentator...")
    # La API interna de DentalSegmentator puede variar por version.
    # Ajustar este bloque si la extension instalada usa nombres distintos.
    if hasattr(logic, "process"):
        logic.process(volume_node, segmentation_node)
    elif hasattr(logic, "run"):
        logic.run(volume_node, segmentation_node)
    elif hasattr(logic, "apply"):
        logic.apply(volume_node, segmentation_node)
    else:
        raise RuntimeError(
            "No se encontro metodo compatible en DentalSegmentator logic() "
            "(process/run/apply). Revise la API de la version instalada."
        )

    log("80% Exportando segmentacion a labelmap...")
    labelmap_node = slicer.mrmlScene.AddNewNodeByClass(
        "vtkMRMLLabelMapVolumeNode", "DentalSegmentator_Labelmap"
    )
    labelmap_node.SetOrigin(volume_node.GetOrigin())
    labelmap_node.SetSpacing(volume_node.GetSpacing())

    segmentation_logic = slicer.modules.segmentations.logic()
    segmentation_logic.ExportAllSegmentsToLabelmapNode(
        segmentation_node,
        labelmap_node,
        slicer.vtkSegmentation.EXTENT_REFERENCE_GEOMETRY,
    )

    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    if not slicer.util.saveNode(labelmap_node, output_path):
        raise RuntimeError(f"No se pudo guardar labelmap: {output_path}")

    log(f"100% Labelmap guardado: {output_path}")


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        log("ERROR: " + str(exc))
        traceback.print_exc()
        sys.exit(1)
