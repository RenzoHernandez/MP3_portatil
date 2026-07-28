#pragma once

#include "Audio.h"

// =============================================================
// Volume Control — Botones, Debounce, Toast con animaciones
// =============================================================

// Configura los pines GPIO de los botones de volumen
void volume_init();

// Lee los botones y ajusta el volumen del audio (llamar desde audioTask)
void volume_readButtons(Audio& audio);

// Gestiona la visibilidad del toast de volumen en la UI (llamar desde uiTask)
void volumeToast_update();
