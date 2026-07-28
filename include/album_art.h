#pragma once

#include <stdint.h>
#include <stddef.h>
#include <lvgl.h>

// =============================================================
// Album Art — Decodificación de carátulas JPEG/PNG
// =============================================================

// Inicializa el widget de imagen LVGL dentro del panel padre
void albumArt_init(lv_obj_t* parent);

// Decodifica la carátula desde datos crudos del frame APIC.
// Detecta automáticamente JPEG o PNG, escala a ALBUM_ART_SIZE y actualiza el widget.
// Siempre libera imgData al finalizar (independientemente del resultado).
void albumArt_decode(uint8_t* imgData, size_t imgSize);
