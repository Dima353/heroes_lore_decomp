#pragma once
// Minimal native diagnostics; kept independent of SDL for the combat library.
void psp_diag_reset();
void psp_diag_stage(const char* stage, int first=0, int second=0);
void psp_diag_audio_stage(unsigned stage);
