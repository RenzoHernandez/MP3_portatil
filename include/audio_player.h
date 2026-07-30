#pragma once

#include <Arduino.h>
#include "Audio.h"

// =============================================================
// Audio Player — I2S, MP3, Playlist, Callbacks ID3
// =============================================================

// --- Tipos de metadata (compartidos con la tarea de UI) ---
enum MetadataType {
    META_TITLE,
    META_ARTIST,
    META_COVER_RAW
};

struct MetadataMsg {
    MetadataType type;
    char data[128];       // Búfer para el texto del tag ID3
    uint8_t* imgData;     // Puntero a carátula comprimida (PSRAM)
    size_t imgSize;
};

// --- Playlist ---
struct FileEntry {
    String name;
    bool isDir;
};

struct Playlist {
    static const int MAX_TRACKS = 128;
    FileEntry entries[MAX_TRACKS];
    int count;
    int currentIndex;
    String currentPath;

    Playlist();
    void scanSD(String path = "/");
    String currentTrackPath() const;
    String nextTrack();
    String prevTrack();
};

// Inicializa el audio I2S y escanea la SD (no reproduce automáticamente)
void audioPlayer_init();

// Reproduce la canción en el índice especificado
void audioPlayer_playIndex(int index);

// Entra a un directorio y recarga la lista
void audioPlayer_enterDir(int index);

// Tarea FreeRTOS de audio (Core 0, prioridad alta)
void audioTask(void *pvParameters);

// Acceso al objeto Audio (necesario para volume_control)
Audio& audioPlayer_getAudio();

// Acceso a la playlist
Playlist& audioPlayer_getPlaylist();
