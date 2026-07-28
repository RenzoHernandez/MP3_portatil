#pragma once

#include <stdint.h>
#include <stddef.h>

// =============================================================
// Configuración de Hardware — ESP32-S3 Super Mini N4R2
// Reproductor MP3 Portátil
// =============================================================

// --- Bus SPI compartido (Pantalla IPS + Micro SD) ---
#define SPI_SCK   1
#define SPI_MISO  6   // Solo lo usa la micro SD (lectura)
#define SPI_MOSI  2

// --- Micro SD ---
#define SD_CS     7

// --- Pantalla IPS ST7789 (1.69", 240x280) ---
#define TFT_CS    5
#define TFT_DC    4
#define TFT_RST   3

// --- I2S Audio (MAX98357A) ---
#define I2S_BCLK  9
#define I2S_LRC   8
#define I2S_DOUT  10

// --- Encoder Rotatorio ---
#define ENC_PIN_A     12
#define ENC_PIN_B     11
#define ENC_BTN_PIN   13

// --- Dimensiones de pantalla ---
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 280

// --- Carátulas ---
constexpr int    ALBUM_ART_SIZE  = 150;       // Tamaño de salida en píxeles (cuadrado)
constexpr size_t MAX_COVER_SIZE  = 250000;    // Límite de tamaño de carátula (bytes)

// --- Audio (ESP32-audioI2S usa rango 0-21) ---
constexpr int VOL_MAX     = 21;
constexpr int VOL_DEFAULT = 5;
constexpr int VOL_STEP    = 1;

// --- Timings ---
constexpr unsigned long ENC_LONG_PRESS_MS     = 1000;  // Umbral de pulsación larga
constexpr unsigned long ENC_BTN_DEBOUNCE_MS   = 50;    // Debounce del botón del encoder
constexpr unsigned long VOL_TOAST_DURATION_MS = 3000;  // Tiempo visible antes del fade out
constexpr unsigned long VOL_FADE_DURATION_MS  = 300;   // Duración de la animación de fade

// --- Velocidad SPI de la tarjeta SD ---
constexpr uint32_t SD_SPI_SPEED = 80000000;   // 80 MHz

// --- FreeRTOS: Configuración de tareas ---
constexpr uint32_t AUDIO_TASK_STACK    = 16384;  // Stack en bytes
constexpr uint32_t UI_TASK_STACK       = 16384;
constexpr int      AUDIO_TASK_PRIORITY = 2;      // Alta: audio es crítico en tiempo real
constexpr int      UI_TASK_PRIORITY    = 1;
constexpr int      AUDIO_TASK_CORE     = 0;
constexpr int      UI_TASK_CORE        = 1;
