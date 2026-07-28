#include "display_manager.h"
#include "config.h"
#include "shared_state.h"
#include <Arduino.h>
#include <Arduino_GFX_Library.h>
#include <lvgl.h>
#include <SPI.h>

// =============================================================
// Objetos de pantalla (internos al módulo)
// =============================================================

static Arduino_DataBus *bus = new Arduino_ESP32SPI(
    TFT_DC, TFT_CS, SPI_SCK, SPI_MOSI, -1 // MISO no se usa para la pantalla
);
static Arduino_GFX *gfx = new Arduino_ST7789(
    bus, TFT_RST,
    0,      // Rotación
    true,   // Panel IPS
    SCREEN_WIDTH, SCREEN_HEIGHT,
    0, 20   // Offsets (X, Y)
);

// =============================================================
// LVGL — Buffer y función de flush con protección SPI
// =============================================================

static lv_disp_draw_buf_t draw_buf;
static lv_color_t *lvgl_buf = NULL;

// Función obligatoria para transferir píxeles de LVGL a la pantalla.
// Protegida con mutex porque la pantalla comparte el bus SPI con la SD.
static void my_disp_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);

    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
        gfx->draw16bitRGBBitmap(area->x1, area->y1, (uint16_t *)&color_p->full, w, h);
        xSemaphoreGive(spiMutex);
    }

    lv_disp_flush_ready(disp_drv);
}

// =============================================================
// Inicialización pública
// =============================================================

void display_init() {
    // Inicializar pantalla IPS
    // gfx->begin() configura internamente el bus SPI (SCK, MOSI)
    Serial.println("[Setup] Inicializando pantalla IPS...");
    if (!gfx->begin()) {
        Serial.println("[Setup] Error: No se detecta la pantalla!");
    }
    gfx->fillScreen(BLACK);
    Serial.println("[Setup] Pantalla lista.");

    // Reconfigurar SPI para agregar MISO (pin 6).
    // La pantalla no usa MISO (es write-only), pero la micro SD
    // necesita MISO para leer datos.
    SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

    // Inicializar LVGL
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

    Serial.println("[Setup] LVGL inicializado.");
}
