#pragma once

#include <stdint.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>

// =============================================================
// Estado compartido entre tareas FreeRTOS
// Las instancias se definen en main.cpp
// =============================================================

// Acciones del encoder pendientes para la UI
enum EncoderAction {
    ENC_ACTION_NONE = 0,
    ENC_ACTION_PLAY_PAUSE,
    ENC_ACTION_GO_FILES,
    ENC_ACTION_GO_PLAYER
};

// Pantalla actual de la UI (escrita por uiTask, leída por audioTask)
enum AppScreen {
    SCREEN_PLAYER = 0,
    SCREEN_FILES
};

// Estado del encoder
struct EncoderState {
    volatile EncoderAction pendingAction;
    volatile AppScreen     currentScreen;
    volatile int           fileSelectedIndex;
    volatile bool          selectionChanged;
};

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
};

// --- Instancias globales (definidas en main.cpp) ---
extern AudioState        g_audioState;
extern VolumeState       g_volumeState;
extern EncoderState      g_encoderState;
extern SemaphoreHandle_t spiMutex;
extern QueueHandle_t     metadataQueue;
