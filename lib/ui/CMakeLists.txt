SET(SOURCES ui_comp_contstatusbar.c
    ui_comp.c
    ui_Reproductor.c
    ui_Archivos.c
    ui.c
    ui_comp_hook.c
    ui_helpers.c
    ui_img_bateria_png.c
    ui_img_pause_png.c
    ui_img_play_png.c
    ui_img_volume_png.c
    fonts/ui_font_Bold16.c
    fonts/ui_font_Regular14.c)

add_library(ui ${SOURCES})
