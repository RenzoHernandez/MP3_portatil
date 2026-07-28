#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "ui.h"
#include "Audio.h" // ESP32-audioI2S (schreibfaul1)
#include <SD.h>
#include <SPI.h>

// =============================================================
// 1. CONFIGURACIÓN DE PINES — ESP32-S3 Super Mini N4R2
// =============================================================

// Bus SPI compartido (Pantalla IPS + Micro SD)
#define SPI_SCK   1
#define SPI_MISO  6   // Solo lo usa la micro SD (lectura)
#define SPI_MOSI  2

// Micro SD
#define SD_CS     7

// Pantalla IPS ST7789 (1.69", 240x280)
#define TFT_CS    5
#define TFT_DC    4
#define TFT_RST   3

// I2S Audio (MAX98357A)
#define I2S_BCLK  9
#define I2S_LRC   8
#define I2S_DOUT  10

// Botones de volumen
#define PIN_VOL_DOWN  13
#define PIN_VOL_UP    11

// Dimensiones de pantalla
#define SCREEN_WIDTH  240
#define SCREEN_HEIGHT 280

// =============================================================
// 2. OBJETOS GLOBALES
// =============================================================

// --- Pantalla ---
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, SPI_SCK, SPI_MOSI, -1 // MISO no se usa para la pantalla
);
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, TFT_RST,
    0,      // Rotación
    true,   // Panel IPS
    SCREEN_WIDTH, SCREEN_HEIGHT,
    0, 20   // Offsets (X, Y)
);

// --- Audio ---
Audio audio;

// --- FreeRTOS: Sincronización entre tareas ---
SemaphoreHandle_t spiMutex     = NULL; // Mutex para el bus SPI compartido
QueueHandle_t     metadataQueue = NULL; // Cola para pasar metadata ID3 → UI

// --- Estado compartido (escrito por audioTask, leído por uiTask) ---
volatile uint32_t g_audioCurrent  = 0;     // Tiempo actual en segundos
volatile uint32_t g_audioDuration = 0;     // Duración total en segundos
volatile bool     g_audioPlaying  = false; // ¿Está reproduciendo?

// --- Volumen (ESP32-audioI2S usa rango 0-21) ---
int          g_volume         = 5;
const int    VOL_STEP         = 1;
unsigned long g_lastButtonTime = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 250;

// =============================================================
// 3. ESTRUCTURA PARA MENSAJES DE METADATA
// =============================================================

enum MetadataType {
    META_TITLE,
    META_ARTIST
};

struct MetadataMsg {
    MetadataType type;
    char data[128]; // Búfer para el texto del tag ID3
};

// =============================================================
// 4. LVGL — Buffer y función de flush con protección SPI
// =============================================================

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvgl_buf = NULL;

// Función obligatoria para transferir píxeles de LVGL a la pantalla.
// Protegida con mutex porque la pantalla comparte el bus SPI con la SD.
void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
        xSemaphoreGive(spiMutex);
    }

    lv_disp_flush_ready(disp_drv);
}

// =============================================================
// 5. UTILIDAD: Buscar el primer archivo .mp3 en la raíz de la SD
// =============================================================

String findFirstMP3() {
    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        return "";
    }

    File entry;
    while ((entry = root.openNextFile())) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            String nameLower = name;
            nameLower.toLowerCase();

            if (nameLower.endsWith(".mp3")) {
                // Asegurar que la ruta tenga "/" al inicio
                String path = name.startsWith("/") ? name : "/" + name;
                entry.close();
                root.close();
                return path;
            }
        }
        entry.close();
    }
    root.close();
    return "";
}

// =============================================================
// 6. CALLBACKS DE ESP32-audioI2S (weak-linked, se llaman solos)
// =============================================================

// Llamado automáticamente cuando la librería encuentra un tag ID3.
// El formato típico es "Title: Nombre" o "Artist: Nombre".
void audio_id3data(const char *info) {
    Serial.printf("[Audio] ID3: %s\n", info);

    MetadataMsg msg;
    String infoStr = String(info);

    if (infoStr.startsWith("Title:")) {
        msg.type = META_TITLE;
        String value = infoStr.substring(6);
        value.trim();
        strncpy(msg.data, value.c_str(), sizeof(msg.data) - 1);
        msg.data[sizeof(msg.data) - 1] = '\0';
        xQueueSend(metadataQueue, &msg, 0); // Non-blocking
    }
    else if (infoStr.startsWith("Artist:")) {
        msg.type = META_ARTIST;
        String value = infoStr.substring(7);
        value.trim();
        strncpy(msg.data, value.c_str(), sizeof(msg.data) - 1);
        msg.data[sizeof(msg.data) - 1] = '\0';
        xQueueSend(metadataQueue, &msg, 0);
    }
}

// Llamado cuando termina la reproducción del archivo.
void audio_eof_mp3(const char *info) {
    Serial.printf("[Audio] Fin del archivo: %s\n", info);
}

// =============================================================
// 7. TAREA DE AUDIO — Core 0, Prioridad 2 (alta)
//    Responsable de: decodificar MP3, alimentar I2S, leer botones
// =============================================================

void audioTask(void *pvParameters) {
    Serial.println("[Audio] Tarea iniciada en Core 0");

    // Configurar botones de volumen
    pinMode(PIN_VOL_DOWN, INPUT_PULLDOWN);
    pinMode(PIN_VOL_UP, INPUT_PULLDOWN);

    while (true) {
        // --- Procesamiento de audio (accede a la SD vía SPI) ---
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
            audio.loop();

            // Actualizar estado compartido para la tarea de UI
            if (audio.isRunning()) {
                g_audioCurrent  = audio.getAudioCurrentTime();
                g_audioDuration = audio.getAudioFileDuration();
                g_audioPlaying  = true;
            } else {
                g_audioPlaying = false;
            }

            xSemaphoreGive(spiMutex);
        }

        // --- Botones de volumen (GPIO, sin SPI, sin mutex) ---
        if (millis() - g_lastButtonTime > BUTTON_DEBOUNCE_MS) {
            bool changed = false;

            if (digitalRead(PIN_VOL_DOWN) == HIGH) {
                g_volume = max(0, g_volume - VOL_STEP);
                changed = true;
            }
            else if (digitalRead(PIN_VOL_UP) == HIGH) {
                g_volume = min(21, g_volume + VOL_STEP);
                changed = true;
            }

            if (changed) {
                audio.setVolume(g_volume);
                Serial.printf("[Audio] Volumen: %d/21\n", g_volume);
                g_lastButtonTime = millis();
            }
        }

        // Ceder CPU mínimamente (1 tick ≈ 1ms)
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// =============================================================
// 8. TAREA DE UI — Core 1, Prioridad 1
//    Responsable de: LVGL, mostrar metadata, actualizar progreso
// =============================================================

void uiTask(void *pvParameters) {
    Serial.println("[UI] Tarea iniciada en Core 1");

    unsigned long lastProgressUpdate = 0;
    unsigned long lastTick = millis();
    char timeStr[16];

    while (true) {
        // --- Actualizar tiempo interno de LVGL ---
        unsigned long currentTick = millis();
        lv_tick_inc(currentTick - lastTick);
        lastTick = currentTick;

        // --- Procesar LVGL (el flush usa mutex internamente) ---
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
            }
        }

        // --- Actualizar barra de progreso y tiempos cada 500ms ---
        if (millis() - lastProgressUpdate >= 500) {
            uint32_t current  = g_audioCurrent;
            uint32_t duration = g_audioDuration;

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
            if (g_audioPlaying) {
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
// 9. SETUP — Inicialización secuencial de todo el hardware
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

    // --- Paso 2: Inicializar pantalla IPS ---
    // gfx->begin() configura internamente el bus SPI (SCK, MOSI)
    // en modo compartido (is_shared_interface=true por defecto).
    Serial.println("[Setup] Inicializando pantalla IPS...");
    if (!gfx->begin()) {
        Serial.println("[Setup] Error: No se detecta la pantalla!");
    }
    gfx->fillScreen(BLACK);
    Serial.println("[Setup] Pantalla lista.");

    // --- Paso 3: Reconfigurar SPI para agregar MISO (pin 6) ---
    // La pantalla no usa MISO (es write-only), pero la micro SD
    // necesita MISO para leer datos. Re-inicializamos el bus
    // para agregar el pin MISO sin afectar la pantalla.
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // --- Paso 4: Montar la tarjeta micro SD ---
    Serial.println("[Setup] Montando tarjeta SD...");
    if (!SD.begin(SD_CS, SPI, 80000000)) {
        Serial.println("[Setup] Error: No se pudo montar la tarjeta SD!");
        // Continuamos sin audio — la pantalla seguirá funcionando
    } else {
        Serial.println("[Setup] Tarjeta SD lista.");
    }

    // --- Paso 5: Inicializar LVGL ---
    Serial.println("[Setup] Inicializando LVGL...");
    lv_init();

    // Buffer gráfico en PSRAM (1/4 de pantalla = muy fluido)
    uint32_t buffer_size = (SCREEN_WIDTH * SCREEN_HEIGHT) / 4;
    lvgl_buf = (lv_color_t *)heap_caps_malloc(
        buffer_size * sizeof(lv_color_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
    );

    if (!lvgl_buf) {
        Serial.println("[Setup] Error fatal: No se pudo asignar PSRAM para LVGL!");
        while (true) { delay(100); }
    }

    lv_disp_draw_buf_init(&draw_buf, lvgl_buf, NULL, buffer_size);

    // Configurar driver de pantalla de LVGL
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res  = SCREEN_WIDTH;
    disp_drv.ver_res  = SCREEN_HEIGHT;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    // --- Paso 6: Cargar interfaz de SquareLine Studio ---
    ui_init();

    // Textos iniciales mientras se busca la canción
    lv_label_set_text(ui_LblSongTitle,   "Buscando música...");
    lv_label_set_text(ui_LblArtistName,  "");
    lv_label_set_text(ui_LblTimeCurrent, "0:00");
    lv_label_set_text(ui_LblTimeTotal,   "0:00");
    lv_bar_set_value(ui_BarProgress, 0, LV_ANIM_OFF);

    Serial.println("[Setup] Interfaz cargada.");

    // --- Paso 7: Configurar salida de audio I2S ---
    Serial.println("[Setup] Configurando audio I2S...");
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(g_volume);

    // --- Paso 8: Buscar y reproducir el primer .mp3 de la SD ---
    String mp3Path = findFirstMP3();
    if (!mp3Path.isEmpty()) {
        Serial.printf("[Setup] Reproduciendo: %s\n", mp3Path.c_str());
        audio.connecttoFS(SD, mp3Path.c_str());
        g_audioPlaying = true;
    } else {
        Serial.println("[Setup] No se encontró ningún archivo .mp3 en la SD.");
        lv_label_set_text(ui_LblSongTitle, "Sin música");
    }

    // --- Paso 9: Crear tareas FreeRTOS en sus respectivos cores ---
    Serial.println("[Setup] Creando tareas FreeRTOS...");

    // Tarea de UI en Core 1 (prioridad 1)
    xTaskCreatePinnedToCore(
        uiTask,   // Función de la tarea
        "UI",     // Nombre (para debug)
        16384,    // Stack en bytes
        NULL,     // Parámetros
        1,        // Prioridad
        NULL,     // Handle (no lo necesitamos)
        1         // Core 1
    );

    // Tarea de Audio en Core 0 (prioridad 2 — mayor que UI)
    xTaskCreatePinnedToCore(
        audioTask,
        "Audio",
        16384,
        NULL,
        2,        // Prioridad alta: el audio es crítico en tiempo real
        NULL,
        0         // Core 0
    );

    Serial.println("[Setup] Sistema listo. ¡Reproduciendo!");
}

// =============================================================
// 10. LOOP — Vacío, todo corre en tareas FreeRTOS
// =============================================================

void loop() {
    // El loop() de Arduino queda suspendido indefinidamente.
    // Todo el trabajo se realiza en audioTask (Core 0) y uiTask (Core 1).
    vTaskDelay(portMAX_DELAY);
}