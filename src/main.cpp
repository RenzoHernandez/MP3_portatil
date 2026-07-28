#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include "ui.h"
#include "Audio.h" // ESP32-audioI2S (schreibfaul1)
#include <SD.h>
#include <SPI.h>
#include <JPEGDEC.h>
#include <PNGdec.h>

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

// --- Decodificadores de imagen y buffers (Carátulas) ---
JPEGDEC jpeg;
PNG png;
uint16_t* albumArtBuf = NULL;
lv_img_dsc_t albumArtDsc;
lv_obj_t* ui_AlbumArtImg = NULL;
int g_imgSrcW = 150;
int g_imgSrcH = 150;

// --- Estado compartido (escrito por audioTask, leído por uiTask) ---
volatile uint32_t g_audioCurrent  = 0;     // Tiempo actual en segundos
volatile uint32_t g_audioDuration = 0;     // Duración total en segundos
volatile bool     g_audioPlaying  = false; // ¿Está reproduciendo?

// --- Volumen (ESP32-audioI2S usa rango 0-21) ---
int          g_volume         = 5;
const int    VOL_STEP         = 1;
unsigned long g_lastButtonTime = 0;
const unsigned long BUTTON_DEBOUNCE_MS = 250;

// --- Toast de volumen (señalización audioTask → uiTask) ---
volatile bool  g_volumeChanged        = false; // Flag: audioTask la pone true, uiTask la consume
unsigned long  g_volToastShowTime     = 0;     // Timestamp del último cambio de volumen
bool           g_volToastVisible      = false; // Estado lógico del toast en UI
const unsigned long VOL_TOAST_DURATION_MS = 3000; // Tiempo visible antes del fade out
const unsigned long VOL_FADE_DURATION_MS  = 300;  // Duración de la animación de fade

// =============================================================
// 3. ESTRUCTURA PARA MENSAJES DE METADATA
// =============================================================

enum MetadataType {
    META_TITLE,
    META_ARTIST,
    META_COVER_RAW
};

struct MetadataMsg {
    MetadataType type;
    char data[128]; // Búfer para el texto del tag ID3
    uint8_t* imgData; // Puntero a carátula comprimida (PSRAM)
    size_t imgSize;
};

// =============================================================
// 3.5. CALLBACKS DE DIBUJO DE IMAGEN (JPEG y PNG)
// =============================================================

int JPEGDraw(JPEGDRAW *pDraw) {
    if (!albumArtBuf) return 0;
    
    int targetW = 150;
    int targetH = 150;
    
    bool upscale = (g_imgSrcW < targetW || g_imgSrcH < targetH);
    int offsetX = upscale ? (targetW - g_imgSrcW) / 2 : 0;
    int offsetY = upscale ? (targetH - g_imgSrcH) / 2 : 0;
    
    for (int y = 0; y < pDraw->iHeight; y++) {
        int srcY = pDraw->y + y;
        if (srcY >= g_imgSrcH) continue;
        
        int destY = upscale ? (srcY + offsetY) : ((srcY * targetH) / g_imgSrcH);
        if (destY >= targetH || destY < 0) continue;
        
        for (int x = 0; x < pDraw->iWidth; x++) {
            int srcX = pDraw->x + x;
            if (srcX >= g_imgSrcW) continue;
            
            int destX = upscale ? (srcX + offsetX) : ((srcX * targetW) / g_imgSrcW);
            if (destX >= targetW || destX < 0) continue;
            
            albumArtBuf[destY * targetW + destX] = pDraw->pPixels[y * pDraw->iWidth + x];
        }
    }
    return 1;
}

int PNGDraw(PNGDRAW *pDraw) {
    if (!albumArtBuf) return 0;
    
    int targetW = 150;
    int targetH = 150;
    
    bool upscale = (g_imgSrcW < targetW || g_imgSrcH < targetH);
    int offsetX = upscale ? (targetW - g_imgSrcW) / 2 : 0;
    int offsetY = upscale ? (targetH - g_imgSrcH) / 2 : 0;
    
    int srcY = pDraw->y;
    if (srcY >= g_imgSrcH) return 0;
    
    int destY = upscale ? (srcY + offsetY) : ((srcY * targetH) / g_imgSrcH);
    if (destY >= targetH || destY < 0) return 0;
    
    // Convertir línea actual a RGB565
    uint16_t *usPixels = (uint16_t *)pDraw->pPixels;
    png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);
    
    for (int x = 0; x < pDraw->iWidth; x++) {
        int srcX = x;
        if (srcX >= g_imgSrcW) break;
        
        int destX = upscale ? (srcX + offsetX) : ((srcX * targetW) / g_imgSrcW);
        if (destX >= targetW || destX < 0) continue;
        
        albumArtBuf[destY * targetW + destX] = usPixels[x];
    }
    return 1;
}

// =============================================================
// 3.6. ANIMACIONES DEL TOAST DE VOLUMEN (Fade In / Fade Out)
// =============================================================

// Callback de animación: actualiza la opacidad del panel
static void volToast_setOpa(void *obj, int32_t v) {
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)v, LV_PART_MAIN | LV_STATE_DEFAULT);
}

// Callback al finalizar el fade out: oculta el panel completamente
static void volToast_fadeOutDone(lv_anim_t *a) {
    lv_obj_add_flag((lv_obj_t *)a->var, LV_OBJ_FLAG_HIDDEN);
}

// Muestra el toast con animación de fade in
static void volumeToast_fadeIn(void) {
    lv_obj_clear_flag(ui_PnlVolumeToast, LV_OBJ_FLAG_HIDDEN);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_PnlVolumeToast);
    lv_anim_set_exec_cb(&a, volToast_setOpa);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_time(&a, VOL_FADE_DURATION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

// Oculta el toast con animación de fade out
static void volumeToast_fadeOut(void) {
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, ui_PnlVolumeToast);
    lv_anim_set_exec_cb(&a, volToast_setOpa);
    lv_anim_set_values(&a, 255, 0);
    lv_anim_set_time(&a, VOL_FADE_DURATION_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_set_ready_cb(&a, volToast_fadeOutDone);
    lv_anim_start(&a);
}

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

// Llamado cuando se encuentra la carátula en el MP3 (ID3 APIC frame)
void audio_id3image(File& file, const size_t pos, const size_t size) {
    Serial.printf("[Audio] Carátula encontrada. Tamaño: %u bytes\n", size);
    
    // Limitar tamaño a 250KB para evitar consumir demasiada RAM si la carátula es absurda
    if (size > 250000) {
        Serial.println("[Audio] Carátula demasiado grande, ignorando.");
        return;
    }

    uint8_t* imgBuf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!imgBuf) {
        Serial.println("[Audio] Error: No hay memoria PSRAM para la carátula.");
        return;
    }

    // audio.loop() (y por tanto este callback) ya adquirió el spiMutex
    uint32_t currentPos = file.position();
    file.seek(pos);
    file.read(imgBuf, size);
    file.seek(currentPos); // Restaurar posición para que el decodificador de MP3 continúe feliz

    MetadataMsg msg;
    msg.type = META_COVER_RAW;
    msg.imgData = imgBuf;
    msg.imgSize = size;
    msg.data[0] = '\0'; // Limpiar

    if (xQueueSend(metadataQueue, &msg, 0) != pdTRUE) {
        heap_caps_free(imgBuf);
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
                g_volumeChanged = true; // Señalar al uiTask para mostrar el toast
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
    char timeStr[16];

    while (true) {
        // --- Procesar LVGL (tick automático vía LV_TICK_CUSTOM, el flush usa mutex internamente) ---
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
                    if (msg.imgData) {
                        bool isDecoded = false;
                        int outW = 0, outH = 0;
                        
                        // El callback audio_id3image entrega el frame APIC completo,
                        // el cual incluye un encabezado de texto (MIME type, descripción, etc.).
                        // Buscamos la firma real del JPEG (FF D8 FF) o PNG (89 50 4E 47).
                        int imgOffset = -1;
                        int imgType = 0; // 1 = JPEG, 2 = PNG
                        
                        for (int i = 0; i < 200 && i < (int)msg.imgSize - 4; i++) {
                            if (msg.imgData[i] == 0xFF && msg.imgData[i+1] == 0xD8 && msg.imgData[i+2] == 0xFF) {
                                imgOffset = i;
                                imgType = 1;
                                break;
                            } else if (msg.imgData[i] == 0x89 && msg.imgData[i+1] == 0x50 && msg.imgData[i+2] == 0x4E && msg.imgData[i+3] == 0x47) {
                                imgOffset = i;
                                imgType = 2;
                                break;
                            }
                        }
                        
                        if (imgOffset >= 0) {
                            uint8_t* actualImgData = msg.imgData + imgOffset;
                            size_t actualImgSize = msg.imgSize - imgOffset;
                            
                            // 1. Decodificar JPEG
                            if (imgType == 1) {
                                if (jpeg.openRAM(actualImgData, actualImgSize, JPEGDraw)) {
                                    int w = jpeg.getWidth();
                                    int scale = 0; // 0=1:1, 1=1:2, 2=1:4, 3=1:8
                                    int iOptions = 0;
                                    
                                    if (w >= 1200) { scale = 3; iOptions = JPEG_SCALE_EIGHTH; }
                                    else if (w >= 600) { scale = 2; iOptions = JPEG_SCALE_QUARTER; }
                                    else if (w >= 300) { scale = 1; iOptions = JPEG_SCALE_HALF; }
                                    
                                    g_imgSrcW = jpeg.getWidth() >> scale;
                                    g_imgSrcH = jpeg.getHeight() >> scale;
                                    
                                    if (!albumArtBuf) {
                                        albumArtBuf = (uint16_t*)heap_caps_malloc(150 * 150 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
                                    }
                                    
                                    if (albumArtBuf) {
                                        memset(albumArtBuf, 0, 150 * 150 * sizeof(uint16_t));
                                        
                                        // Configurar Header LVGL
                                        albumArtDsc.header.always_zero = 0;
                                        albumArtDsc.header.w = 150;
                                        albumArtDsc.header.h = 150;
                                        albumArtDsc.data_size = 150 * 150 * sizeof(uint16_t);
                                        albumArtDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                                        albumArtDsc.data = (const uint8_t*)albumArtBuf;
                                        
                                        jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
                                        if (jpeg.decode(0, 0, iOptions)) {
                                            isDecoded = true;
                                        }
                                    }
                                    jpeg.close();
                                }
                            }
                            // 2. Detectar si es PNG
                            else if (imgType == 2) {
                                if (png.openRAM(actualImgData, actualImgSize, PNGDraw)) {
                                    g_imgSrcW = png.getWidth();
                                    g_imgSrcH = png.getHeight();
                                    
                                    if (!albumArtBuf) {
                                        albumArtBuf = (uint16_t*)heap_caps_malloc(150 * 150 * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
                                    }
                                    
                                    if (albumArtBuf) {
                                        memset(albumArtBuf, 0, 150 * 150 * sizeof(uint16_t));
                                        
                                        albumArtDsc.header.always_zero = 0;
                                        albumArtDsc.header.w = 150;
                                        albumArtDsc.header.h = 150;
                                        albumArtDsc.data_size = 150 * 150 * sizeof(uint16_t);
                                        albumArtDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
                                        albumArtDsc.data = (const uint8_t*)albumArtBuf;
                                        
                                        if (png.decode(NULL, 0)) {
                                            isDecoded = true;
                                        }
                                    }
                                    png.close();
                                }
                            }
                        } // Fin if (imgOffset >= 0)
                        
                        // 3. Mostrar en UI si fue exitoso
                        if (isDecoded && ui_AlbumArtImg) {
                            lv_img_set_src(ui_AlbumArtImg, &albumArtDsc);
                            
                            // Ya no usamos el software zoom, la imagen YA fue escalada a mano a 150x150
                            lv_img_set_zoom(ui_AlbumArtImg, 256);
                            lv_obj_align(ui_AlbumArtImg, LV_ALIGN_CENTER, 0, 0);
                            
                            Serial.println("[UI] Carátula escalada y mostrada.");
                        } else {
                            Serial.println("[UI] Error al decodificar la carátula o formato no soportado.");
                        }
                        
                        // Siempre liberar la memoria comprimida enviada por la tarea de audio
                        heap_caps_free(msg.imgData);
                    }
                    break;
            }
        }

        // --- Toast de volumen: detectar cambio y gestionar visibilidad ---
        if (g_volumeChanged) {
            g_volumeChanged = false; // Consumir la flag (atómico en ESP32)

            // Actualizar valor de la barra (0-21 → 0-100%)
            int volPercent = (g_volume * 100) / 21;
            lv_bar_set_value(ui_BarVolumeLevel, volPercent, LV_ANIM_ON);

            // Mostrar toast con fade in (si no estaba visible)
            if (!g_volToastVisible) {
                volumeToast_fadeIn();
                g_volToastVisible = true;
            }

            // Reiniciar timeout (cada pulsación resetea el timer)
            g_volToastShowTime = millis();
        }

        // Ocultar toast si expiró el timeout de 3 segundos
        if (g_volToastVisible && (millis() - g_volToastShowTime >= VOL_TOAST_DURATION_MS)) {
            volumeToast_fadeOut();
            g_volToastVisible = false;
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

    // Toast de volumen: asegurar estado inicial oculto con opacidad 0
    lv_obj_add_flag(ui_PnlVolumeToast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_opa(ui_PnlVolumeToast, 0, LV_PART_MAIN | LV_STATE_DEFAULT);

    // Crear el objeto LVGL de imagen dinámicamente dentro del panel de máscara
    ui_AlbumArtImg = lv_img_create(ui_PnlAlbumMask);
    lv_obj_align(ui_AlbumArtImg, LV_ALIGN_CENTER, 0, 0);

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