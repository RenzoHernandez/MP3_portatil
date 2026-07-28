#include "volume_control.h"
#include "config.h"
#include "shared_state.h"
#include <Arduino.h>
#include <lvgl.h>
#include "ui.h"

// =============================================================
// Estado interno del toast
// =============================================================

static unsigned long g_volToastShowTime = 0;
static bool g_volToastVisible = false;

// =============================================================
// Animaciones del Toast de Volumen (Fade In / Fade Out)
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
// API pública
// =============================================================

void volume_init() {
    pinMode(PIN_VOL_DOWN, INPUT_PULLDOWN);
    pinMode(PIN_VOL_UP, INPUT_PULLDOWN);
}

void volume_readButtons(Audio& audio) {
    if (millis() - g_volumeState.lastButtonTime > BUTTON_DEBOUNCE_MS) {
        bool changed = false;

        if (digitalRead(PIN_VOL_DOWN) == HIGH) {
            g_volumeState.level = max(0, g_volumeState.level - VOL_STEP);
            changed = true;
        }
        else if (digitalRead(PIN_VOL_UP) == HIGH) {
            g_volumeState.level = min(VOL_MAX, g_volumeState.level + VOL_STEP);
            changed = true;
        }

        if (changed) {
            audio.setVolume(g_volumeState.level);
            g_volumeState.changed = true; // Señalar al uiTask para mostrar el toast
            Serial.printf("[Audio] Volumen: %d/%d\n", g_volumeState.level, VOL_MAX);
            g_volumeState.lastButtonTime = millis();
        }
    }
}

void volumeToast_update() {
    // Detectar cambio señalizado por audioTask
    if (g_volumeState.changed) {
        g_volumeState.changed = false; // Consumir la flag (atómico en ESP32)

        // Actualizar valor de la barra (0-VOL_MAX → 0-100%)
        int volPercent = (g_volumeState.level * 100) / VOL_MAX;
        lv_bar_set_value(ui_BarVolumeLevel, volPercent, LV_ANIM_ON);

        // Mostrar toast con fade in (si no estaba visible)
        if (!g_volToastVisible) {
            volumeToast_fadeIn();
            g_volToastVisible = true;
        }

        // Reiniciar timeout (cada pulsación resetea el timer)
        g_volToastShowTime = millis();
    }

    // Ocultar toast si expiró el timeout
    if (g_volToastVisible && (millis() - g_volToastShowTime >= VOL_TOAST_DURATION_MS)) {
        volumeToast_fadeOut();
        g_volToastVisible = false;
    }
}
