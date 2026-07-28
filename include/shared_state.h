#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// =============================================================
// Estado compartido entre tareas FreeRTOS
// Las instancias se definen en main.cpp
// =============================================================

// Estado de reproducción de audio (escrito por audioTask, leído por uiTask)
struct AudioState {
    volatile uint32_t current;    // Tiempo actual en segundos
    volatile uint32_t duration;   // Duración total en segundos
    volatile bool     playing;    // ¿Está reproduciendo?
};

// Estado de volumen (compartido entre audioTask y uiTask)
struct VolumeState {
    int           level;          // Nivel actual (0-VOL_MAX)
    volatile bool changed;        // Flag: audioTask la pone true, uiTask la consume
    unsigned long lastButtonTime; // Timestamp para debounce
};

// --- Instancias globales (definidas en main.cpp) ---
extern AudioState        g_audioState;
extern VolumeState       g_volumeState;
extern SemaphoreHandle_t spiMutex;
extern QueueHandle_t     metadataQueue;
