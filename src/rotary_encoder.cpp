#include "rotary_encoder.h"
#include "config.h"
#include "shared_state.h"
#include "audio_player.h"
#include <ESP32Encoder.h>

static ESP32Encoder encoder;
static int32_t lastEncoderCount = 0;
static unsigned long btnPressTime = 0;
static bool btnPressed = false;
static unsigned long lastBtnReadTime = 0;

void encoder_init() {
    ESP32Encoder::useInternalWeakPullResistors = puType::up;
    encoder.attachSingleEdge(ENC_PIN_A, ENC_PIN_B);
    encoder.setCount(0);

    pinMode(ENC_BTN_PIN, INPUT_PULLUP);
    
    Serial.println("[Setup] Encoder rotatorio inicializado.");
}

void encoder_update(Audio& audio) {
    // 1. Leer giro del encoder
    int32_t currentCount = encoder.getCount();
    if (currentCount != lastEncoderCount) {
        int32_t diff = currentCount - lastEncoderCount;
        lastEncoderCount = currentCount;

        switch (g_encoderState.currentScreen) {
            case SCREEN_PLAYER:
                {
                    // El giro del encoder ajusta el volumen
                    int oldLevel = g_volumeState.level;
                    if (diff > 0) { // Horario -> Subir volumen
                        g_volumeState.level = min(VOL_MAX, g_volumeState.level + VOL_STEP);
                    } else if (diff < 0) { // Antihorario -> Bajar volumen
                        g_volumeState.level = max(0, g_volumeState.level - VOL_STEP);
                    }

                    if (g_volumeState.level != oldLevel) {
                        audio.setVolume(g_volumeState.level);
                        g_volumeState.changed = true; // Para el toast en uiTask
                        Serial.printf("[Audio] Volumen: %d/%d\n", g_volumeState.level, VOL_MAX);
                    }
                }
                break;

            case SCREEN_FILES:
                int playlistCount = audioPlayer_getPlaylist().count;
                if (playlistCount > 0) {
                    if (diff > 0) {
                        g_encoderState.fileSelectedIndex = (g_encoderState.fileSelectedIndex + 1) % playlistCount;
                    } else if (diff < 0) {
                        g_encoderState.fileSelectedIndex = (g_encoderState.fileSelectedIndex - 1 + playlistCount) % playlistCount;
                    }
                    g_encoderState.selectionChanged = true;
                    Serial.printf("[Encoder] Archivos: Indice seleccionado = %d\n", g_encoderState.fileSelectedIndex);
                }
                break;
        }
    }

    // 2. Leer estado del botón (con polling y debounce no bloqueante)
    unsigned long now = millis();
    if (now - lastBtnReadTime >= ENC_BTN_DEBOUNCE_MS) {
        bool btnState = (digitalRead(ENC_BTN_PIN) == LOW); // LOW significa presionado con PULLUP

        if (btnState && !btnPressed) {
            // Flanco de bajada (botón presionado)
            btnPressed = true;
            btnPressTime = now;
        } 
        else if (!btnState && btnPressed) {
            // Flanco de subida (botón soltado)
            btnPressed = false;
            unsigned long pressDuration = now - btnPressTime;

            switch (g_encoderState.currentScreen) {
                case SCREEN_PLAYER:
                    if (pressDuration >= ENC_LONG_PRESS_MS) {
                        // Pulsación larga: ir a archivos
                        Serial.println("[Encoder] Player: Pulsación larga -> Archivos");
                        g_encoderState.pendingAction = ENC_ACTION_GO_FILES;
                    } else {
                        // Pulsación corta: play/pause
                        Serial.println("[Encoder] Player: Pulsación corta -> Play/Pause");
                        audio.pauseResume();
                        g_encoderState.pendingAction = ENC_ACTION_PLAY_PAUSE;
                    }
                    break;

                case SCREEN_FILES:
                    if (pressDuration >= ENC_LONG_PRESS_MS) {
                        // Pulsación larga: volver a reproductor
                        Serial.println("[Encoder] Archivos: Pulsación larga -> Reproductor");
                        g_encoderState.pendingAction = ENC_ACTION_GO_PLAYER;
                    } else {
                        // Pulsación corta: Seleccionar archivo o entrar a carpeta
                        Playlist& pl = audioPlayer_getPlaylist();
                        if (pl.entries[g_encoderState.fileSelectedIndex].isDir) {
                            Serial.println("[Encoder] Archivos: Pulsación corta -> Entrar a carpeta");
                            audioPlayer_enterDir(g_encoderState.fileSelectedIndex);
                        } else {
                            Serial.println("[Encoder] Archivos: Pulsación corta -> Reproducir seleccion");
                            audioPlayer_playIndex(g_encoderState.fileSelectedIndex);
                            g_encoderState.pendingAction = ENC_ACTION_GO_PLAYER;
                        }
                    }
                    break;
            }
        }
        
        lastBtnReadTime = now;
    }
}
