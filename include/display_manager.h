#pragma once

// =============================================================
// Display Manager — Pantalla IPS ST7789 + LVGL
// =============================================================

// Inicializa la pantalla IPS, reconfigura SPI con MISO, e inicializa LVGL.
// Debe llamarse después de crear el spiMutex.
void display_init();
