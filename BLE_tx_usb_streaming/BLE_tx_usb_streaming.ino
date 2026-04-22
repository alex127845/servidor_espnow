// ════════════════════════════════════════════════════════════��═══
// 📡 FILE TRANSFER SYSTEM - TX USB STREAMING (SIN BLE, SIN ALMACENAMIENTO)
// ════════════════════════════════════════════════════════════════
// Heltec WiFi LoRa 32 V3
// Conexión: USB Serial → Buffer circular → Transmite ESP-NOW broadcast
// ════════════════════════════════════════════════════════════════

#include <Arduino.h>
#include <BLEDevice.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <ArduinoJson.h>
#include <Preferences.h>

// ════════════════════════════════════════════════════════════════
// 🔧 CONFIGURACIÓN
// ════════════════════════════════════════════════════════════════

#define VEXT 3
#define VEXT_ON LOW

#define CHUNK_SIZE_ESPNOW 240
#define BUFFER_SIZE 16384           // Buffer circular 16KB
#define MAX_RETRIES 3
#define FEC_BLOCK_SIZE 8
#define MANIFEST_REPEAT 3
#define REPEAT_COUNT 3             // 3 vueltas de transmisión
#define FILE_END_REPEAT 5

// Magic bytes
#define MANIFEST_MAGIC_1  0xAA
#define MANIFEST_MAGIC_2  0xBB
#define DATA_MAGIC_1      0xCC
#define DATA_MAGIC_2      0xDD
#define PARITY_MAGIC_1    0xEE
#define PARITY_MAGIC_2    0xFF
#define FILE_END_MAGIC_1  0x99
#define FILE_END_MAGIC_2  0x88

// ════════════════════════════════════════════════════════════════
// 🌐 VARIABLES GLOBALES - ESP-NOW
// ════════════════════════════════════════════════════════════════

uint8_t broadcastAddress[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
int currentPower = 20;
int currentChannel = 1;
int currentRate = 1;

volatile bool transmitting = false;
unsigned long espnowTransmissionStartTime = 0;
uint32_t totalESPNowPacketsSent = 0;
uint32_t totalESPNowRetries = 0;
uint32_t currentFileID = 0;
uint32_t lastFileSize = 0;
static uint32_t fileIDCounter = 0;

// ════════════════════════════════════════════════════════════════
// 💾 BUFFER CIRCULAR
// ════════════════════════════════════════════════════════════════

uint8_t circularBuffer[BUFFER_SIZE];
volatile uint16_t writePtr = 0;
volatile uint16_t readPtr = 0;
volatile uint32_t totalBytesWritten = 0;

// ════════════════════════════════════════════════════════════════
// 📊 ESTADO DE TRANSMISIÓN
// ════════════════════════════════════════════════════════════════

enum TxState {
  STATE_IDLE,
  STATE_RX_STREAMING,
  STATE_TRANSMITTING,
  STATE_FINALIZING
};

TxState currentTxState = STATE_IDLE;
String rxFilename = "";
uint32_t rxFileSize = 0;
uint32_t rxBytesReceived = 0;
uint16_t txChunksSent = 0;

Preferences preferences;

// ════════════════════════════════════════════════════════════════
// 📝 DECLARACIÓN DE FUNCIONES
// ════════════════════════════════════════════════════════════════

void setupESPNow();
void enableVext(bool on);
void applyESPNowConfig();
void loadESPNowConfig();
void saveESPNowConfig();
int getInterPacketDelay();

void readUSBData();
void handleUSBCommand(String cmd);
void bufferWrite(uint8_t byte);
void bufferRead(uint8_t* data, size_t len);
uint16_t getBufferFilledBytes();
bool bufferHasData();
void transmitNextChunks();
void finalizeBroadcast();

uint16_t crc16_ccitt(const uint8_t* data, size_t len);
void onESPNowSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status);

bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName);
bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len);
bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len);
bool sendFileEnd(uint32_t fileID, uint16_t totalChunks);
bool sendFileViaESPNow(const String& filename, uint32_t fileSize);

// ════════════════════════════════════════════════════════════════
// 🔐 CRC16-CCITT
// ════════════════════════════════════════════════════════════════

uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) crc = (crc << 1) ^ 0x1021;
      else              crc = (crc << 1);
    }
  }
  return crc;
}

// ════════════════════════════════════════════════════════════════
// 🔌 ESP-NOW CALLBACKS
// ════════════════════════════��═══════════════════════════════════

void onESPNowSent(const wifi_tx_info_t *tx_info, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    totalESPNowRetries++;
  }
}

// ════════════════════════════════════════════════════════════════
// 🚀 SETUP
// ════════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("\n");
  Serial.println("════════════════════════════════════════════════");
  Serial.println("  📡 USB STREAMING TX SYSTEM v1.0");
  Serial.println("  Heltec WiFi LoRa 32 V3");
  Serial.println("  MODO: TRANSMISOR USB BROADCAST (SIN BLE)");
  Serial.println("  🔄 BUFFER CIRCULAR 8KB + TRANSMISIÓN SIMULTÁNEA");
  Serial.println("════════════════════════════════════════════════");
  Serial.println();

  setupESPNow();
  loadESPNowConfig();

  Serial.println("\n✅ Sistema TX USB STREAMING listo");
  Serial.println("🎧 Esperando conexión por USB...");
  Serial.println("📋 Protocolo: START:filename:size → data → END\n");
}

// ════════════════════════════════════════════════════════════════
// 🔁 LOOP PRINCIPAL
// ════════════════════════════════════════════════════════════════

void loop() {
  // 1. LEER USB (siempre activo)
  if (Serial.available()) {
    readUSBData();
  }

  // 2. TRANSMITIR (si hay datos buffereados)
  if ((currentTxState == STATE_RX_STREAMING || currentTxState == STATE_TRANSMITTING) && !transmitting) {
    if (bufferHasData()) {
      transmitNextChunks();
    }
  }

  // 3. FINALIZAR (después de END)
  if (currentTxState == STATE_FINALIZING) {
    finalizeBroadcast();
    currentTxState = STATE_IDLE;
  }

  yield();
  delay(5);
}

// ════════════════════════════════════════════════════════════════
// 📡 ESP-NOW - INICIALIZACIÓN
// ════════════════════════════════════════════════════════════════

void enableVext(bool on) {
  pinMode(VEXT, OUTPUT);
  digitalWrite(VEXT, on ? VEXT_ON : !VEXT_ON);
}

void setupESPNow() {
  Serial.println("\n📡 Inicializando ESP-NOW...");

  enableVext(true);
  delay(200);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  if (esp_now_init() != ESP_OK) {
    Serial.println("❌ Error inicializando ESP-NOW");
    return;
  }

  Serial.println("✅ ESP-NOW inicializado");
  esp_now_register_send_cb(onESPNowSent);

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = currentChannel;
  peerInfo.encrypt = false;

  if (esp_now_add_peer(&peerInfo) != ESP_OK) {
    Serial.println("❌ Error agregando peer broadcast");
    return;
  }

  Serial.println("✅ Peer broadcast agregado");
}

// ════════════════════════════════════════════════════════════════
// ✅ CARGAR CONFIGURACIÓN ESP-NOW
// ════════════════════════════════════════════════════════════════

void loadESPNowConfig() {
  Serial.println("\n💾 Cargando configuración ESP-NOW...");
  preferences.begin("espnow-config", true);

  currentPower = preferences.getInt("power", 20);
  currentChannel = preferences.getInt("channel", 1);
  currentRate = preferences.getInt("rate", 1);

  preferences.end();

  Serial.println("✅ Configuración ESP-NOW cargada:");
  Serial.printf("   Potencia: %d dBm\n", currentPower);
  Serial.printf("   Canal: %d\n", currentChannel);
  Serial.printf("   Velocidad: %s\n",
    currentRate == 0 ? "1 Mbps" :
    currentRate == 1 ? "2 Mbps" :
    currentRate == 2 ? "5.5 Mbps" : "11 Mbps");

  applyESPNowConfig();
}

// ════════════════════════════════════════════════════════════════
// ✅ GUARDAR CONFIGURACIÓN ESP-NOW
// ════════════════════════════════════════════════════════════════

void saveESPNowConfig() {
  preferences.begin("espnow-config", false);
  preferences.putInt("power", currentPower);
  preferences.putInt("channel", currentChannel);
  preferences.putInt("rate", currentRate);
  preferences.end();
  Serial.println("💾 ✅ Configuración guardada");
}

// ════════════════════════════════════════════════════════════════
// 📡 APLICAR CONFIGURACIÓN ESP-NOW
// ════════════════════════════════════════════════════════════════

void applyESPNowConfig() {
  Serial.println("\n📻 Aplicando configuración ESP-NOW...");

  if (currentChannel < 1 || currentChannel > 13) currentChannel = 1;
  if (currentPower < 2 || currentPower > 20) currentPower = 20;
  if (currentRate < 0 || currentRate > 3) currentRate = 1;

  esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
  delay(100);
  esp_wifi_set_max_tx_power(currentPower * 4);
  delay(100);

  Serial.println("✅ Configuración aplicada");
}

// ════════════════════════════════════════════════════════════════
// 📡 CALCULAR DELAYS
// ════════════════════════════════════════════════════════════════

int getInterPacketDelay() {
  if (currentRate == 0) return 32;
  if (currentRate == 1) return 24;
  if (currentRate == 2) return 16;
  return 12;
}

// ════════════════════════════════════════════════════════════════
// 📥 LEER DATOS USB - MEJORADO
// ════════════════════════════════════════════════════════════════

void readUSBData() {
  if (!Serial.available()) return;

  int firstByte = Serial.peek();

  // Detectar comando (ASCII printable)
  if (firstByte >= 32 && firstByte < 127) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd.length() > 0) {
      handleUSBCommand(cmd);
    }
  } 
  // Raw binary data (mientras estamos en RX_STREAMING)
  else if (currentTxState == STATE_RX_STREAMING && firstByte >= 0) {
    uint8_t byte = Serial.read();
    bufferWrite(byte);
    rxBytesReceived++;

    // Feedback cada 5KB
    if (rxBytesReceived % 5120 == 0) {
      Serial.printf("ACK:BUFFERED:%u/%u\n", rxBytesReceived, rxFileSize);
      Serial.flush();
    }
  }
  else {
    Serial.read(); // Descartar bytes no esperados
  }
}

// ════════════════════════════════════════════════════════════════
// 🎯 MANEJO DE COMANDOS USB - MEJORADO
// ══════════════════════════════════════════��═════════════════════

void handleUSBCommand(String cmd) {
  cmd.trim();
  Serial.printf("📩 Comando recibido: %s\n", cmd.c_str());

  if (cmd.startsWith("START:")) {
    // START:archivo.bin:12345
    int firstColon = cmd.indexOf(':', 6);
    if (firstColon < 0) {
      Serial.println("ERROR:INVALID_FORMAT");
      Serial.flush();
      return;
    }

    rxFilename = cmd.substring(6, firstColon);
    String sizeStr = cmd.substring(firstColon + 1);
    rxFileSize = sizeStr.toInt();

    Serial.printf("📋 Parseado:\n");
    Serial.printf("   Archivo: %s\n", rxFilename.c_str());
    Serial.printf("   Tamaño: %u bytes\n", rxFileSize);

    if (rxFileSize <= 0) {
      Serial.println("ERROR:INVALID_SIZE");
      Serial.flush();
      return;
    }

    currentTxState = STATE_RX_STREAMING;
    rxBytesReceived = 0;
    txChunksSent = 0;
    currentFileID = (uint32_t)millis() ^ (rxFileSize << 8) ^ (++fileIDCounter);
    writePtr = 0;
    readPtr = 0;
    totalBytesWritten = 0;

    // Importante: enviar respuesta con flush
    Serial.printf("✅ OK:READY_TO_RX:%u\n", rxFileSize);
    Serial.flush();
    delay(100);
    
    Serial.printf("   FileID: 0x%08X\n", currentFileID);
    Serial.flush();
  }
  else if (cmd == "END") {
    if (currentTxState != STATE_RX_STREAMING) {
      Serial.println("ERROR:NOT_RECEIVING");
      Serial.flush();
      return;
    }

    currentTxState = STATE_FINALIZING;
    Serial.printf("✅ OK:FINALIZING\n");
    Serial.printf("   Recibido: %u / %u bytes\n", rxBytesReceived, rxFileSize);
    Serial.flush();
  }
  else if (cmd.startsWith("CONFIG:")) {
    int p, ch, r;
    sscanf(cmd.c_str(), "CONFIG:%d:%d:%d", &p, &ch, &r);
    currentPower = p;
    currentChannel = ch;
    currentRate = r;
    saveESPNowConfig();
    applyESPNowConfig();
    Serial.println("✅ OK:CONFIG_SET");
    Serial.flush();
  }
  else if (cmd == "PING") {
    Serial.println("PONG");
    Serial.flush();
  }
  else if (cmd == "STATUS") {
    Serial.printf("STATUS:state=%d:buffered=%u:received=%u\n",
      currentTxState, getBufferFilledBytes(), rxBytesReceived);
    Serial.flush();
  }
  else {
    Serial.printf("ERROR:UNKNOWN_COMMAND:%s\n", cmd.c_str());
    Serial.flush();
  }
}

// ════════════════════════════════════════════════════════════════
// 💾 BUFFER CIRCULAR - OPERACIONES
// ════════════════════════════════════════════════════════════════

void bufferWrite(uint8_t byte) {
  uint16_t nextPtr = (writePtr + 1) % BUFFER_SIZE;
  
  if (nextPtr == readPtr) {
    // Buffer lleno - implementar política: descartar el más antiguo
    readPtr = (readPtr + 1) % BUFFER_SIZE;
    Serial.println("⚠️  BUFFER_OVERFLOW - descartando datos antiguos");
  }

  circularBuffer[writePtr] = byte;
  writePtr = nextPtr;
  totalBytesWritten++;
}

void bufferRead(uint8_t* data, size_t len) {
  for (size_t i = 0; i < len; i++) {
    data[i] = circularBuffer[readPtr];
    readPtr = (readPtr + 1) % BUFFER_SIZE;
  }
}

uint16_t getBufferFilledBytes() {
  if (writePtr >= readPtr) {
    return writePtr - readPtr;
  }
  return BUFFER_SIZE - readPtr + writePtr;
}

bool bufferHasData() {
  return getBufferFilledBytes() >= CHUNK_SIZE_ESPNOW;
}

// ════════════════════════════════════════════════════════════════
// 📡 TRANSMITIR CHUNKS DESDE BUFFER
// ════════════════════════════════════════════════════════════════

void transmitNextChunks() {
  if (transmitting) return;

  uint16_t availableBytes = getBufferFilledBytes();
  uint16_t chunksToSend = availableBytes / CHUNK_SIZE_ESPNOW;

  if (chunksToSend == 0) return;

  transmitting = true;

  for (uint16_t i = 0; i < chunksToSend; i++) {
    uint8_t chunkData[CHUNK_SIZE_ESPNOW];
    bufferRead(chunkData, CHUNK_SIZE_ESPNOW);

    uint16_t totalChunks = (rxFileSize + CHUNK_SIZE_ESPNOW - 1) / CHUNK_SIZE_ESPNOW;

    if (sendDataChunk(currentFileID, txChunksSent, totalChunks, chunkData, CHUNK_SIZE_ESPNOW)) {
      totalESPNowPacketsSent++;
    }

    txChunksSent++;

    delay(getInterPacketDelay());
    yield();

    // Mostrar progreso cada 10 chunks
    if (txChunksSent % 10 == 0) {
      Serial.printf("📡 TX Progress: %u chunks enviados\n", txChunksSent);
    }
  }

  transmitting = false;
}

// ════════════════════════════════════════════════════════════════
// 🏁 FINALIZAR TRANSMISIÓN
// ════════════════════════════════════════════════════════════════

void finalizeBroadcast() {
  Serial.println("\n╔════════════════════════════════════════╗");
  Serial.println("║     🏁 FINALIZANDO TRANSMISIÓN       ║");
  Serial.println("╚════════════════════════════════════════╝");

  uint16_t totalChunks = (rxFileSize + CHUNK_SIZE_ESPNOW - 1) / CHUNK_SIZE_ESPNOW;

  // Enviar FILE_END múltiples veces
  for (int e = 0; e < FILE_END_REPEAT; e++) {
    if (sendFileEnd(currentFileID, totalChunks)) {
      Serial.printf("✅ FILE_END enviado (%d/%d)\n", e + 1, FILE_END_REPEAT);
    }
    delay(100);
    yield();
  }

  float transmissionTime = 0;
  if (espnowTransmissionStartTime > 0) {
    transmissionTime = (millis() - espnowTransmissionStartTime) / 1000.0;
  }

  Serial.printf("✅ TX_COMPLETE:%u:%u:%.2f\n", rxFileSize, totalESPNowPacketsSent, transmissionTime);
  Serial.printf("📊 Estadísticas:\n");
  Serial.printf("   Archivo: %s\n", rxFilename.c_str());
  Serial.printf("   Tamaño: %u bytes\n", rxFileSize);
  Serial.printf("   Chunks: %u\n", txChunksSent);
  Serial.printf("   Paquetes ESP-NOW: %u\n", totalESPNowPacketsSent);
  Serial.printf("   Reintentos: %u\n", totalESPNowRetries);
  Serial.println("");

  // Reset
  rxBytesReceived = 0;
  txChunksSent = 0;
  totalESPNowPacketsSent = 0;
  totalESPNowRetries = 0;
  writePtr = 0;
  readPtr = 0;
}

// ════════════════════════════════════════════════════════════════
// 📤 TRANSMITIR MANIFEST
// ════════════════════════════════════════════════════════════════

bool sendManifest(uint32_t fileID, uint32_t totalSize, uint16_t totalChunks, const String& fileName) {
  uint8_t nameLen = (uint8_t)min((size_t)fileName.length(), (size_t)100);
  uint8_t manifestPkt[2 + 4 + 4 + 2 + 2 + 1 + 100 + 2];
  size_t idx = 0;

  manifestPkt[idx++] = MANIFEST_MAGIC_1;
  manifestPkt[idx++] = MANIFEST_MAGIC_2;
  memcpy(manifestPkt + idx, &fileID, 4); idx += 4;
  memcpy(manifestPkt + idx, &totalSize, 4); idx += 4;
  memcpy(manifestPkt + idx, &totalChunks, 2); idx += 2;
  uint16_t chunkSize = CHUNK_SIZE_ESPNOW;
  memcpy(manifestPkt + idx, &chunkSize, 2); idx += 2;
  manifestPkt[idx++] = nameLen;
  memcpy(manifestPkt + idx, fileName.c_str(), nameLen); idx += nameLen;

  uint16_t crc = crc16_ccitt(manifestPkt, idx);
  memcpy(manifestPkt + idx, &crc, 2); idx += 2;

  int state = esp_now_send(broadcastAddress, manifestPkt, idx);
  if (state == ESP_OK) {
    return true;
  }

  return false;
}

// ════════════════════════════════════════════════════════════════
// 📤 TRANSMITIR DATA CHUNK
// ════════════════════════════════════════════════════════════════

bool sendDataChunk(uint32_t fileID, uint16_t chunkIndex, uint16_t totalChunks, uint8_t* data, size_t len) {
  uint8_t dataPkt[2 + 4 + 2 + 2 + CHUNK_SIZE_ESPNOW + 2];
  size_t idx = 0;

  dataPkt[idx++] = DATA_MAGIC_1;
  dataPkt[idx++] = DATA_MAGIC_2;
  memcpy(dataPkt + idx, &fileID, 4); idx += 4;
  memcpy(dataPkt + idx, &chunkIndex, 2); idx += 2;
  memcpy(dataPkt + idx, &totalChunks, 2); idx += 2;
  memcpy(dataPkt + idx, data, len); idx += len;

  uint16_t crc = crc16_ccitt(dataPkt, idx);
  memcpy(dataPkt + idx, &crc, 2); idx += 2;

  for (int retries = 0; retries < MAX_RETRIES; retries++) {
    int state = esp_now_send(broadcastAddress, dataPkt, idx);
    if (state == ESP_OK) return true;

    totalESPNowRetries++;
    delay(10);
    yield();
  }

  return false;
}

// ════════════════════════════════════════════════════════════════
// 📤 TRANSMITIR PARITY (FEC - XOR)
// ══════════════════════════════════════════════��═════════════════

bool sendParityChunk(uint32_t fileID, uint16_t blockIndex, uint8_t* parityData, size_t len) {
  uint8_t parityPkt[2 + 4 + 2 + CHUNK_SIZE_ESPNOW + 2];
  size_t idx = 0;

  parityPkt[idx++] = PARITY_MAGIC_1;
  parityPkt[idx++] = PARITY_MAGIC_2;
  memcpy(parityPkt + idx, &fileID, 4); idx += 4;
  memcpy(parityPkt + idx, &blockIndex, 2); idx += 2;
  memcpy(parityPkt + idx, parityData, len); idx += len;

  uint16_t crc = crc16_ccitt(parityPkt, idx);
  memcpy(parityPkt + idx, &crc, 2); idx += 2;

  int state = esp_now_send(broadcastAddress, parityPkt, idx);
  return (state == ESP_OK);
}

// ════════════════════════════════════════════════════════════════
// 📤 TRANSMITIR FILE_END
// ════════════════════════════════════════════════════════════════

bool sendFileEnd(uint32_t fileID, uint16_t totalChunks) {
  uint8_t endPkt[2 + 4 + 2 + 2];
  size_t idx = 0;

  endPkt[idx++] = FILE_END_MAGIC_1;
  endPkt[idx++] = FILE_END_MAGIC_2;
  memcpy(endPkt + idx, &fileID, 4); idx += 4;
  memcpy(endPkt + idx, &totalChunks, 2); idx += 2;

  uint16_t crc = crc16_ccitt(endPkt, idx);
  memcpy(endPkt + idx, &crc, 2); idx += 2;

  int state = esp_now_send(broadcastAddress, endPkt, idx);
  return (state == ESP_OK);
}