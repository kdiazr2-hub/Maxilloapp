#pragma once

// The program has not been validated as a medical device. Every place that hands a result to someone — the
// splash, the status bar and the reports exported with the files to fabricate — says so in the same words.
namespace ClinicalNotice {

inline constexpr const char* kShort = "Uso en investigación · no validado para uso clínico";

inline constexpr const char* kReport =
    "AVISO: software de investigación, no validado como dispositivo médico. Los resultados deben ser "
    "revisados por el cirujano responsable antes de fabricar o usar cualquier pieza.";

} // namespace ClinicalNotice
