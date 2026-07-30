#include "audio_player.h"
#include "config.h"
#include "shared_state.h"
#include "rotary_encoder.h"
#include <SD.h>

// =============================================================
// Objetos internos
// =============================================================

static Audio audio;
static Playlist playlist;

// =============================================================
// Playlist
// =============================================================

Playlist::Playlist() : count(0), currentIndex(0), currentPath("/") {}

void Playlist::scanSD(String path) {
    if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
        count = 0;
        currentIndex = 0;
        currentPath = path;

        if (currentPath != "/") {
            entries[count].name = "..";
            entries[count].isDir = true;
            count++;
        }

        File root = SD.open(currentPath.c_str());
        if (!root || !root.isDirectory()) {
            Serial.println("[Audio] Error: No se pudo abrir el directorio.");
            if (root) root.close();
            xSemaphoreGive(spiMutex);
            return;
        }

        File entry;
        while ((entry = root.openNextFile()) && count < MAX_TRACKS) {
            String name = String(entry.name());
            
            // Asegurarnos de obtener solo el nombre base
            int lastSlash = name.lastIndexOf('/');
            if (lastSlash >= 0) {
                name = name.substring(lastSlash + 1);
            }

            // Ignorar archivos ocultos o de sistema
            if (name.startsWith(".") || name.equalsIgnoreCase("System Volume Information")) {
                entry.close();
                continue;
            }

            if (entry.isDirectory()) {
                entries[count].name = name;
                entries[count].isDir = true;
                count++;
            } else {
                String nameLower = name;
                nameLower.toLowerCase();
                if (nameLower.endsWith(".mp3")) {
                    entries[count].name = name;
                    entries[count].isDir = false;
                    count++;
                }
            }
            entry.close();
        }
        root.close();

        // Ordenamiento alfabético: carpetas primero, luego archivos
        int startIndex = (currentPath != "/") ? 1 : 0;
        for (int i = startIndex + 1; i < count; i++) {
            FileEntry key = entries[i];
            int j = i - 1;
            
            while (j >= startIndex) {
                bool swapNeeded = false;
                if (entries[j].isDir == key.isDir) {
                    // Ambos son del mismo tipo, comparar alfabéticamente
                    if (strcasecmp(entries[j].name.c_str(), key.name.c_str()) > 0) {
                        swapNeeded = true;
                    }
                } else if (!entries[j].isDir && key.isDir) {
                    // key es directorio, j es archivo -> key debe ir antes
                    swapNeeded = true;
                }
                
                if (swapNeeded) {
                    entries[j + 1] = entries[j];
                    j--;
                } else {
                    break;
                }
            }
            entries[j + 1] = key;
        }
        xSemaphoreGive(spiMutex);
        Serial.printf("[Audio] Playlist: %d elementos en %s\n", count, currentPath.c_str());
    }
}

String Playlist::currentTrackPath() const {
    if (count == 0) return "";
    String name = entries[currentIndex].name;
    if (currentPath == "/") return "/" + name;
    return currentPath + "/" + name;
}

String Playlist::nextTrack() {
    if (count == 0) return "";
    currentIndex = (currentIndex + 1) % count;
    // Skip directories when auto-playing next track
    while (entries[currentIndex].isDir) {
        currentIndex = (currentIndex + 1) % count;
    }
    return currentTrackPath();
}

String Playlist::prevTrack() {
    if (count == 0) return "";
    currentIndex = (currentIndex - 1 + count) % count;
    while (entries[currentIndex].isDir) {
        currentIndex = (currentIndex - 1 + count) % count;
    }
    return currentTrackPath();
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

void audioPlayer_playIndex(int index) {
    if (index >= 0 && index < playlist.count && !playlist.entries[index].isDir) {
        playlist.currentIndex = index;
        String mp3Path = playlist.currentTrackPath();
        if (!mp3Path.isEmpty()) {
            Serial.printf("[Audio] Reproduciendo seleccion: %s\n", mp3Path.c_str());
            if (xSemaphoreTake(spiMutex, pdMS_TO_TICKS(100))) {
                audio.connecttoFS(SD, mp3Path.c_str());
                xSemaphoreGive(spiMutex);
                g_audioState.playing = true;
            } else {
                Serial.println("[Audio] Error: No se pudo obtener el spiMutex para reproducir.");
            }
        }
    }
}

void audioPlayer_enterDir(int index) {
    if (index >= 0 && index < playlist.count && playlist.entries[index].isDir) {
        String dirName = playlist.entries[index].name;
        String newPath;
        
        if (dirName == "..") {
            // Subir un nivel
            int lastSlash = playlist.currentPath.lastIndexOf('/');
            if (lastSlash <= 0) {
                newPath = "/";
            } else {
                newPath = playlist.currentPath.substring(0, lastSlash);
            }
        } else {
            // Bajar un nivel
            if (playlist.currentPath == "/") {
                newPath = "/" + dirName;
            } else {
                newPath = playlist.currentPath + "/" + dirName;
            }
        }
        
        Serial.printf("[Audio] Entrando a directorio: %s\n", newPath.c_str());
        playlist.scanSD(newPath);
        
        // Notificar a la UI
        g_encoderState.fileSelectedIndex = 0;
        g_encoderState.fileListChanged = true;
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

        // --- Leer Encoder y Botón (GPIO, sin SPI, sin mutex) ---
        encoder_update(audio);

        // Ceder CPU mínimamente (1 tick ≈ 1ms)
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}
