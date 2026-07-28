#include "audio_player.h"
#include "config.h"
#include "shared_state.h"
#include "volume_control.h"
#include <SD.h>

// =============================================================
// Objetos internos
// =============================================================

static Audio audio;
static Playlist playlist;

// =============================================================
// Playlist
// =============================================================

Playlist::Playlist() : count(0), currentIndex(0) {}

void Playlist::scanSD() {
    count = 0;
    currentIndex = 0;

    File root = SD.open("/");
    if (!root || !root.isDirectory()) {
        Serial.println("[Audio] Error: No se pudo abrir la raíz de la SD.");
        return;
    }

    File entry;
    while ((entry = root.openNextFile()) && count < MAX_TRACKS) {
        if (!entry.isDirectory()) {
            String name = String(entry.name());
            String nameLower = name;
            nameLower.toLowerCase();

            if (nameLower.endsWith(".mp3")) {
                // Asegurar que la ruta tenga "/" al inicio
                tracks[count] = name.startsWith("/") ? name : "/" + name;
                count++;
            }
        }
        entry.close();
    }
    root.close();

    Serial.printf("[Audio] Playlist: %d cancion(es) encontrada(s)\n", count);
}

String Playlist::currentTrackPath() const {
    if (count == 0) return "";
    return tracks[currentIndex];
}

String Playlist::nextTrack() {
    if (count == 0) return "";
    currentIndex = (currentIndex + 1) % count;
    return tracks[currentIndex];
}

String Playlist::prevTrack() {
    if (count == 0) return "";
    currentIndex = (currentIndex - 1 + count) % count;
    return tracks[currentIndex];
}

// =============================================================
// Accesores
// =============================================================

Audio& audioPlayer_getAudio() {
    return audio;
}

Playlist& audioPlayer_getPlaylist() {
    return playlist;
}

// =============================================================
// Callbacks de ESP32-audioI2S (weak-linked, se llaman automáticamente)
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

    // Limitar tamaño para evitar consumir demasiada RAM
    if (size > MAX_COVER_SIZE) {
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
    file.seek(currentPos); // Restaurar posición para que el decodificador de MP3 continúe

    MetadataMsg msg;
    msg.type = META_COVER_RAW;
    msg.imgData = imgBuf;
    msg.imgSize = size;
    msg.data[0] = '\0';

    if (xQueueSend(metadataQueue, &msg, 0) != pdTRUE) {
        heap_caps_free(imgBuf); // Liberar si la cola está llena
    }
}

// Llamado cuando termina la reproducción del archivo.
void audio_eof_mp3(const char *info) {
    Serial.printf("[Audio] Fin del archivo: %s\n", info);
    // TODO: Avanzar a la siguiente canción de la playlist
    // String next = playlist.nextTrack();
    // if (!next.isEmpty()) audio.connecttoFS(SD, next.c_str());
}

// =============================================================
// Inicialización pública
// =============================================================

void audioPlayer_init() {
    Serial.println("[Setup] Configurando audio I2S...");
    audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
    audio.setVolume(g_volumeState.level);

    // Escanear la SD para construir la playlist
    playlist.scanSD();

    // Reproducir la primera canción si existe
    String mp3Path = playlist.currentTrackPath();
    if (!mp3Path.isEmpty()) {
        Serial.printf("[Setup] Reproduciendo: %s\n", mp3Path.c_str());
        audio.connecttoFS(SD, mp3Path.c_str());
        g_audioState.playing = true;
    } else {
        Serial.println("[Setup] No se encontró ningún archivo .mp3 en la SD.");
    }
}

// =============================================================
// Tarea FreeRTOS de Audio — Core 0, Prioridad alta
// Responsable de: decodificar MP3, alimentar I2S, leer botones
// =============================================================

void audioTask(void *pvParameters) {
    Serial.println("[Audio] Tarea iniciada en Core 0");

    while (true) {
        // --- Procesamiento de audio (accede a la SD vía SPI) ---
        if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(50))) {
            audio.loop();

            // Actualizar estado compartido para la tarea de UI
            if (audio.isRunning()) {
                g_audioState.current  = audio.getAudioCurrentTime();
                g_audioState.duration = audio.getAudioFileDuration();
                g_audioState.playing  = true;
            } else {
                g_audioState.playing = false;
            }

            xSemaphoreGive(spiMutex);
        }

        // --- Botones de volumen (GPIO, sin SPI, sin mutex) ---
        volume_readButtons(audio);

        // Ceder CPU mínimamente (1 tick ≈ 1ms)
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
