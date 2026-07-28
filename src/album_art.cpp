#include "album_art.h"
#include "config.h"
#include <Arduino.h>
#include <JPEGDEC.h>
#include <PNGdec.h>

// =============================================================
// Estado interno del módulo
// =============================================================

static JPEGDEC jpeg;
static PNG png;
static uint16_t* albumArtBuf = NULL;
static lv_img_dsc_t albumArtDsc;
static lv_obj_t* albumArtImg = NULL;
static int g_imgSrcW = ALBUM_ART_SIZE;
static int g_imgSrcH = ALBUM_ART_SIZE;

// =============================================================
// Helpers internos
// =============================================================

// Configura el descriptor LVGL para el buffer de carátula.
// Elimina la duplicación que existía entre los paths JPEG y PNG.
static void setupArtDescriptor() {
    albumArtDsc.header.always_zero = 0;
    albumArtDsc.header.w = ALBUM_ART_SIZE;
    albumArtDsc.header.h = ALBUM_ART_SIZE;
    albumArtDsc.data_size = ALBUM_ART_SIZE * ALBUM_ART_SIZE * sizeof(uint16_t);
    albumArtDsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    albumArtDsc.data = (const uint8_t*)albumArtBuf;
}

// Asegura que el buffer de carátula exista en PSRAM
static bool ensureBuffer() {
    if (!albumArtBuf) {
        albumArtBuf = (uint16_t*)heap_caps_malloc(
            ALBUM_ART_SIZE * ALBUM_ART_SIZE * sizeof(uint16_t), MALLOC_CAP_SPIRAM
        );
    }
    return albumArtBuf != NULL;
}

// Busca la firma real de imagen dentro del frame APIC del ID3.
// El frame APIC incluye un encabezado de texto (MIME type, descripción, etc.)
// antes de los datos de imagen reales.
// Retorna el offset de la firma, o -1 si no se encuentra.
static int findImageSignature(const uint8_t* data, size_t size, int& outType) {
    int searchLimit = (size > 204) ? 200 : (int)size - 4;
    if (searchLimit < 0) searchLimit = 0;

    for (int i = 0; i < searchLimit; i++) {
        // Firma JPEG: FF D8 FF
        if (data[i] == 0xFF && data[i+1] == 0xD8 && data[i+2] == 0xFF) {
            outType = 1;
            return i;
        }
        // Firma PNG: 89 50 4E 47
        if (i + 3 < (int)size &&
            data[i] == 0x89 && data[i+1] == 0x50 &&
            data[i+2] == 0x4E && data[i+3] == 0x47) {
            outType = 2;
            return i;
        }
    }
    outType = 0;
    return -1;
}

// =============================================================
// Callbacks de dibujo (JPEG y PNG)
// =============================================================

// Callback de dibujo JPEG: escala manualmente a ALBUM_ART_SIZE
static int JPEGDraw(JPEGDRAW *pDraw) {
    if (!albumArtBuf) return 0;

    bool upscale = (g_imgSrcW < ALBUM_ART_SIZE || g_imgSrcH < ALBUM_ART_SIZE);
    int offsetX = upscale ? (ALBUM_ART_SIZE - g_imgSrcW) / 2 : 0;
    int offsetY = upscale ? (ALBUM_ART_SIZE - g_imgSrcH) / 2 : 0;

    for (int y = 0; y < pDraw->iHeight; y++) {
        int srcY = pDraw->y + y;
        if (srcY >= g_imgSrcH) continue;

        int destY = upscale ? (srcY + offsetY) : ((srcY * ALBUM_ART_SIZE) / g_imgSrcH);
        if (destY >= ALBUM_ART_SIZE || destY < 0) continue;

        for (int x = 0; x < pDraw->iWidth; x++) {
            int srcX = pDraw->x + x;
            if (srcX >= g_imgSrcW) continue;

            int destX = upscale ? (srcX + offsetX) : ((srcX * ALBUM_ART_SIZE) / g_imgSrcW);
            if (destX >= ALBUM_ART_SIZE || destX < 0) continue;

            albumArtBuf[destY * ALBUM_ART_SIZE + destX] = pDraw->pPixels[y * pDraw->iWidth + x];
        }
    }
    return 1;
}

// Callback de dibujo PNG: escala manualmente, línea por línea
static int PNGDraw(PNGDRAW *pDraw) {
    if (!albumArtBuf) return 0;

    bool upscale = (g_imgSrcW < ALBUM_ART_SIZE || g_imgSrcH < ALBUM_ART_SIZE);
    int offsetX = upscale ? (ALBUM_ART_SIZE - g_imgSrcW) / 2 : 0;
    int offsetY = upscale ? (ALBUM_ART_SIZE - g_imgSrcH) / 2 : 0;

    int srcY = pDraw->y;
    if (srcY >= g_imgSrcH) return 0;

    int destY = upscale ? (srcY + offsetY) : ((srcY * ALBUM_ART_SIZE) / g_imgSrcH);
    if (destY >= ALBUM_ART_SIZE || destY < 0) return 0;

    // Convertir línea actual a RGB565
    uint16_t *usPixels = (uint16_t *)pDraw->pPixels;
    png.getLineAsRGB565(pDraw, usPixels, PNG_RGB565_LITTLE_ENDIAN, 0xffffffff);

    for (int x = 0; x < pDraw->iWidth; x++) {
        int srcX = x;
        if (srcX >= g_imgSrcW) break;

        int destX = upscale ? (srcX + offsetX) : ((srcX * ALBUM_ART_SIZE) / g_imgSrcW);
        if (destX >= ALBUM_ART_SIZE || destX < 0) continue;

        albumArtBuf[destY * ALBUM_ART_SIZE + destX] = usPixels[x];
    }
    return 1;
}

// =============================================================
// API pública
// =============================================================

void albumArt_init(lv_obj_t* parent) {
    albumArtImg = lv_img_create(parent);
    lv_obj_align(albumArtImg, LV_ALIGN_CENTER, 0, 0);
}

void albumArt_decode(uint8_t* imgData, size_t imgSize) {
    if (!imgData) return;

    bool isDecoded = false;
    int imgType = 0;
    int imgOffset = findImageSignature(imgData, imgSize, imgType);

    if (imgOffset >= 0) {
        uint8_t* actualImgData = imgData + imgOffset;
        size_t actualImgSize = imgSize - imgOffset;

        if (ensureBuffer()) {
            memset(albumArtBuf, 0, ALBUM_ART_SIZE * ALBUM_ART_SIZE * sizeof(uint16_t));
            setupArtDescriptor();

            if (imgType == 1) {
                // --- Decodificar JPEG ---
                if (jpeg.openRAM(actualImgData, actualImgSize, JPEGDraw)) {
                    int w = jpeg.getWidth();
                    int scale = 0;
                    int iOptions = 0;

                    if (w >= 1200)     { scale = 3; iOptions = JPEG_SCALE_EIGHTH; }
                    else if (w >= 600) { scale = 2; iOptions = JPEG_SCALE_QUARTER; }
                    else if (w >= 300) { scale = 1; iOptions = JPEG_SCALE_HALF; }

                    g_imgSrcW = jpeg.getWidth() >> scale;
                    g_imgSrcH = jpeg.getHeight() >> scale;

                    jpeg.setPixelType(RGB565_LITTLE_ENDIAN);
                    isDecoded = jpeg.decode(0, 0, iOptions);
                    jpeg.close();
                }
            }
            else if (imgType == 2) {
                // --- Decodificar PNG ---
                if (png.openRAM(actualImgData, actualImgSize, PNGDraw)) {
                    g_imgSrcW = png.getWidth();
                    g_imgSrcH = png.getHeight();
                    isDecoded = png.decode(NULL, 0);
                    png.close();
                }
            }
        }
    }

    // Mostrar en UI si la decodificación fue exitosa
    if (isDecoded && albumArtImg) {
        lv_img_set_src(albumArtImg, &albumArtDsc);
        lv_img_set_zoom(albumArtImg, 256); // Sin zoom por software, la imagen ya está a ALBUM_ART_SIZE
        lv_obj_align(albumArtImg, LV_ALIGN_CENTER, 0, 0);
        Serial.println("[UI] Carátula escalada y mostrada.");
    } else {
        Serial.println("[UI] Error al decodificar la carátula o formato no soportado.");
    }

    // Siempre liberar la memoria comprimida enviada por la tarea de audio
    heap_caps_free(imgData);
}
