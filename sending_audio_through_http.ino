//voice to text//
#include <Arduino.h>
#include <driver/i2s.h>
#include <SD.h>
#include <SPI.h>
#include <WiFi.h>
#include <HTTPClient.h>

// ---------- WIFI CONFIG ----------
const char* ssid = "Realme 7 Pro";
const char* password = "987654321";
//const char* uploadURL = "http://192.168.68.104:5000/upload"; // change to your server endpoint

// ---------- PIN CONFIGURATION ----------
#define SD_CS    10
#define SD_MOSI  11
#define SD_MISO  13
#define SD_SCK   12

#define I2S_WS   45   // LRCL
#define I2S_SD   48   // DOUT
#define I2S_SCK  47   // BCLK
#define I2S_PORT I2S_NUM_0

// ---------- RECORDING SETTINGS ----------
#define SAMPLE_RATE     16000      // Hz
#define BUFFER_LEN      1024
#define BITS_PER_SAMPLE I2S_BITS_PER_SAMPLE_32BIT
#define FILE_DURATION   30        // seconds
#define NUM_RECORDINGS  200         // reduce for testing
#define GAIN_FACTOR     8          // digital gain (adjust 2–8)

// ---------- WAV HEADER ----------
typedef struct {
  char riff[4];                
  uint32_t chunkSize;
  char wave[4];                
  char fmt[4];                 
  uint32_t subchunk1Size;
  uint16_t audioFormat;
  uint16_t numChannels;
  uint32_t sampleRate;
  uint32_t byteRate;
  uint16_t blockAlign;
  uint16_t bitsPerSample;
  char subchunk2ID[4];         
  uint32_t subchunk2Size;
} WAVHeader;

File recordingFile;

// ---------- WAV HEADER CREATION ----------
void createWAVHeader(WAVHeader *header, uint32_t sampleRate, uint16_t bitsPerSample, uint16_t channels) {
  memcpy(header->riff, "RIFF", 4);
  memcpy(header->wave, "WAVE", 4);
  memcpy(header->fmt, "fmt ", 4);
  memcpy(header->subchunk2ID, "data", 4);

  header->subchunk1Size = 16;
  header->audioFormat = 1;
  header->numChannels = channels;
  header->sampleRate = sampleRate;
  header->bitsPerSample = bitsPerSample;
  header->byteRate = sampleRate * channels * (bitsPerSample / 8);
  header->blockAlign = channels * (bitsPerSample / 8);
  header->subchunk2Size = 0;
  header->chunkSize = 36 + header->subchunk2Size;
}

// ---------- AUDIO PROCESS ----------
int16_t processAudioSample(int32_t rawSample) {
  int16_t sample16 = (int16_t)(rawSample >> 16);
  int32_t amplified = (int32_t)sample16 * GAIN_FACTOR;
  if (amplified > 32767) amplified = 32767;
  if (amplified < -32768) amplified = -32768;
  return (int16_t)amplified;
}

// ---------- I2S SETUP ----------
void setupI2S() {
  const i2s_config_t i2sConfig = {
    .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
    .sample_rate = SAMPLE_RATE,
    .bits_per_sample = BITS_PER_SAMPLE,
    .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
    .dma_buf_count = 8,
    .dma_buf_len = BUFFER_LEN,
    .use_apll = false
  };

  const i2s_pin_config_t pinConfig = {
    .bck_io_num = I2S_SCK,
    .ws_io_num = I2S_WS,
    .data_out_num = -1,
    .data_in_num = I2S_SD
  };

  i2s_driver_install(I2S_PORT, &i2sConfig, 0, NULL);
  i2s_set_pin(I2S_PORT, &pinConfig);
}

// ---------- RECORDING ----------
void startNewRecording(const char *filename) {
  recordingFile = SD.open(filename, FILE_WRITE);
  if (!recordingFile) {
    Serial.print("Failed to open file: ");
    Serial.println(filename);
    return;
  }
  WAVHeader header;
  createWAVHeader(&header, SAMPLE_RATE, 16, 1);
  recordingFile.write((uint8_t *)&header, sizeof(WAVHeader));
}

void recordAudio(uint32_t durationSeconds) {
  uint32_t startMillis = millis();
  size_t bytesRead;
  int32_t i2sBuffer[BUFFER_LEN];

  while ((millis() - startMillis) < durationSeconds * 1000) {
    i2s_read(I2S_PORT, (char *)i2sBuffer, sizeof(i2sBuffer), &bytesRead, portMAX_DELAY);
    for (int i = 0; i < bytesRead / 4; i++) {
      int16_t sample16 = processAudioSample(i2sBuffer[i]);
      recordingFile.write((uint8_t *)&sample16, sizeof(int16_t));
    }
  }
}

void finalizeRecording() {
  if (recordingFile) {
    uint32_t fileSize = recordingFile.size();
    WAVHeader header;
    createWAVHeader(&header, SAMPLE_RATE, 16, 1);
    header.subchunk2Size = fileSize - sizeof(WAVHeader);
    header.chunkSize = 36 + header.subchunk2Size;
    recordingFile.seek(0);
    recordingFile.write((uint8_t *)&header, sizeof(WAVHeader));
    recordingFile.close();
  }
}
bool uploadFileStream(const char* path) {
  File file = SD.open(path, FILE_READ);
  if (!file) {
    Serial.println("Failed to open file");
    return false;
  }

  const char* serverHost = "13.233.11.208";  // replace with your server IP/domain
  const int serverPort = 80;                 // 80 = http, 443 = https
  const char* uploadPath = "/upload";        // replace with your server endpoint

  WiFiClient client;
  if (!client.connect(serverHost, serverPort)) {
    Serial.println("Connection failed");
    file.close();
    return false;
  }

  String boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW";
  String bodyStart = "--" + boundary + "\r\n";
  bodyStart += "Content-Disposition: form-data; name=\"audio\"; filename=\"" + String(path) + "\"\r\n";
  bodyStart += "Content-Type: audio/wav\r\n\r\n";

  String bodyEnd = "\r\n--" + boundary + "--\r\n";

  size_t totalLen = bodyStart.length() + file.size() + bodyEnd.length();

  // HTTP request headers
  client.printf("POST %s HTTP/1.1\r\n", uploadPath);
  client.printf("Host: %s\r\n", serverHost);
  client.printf("Content-Type: multipart/form-data; boundary=%s\r\n", boundary.c_str());
  client.printf("Content-Length: %u\r\n", (unsigned)totalLen);
  client.print("Connection: close\r\n\r\n");

  // Body start
  client.print(bodyStart);

  // Send file in chunks
  const size_t bufSize = 1024;
  uint8_t buf[bufSize];
  while (file.available()) {
    size_t len = file.read(buf, bufSize);
    if (len > 0) client.write(buf, len);
  }
  file.close();

  // Body end
  client.print(bodyEnd);

  // Read server response
  while (client.connected() || client.available()) {
    if (client.available()) {
      String line = client.readStringUntil('\n');
      Serial.println(line);
    }
  }
  client.stop();
  return true;
}





// ---------- SETUP ----------
void setup() {
  Serial.begin(115200);

  // WiFi
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected!");

  // SD card
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI)) {
    Serial.println("SD card initialization failed!");
    while (1);
  }
  Serial.println("SD card initialized.");

  setupI2S();

  for (int i = 0; i < NUM_RECORDINGS; i++) {
    char filename[20];
    sprintf(filename, "/rec%d.wav", i + 1);

    Serial.print("Recording ");
    Serial.println(filename);

    startNewRecording(filename);
    recordAudio(FILE_DURATION);
    finalizeRecording();

    Serial.println("Uploading...");
    if (uploadFileStream(filename)) {
      Serial.println("Upload successful!");
    } else {
      Serial.println("Upload failed!");
    }
  }

  Serial.println("All done!");
}

void loop() {
}
