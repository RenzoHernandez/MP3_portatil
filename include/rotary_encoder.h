#pragma once

#include "Audio.h"

// Inicializa el encoder rotatorio y su botón
void encoder_init();

// Actualiza el estado del encoder (giro y botón)
// Debe llamarse frecuentemente desde el audioTask
void encoder_update(Audio& audio);
