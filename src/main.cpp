#include <Arduino.h>
#include <SD.h>
#include <SPI.h>
#include <lvgl.h>
#include "ui.h"
#include "config.h"
#include "shared_state.h"
#include "display_manager.h"
#include "audio_player.h"
#include "album_art.h"
#include "volume_control.h"
#include "rotary_encoder.h"

// =============================================================
// Definición de variables globales compartidas (declaradas en shared_state.h)
// =============================================================

AudioState   g_audioState  = {0, 0, false};
VolumeState  g_volumeState = {VOL_DEFAULT, false};
EncoderState g_encoderState = {ENC_ACTION_NONE, SCREEN_PLAYER};
SemaphoreHandle_t spiMutex     = NULL;
QueueHandle_t     metadataQueue = NULL;

// =============================================================
// Lista de Archivos (UI)
// =============================================================
lv_obj_t* fileListItems[Playlist::MAX_TRACKS];

void populateFileList() {
    lv_obj_clean(ui_PnlFileList);
    Playlist& pl = audioPlayer_getPlaylist();
    
    // Actualizar la ruta actual
    if (ui_LblCurrentPath != NULL) {
        lv_label_set_text(ui_LblCurrentPath, pl.currentPath.c_str());
    }
    
    for (int i = 0; i < pl.count; i++) {
        // Instanciar el componente de SLS
        lv_obj_t* item = ui_FileItem_create(ui_PnlFileList);
        
        String displayName = pl.entries[i].name;
        
        // Obtener el label hijo del componente para setear el texto
        lv_obj_t* label = ui_comp_get_child(item, UI_COMP_FILEITEM_LBLFILENAME);
        if (label) {
            lv_label_set_text(label, displayName.c_str());
        }
        
        // Asignar el ícono correspondiente (carpeta o música)
        lv_obj_t* icon = ui_comp_get_child(item, UI_COMP_FILEITEM_IMGFILEICON);
        if (icon) {
            if (pl.entries[i].isDir) {
                lv_img_set_src(icon, &ui_img_carpeta_png);
            } else {
                lv_img_set_src(icon, &ui_img_musica_png);
            }
        }
        
        // Colorear el primer elemento por defecto aplicando el estado Focused
        if (i == 0) {
            lv_obj_add_state(item, LV_STATE_FOCUSED);
        } else {
            lv_obj_clear_state(item, LV_STATE_FOCUSED);
        }
        
        fileListItems[i] = item;
    }
}

// =============================================================
// Tarea de UI — Core 1, Prioridad 1
// Responsable de: LVGL, mostrar metadata, actualizar progreso
// =============================================================

void uiTask(void *pvParameters) {
    Serial.println("[UI] Tarea iniciada en Core 1");

    unsigned long lastProgressUpdate = 0;
    char timeStr[16];

    while (true) {
        // --- Procesar LVGL (tick automático vía LV_TICK_CUSTOM) ---
        lv_timer_handler();

        // --- Recibir metadata del callback de audio vía Queue ---
        MetadataMsg msg;
        while (xQueueReceive(metadataQueue, &msg, 0) == pdTRUE) {
            switch (msg.type) {
                case META_TITLE:
                    lv_label_set_text(ui_LblSongTitle, msg.data);
                    Serial.printf("[UI] Título: %s\n", msg.data);
                    break;
                case META_ARTIST:
                    lv_label_set_text(ui_LblArtistName, msg.data);
                    Serial.printf("[UI] Artista: %s\n", msg.data);
                    break;
                case META_COVER_RAW:
                    albumArt_decode(msg.imgData, msg.imgSize);
                    break;
            }
        }

        // --- Toast de volumen ---
        volumeToast_update();

        // --- Acciones del encoder ---
        if (g_encoderState.pendingAction != ENC_ACTION_NONE) {
            EncoderAction action = g_encoderState.pendingAction;
            g_encoderState.pendingAction = ENC_ACTION_NONE; // Consumir flag
            
            if (action == ENC_ACTION_GO_FILES) {
                // Cambiar a pantalla de archivos
                _ui_screen_change(&ui_Archivos, LV_SCR_LOAD_ANIM_MOVE_RIGHT, 300, 0, &ui_Archivos_screen_init);
                g_encoderState.currentScreen = SCREEN_FILES;
                
                // Asegurar que la lista esté actualizada y el elemento actual visible
                if (g_encoderState.fileListChanged) {
                    g_encoderState.fileListChanged = false;
                    populateFileList();
                }
                
                if (audioPlayer_getPlaylist().count > 0) {
                    lv_obj_scroll_to_view(fileListItems[g_encoderState.fileSelectedIndex], LV_ANIM_OFF);
                }
            } else if (action == ENC_ACTION_GO_PLAYER) {
                // Cambiar a pantalla de reproductor
                _ui_screen_change(&ui_Reproductor, LV_SCR_LOAD_ANIM_MOVE_LEFT, 300, 0, &ui_Reproductor_screen_init);
                g_encoderState.currentScreen = SCREEN_PLAYER;
            }
        }
        
        // --- Refrescar la lista de archivos dinámicamente si cambió de carpeta estando en SCREEN_FILES ---
        if (g_encoderState.fileListChanged && g_encoderState.currentScreen == SCREEN_FILES) {
            g_encoderState.fileListChanged = false;
            populateFileList();
        }
        
        // --- Actualizar resaltado de la lista ---
        if (g_encoderState.selectionChanged) {
            g_encoderState.selectionChanged = false;
            int count = audioPlayer_getPlaylist().count;
            for (int i = 0; i < count; i++) {
                if (i == g_encoderState.fileSelectedIndex) {
                    lv_obj_add_state(fileListItems[i], LV_STATE_FOCUSED);
                    lv_obj_scroll_to_view(fileListItems[i], LV_ANIM_ON);
                } else {
                    lv_obj_clear_state(fileListItems[i], LV_STATE_FOCUSED);
                }
            }
        }

        // --- Actualizar barra de progreso y tiempos cada 500ms ---
        if (millis() - lastProgressUpdate >= 500) {
            uint32_t current  = g_audioState.current;
            uint32_t duration = g_audioState.duration;

            // Tiempo actual "M:SS"
            snprintf(timeStr, sizeof(timeStr), "%lu:%02lu",
                     (unsigned long)(current / 60),
                     (unsigned long)(current % 60));
            lv_label_set_text(ui_LblTimeCurrent, timeStr);

            // Tiempo total "M:SS"
            snprintf(timeStr, sizeof(timeStr), "%lu:%02lu",
                     (unsigned long)(duration / 60),
                     (unsigned long)(duration % 60));
            lv_label_set_text(ui_LblTimeTotal, timeStr);

            // Barra de progreso (0-100%)
            if (duration > 0) {
                int progress = (int)((current * 100UL) / duration);
                lv_bar_set_value(ui_BarProgress, progress, LV_ANIM_ON);
            }

            // Iconos Play/Pause
            if (g_audioState.playing) {
                // Reproduciendo → mostrar icono de pausa
                lv_obj_clear_flag(ui_ImgPauseIcon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ui_ImgPlayIcon, LV_OBJ_FLAG_HIDDEN);
            } else {
                // Detenido → mostrar icono de play
                lv_obj_add_flag(ui_ImgPauseIcon, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(ui_ImgPlayIcon, LV_OBJ_FLAG_HIDDEN);
            }

            lastProgressUpdate = millis();
        }

        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// =============================================================
// Setup — Inicialización secuencial de todo el hardware
// =============================================================

void setup() {
    Serial.begin(115200);
    delay(1000); // Esperar USB CDC del S3
    Serial.println("=== Reproductor MP3 Portátil ===");
    Serial.println("[Setup] Iniciando sistema...");

    // --- Paso 1: Crear primitivas FreeRTOS ---
    spiMutex = xSemaphoreCreateMutex();
    metadataQueue = xQueueCreate(10, sizeof(MetadataMsg));

    if (!spiMutex || !metadataQueue) {
        Serial.println("[Setup] Error fatal: No se pudieron crear mutex/queue!");
        while (true) { delay(100); }
    }

    // --- Paso 2: Inicializar pantalla + SPI + LVGL ---
    display_init();

    // --- Paso 3: Montar la tarjeta micro SD ---
    Serial.println("[Setup] Montando tarjeta SD...");
    if (!SD.begin(SD_CS, SPI, SD_SPI_SPEED)) {
        Serial.println("[Setup] Error: No se pudo montar la tarjeta SD!");
        // Continuamos sin audio — la pantalla seguirá funcionando
    } else {
        Serial.println("[Setup] Tarjeta SD lista.");
    }

    // --- Paso 4: Cargar interfaz de SquareLine Studio ---
    ui_init();

    // Textos iniciales mientras se busca la canción
    lv_label_set_text(ui_LblSongTitle,   "Buscando música...");
    lv_label_set_text(ui_LblArtistName,  "");
    lv_label_set_text(ui_LblTimeCurrent, "0:00");
    lv_label_set_text(ui_LblTimeTotal,   "0:00");
    lv_bar_set_value(ui_BarProgress, 0, LV_ANIM_OFF);

    // Toast de volumen: asegurar estado inicial oculto con opacidad 0
    lv_obj_add_flag(ui_PnlVolumeToast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ui_PnlVolumeToast, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Crear el widget de carátula dentro del panel de máscara
    albumArt_init(ui_PnlAlbumMask);

    Serial.println("[Setup] Interfaz cargada.");

    // --- Paso 5: Configurar audio e iniciar reproducción ---
    audioPlayer_init();

    // Mostrar "Sin música" si no se encontraron canciones
    if (audioPlayer_getPlaylist().count == 0) {
        lv_label_set_text(ui_LblSongTitle, "Sin música");
    } else {
        populateFileList();
    }

    // --- Paso 6: Configurar encoder rotatorio ---
    encoder_init();

    // --- Paso 7: Crear tareas FreeRTOS en sus respectivos cores ---
    Serial.println("[Setup] Creando tareas FreeRTOS...");

    // Tarea de UI en Core 1 (prioridad 1)
    xTaskCreatePinnedToCore(
        uiTask,
        "UI",
        UI_TASK_STACK,
        NULL,
        UI_TASK_PRIORITY,
        NULL,
        UI_TASK_CORE
    );

    // Tarea de Audio en Core 0 (prioridad 2 — mayor que UI)
    xTaskCreatePinnedToCore(
        audioTask,
        "Audio",
        AUDIO_TASK_STACK,
        NULL,
        AUDIO_TASK_PRIORITY,
        NULL,
        AUDIO_TASK_CORE
    );

    Serial.println("[Setup] Sistema listo. ¡Reproduciendo!");
}

// =============================================================
// Loop — Vacío, todo corre en tareas FreeRTOS
// =============================================================

void loop() {
    // El loop() de Arduino queda suspendido indefinidamente.
    // Todo el trabajo se realiza en audioTask (Core 0) y uiTask (Core 1).
    vTaskDelay(portMAX_DELAY);
}