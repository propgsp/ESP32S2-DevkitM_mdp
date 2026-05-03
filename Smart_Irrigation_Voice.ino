#include <TensorFlowLite_ESP32.h>
#include "tensorflow/lite/micro/all_ops_resolver.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/schema/schema_generated.h"
#include "tensorflow/lite/micro/micro_error_reporter.h"
#include "model.h"
#include "arduinoFFT.h"

// --- HARDWARE PINS ---
#define RELAY_PIN     4
#define SWITCH_PIN    15
#define MOISTURE_PIN  1

// --- LED UI PINS ---
#define LED_MANUAL_PIN  5
#define LED_AUTO_PIN    6
#define LED_PUMP_ON_PIN 7
#define LED_PUMP_OFF_PIN 8

int ledPin = 2;

// --- THRESHOLDS ---
const int DRY_THRESHOLD = 6500;

// --- TENSORFLOW GLOBALS ---
const tflite::Model*        tfl_model   = nullptr;
tflite::MicroInterpreter*   interpreter = nullptr;
tflite::AllOpsResolver      resolver;
tflite::MicroErrorReporter  micro_error_reporter;

const int kTensorArenaSize = 80 * 1024;
uint8_t tensor_arena[kTensorArenaSize];   // static — no malloc

TfLiteTensor* input  = nullptr;
TfLiteTensor* output = nullptr;

// --- AUDIO PROCESSING ---
const int      SAMPLE_RATE  = 16000;
const uint16_t FRAME_LENGTH = 256;
const int      FRAME_STEP   = 128;
const int      TOTAL_FRAMES = 124;
// Total samples the model expects: last frame starts at (TOTAL_FRAMES-1)*FRAME_STEP
const int TOTAL_SAMPLES = (TOTAL_FRAMES - 1) * FRAME_STEP + FRAME_LENGTH; // 16 128

// Sliding window — holds exactly two frames (overlap-add friendly)
// Ring index wraps with & (FRAME_LENGTH*2 - 1)
static int16_t  ring_buf[FRAME_LENGTH * 2];
static uint16_t ring_head = 0;  // next write position in ring_buf
static int      samples_received = 0;
static int      frames_written   = 0;

double vReal[FRAME_LENGTH];
double vImag[FRAME_LENGTH];
arduinoFFT FFT = arduinoFFT();

// Normalization stats accumulated on-the-fly (Welford mean)
static double run_mean   = 0.0;
static double run_max    = 0.1;
static long   run_count  = 0;

// ---------------------------------------------------------------
// Push one sample into the ring and run FFT if a frame is ready.
// Called after every FRAME_STEP new samples arrive.
// ---------------------------------------------------------------
inline void push_sample(int16_t s) {
  // Update running mean and max for normalisation
  run_count++;
  double delta  = s - run_mean;
  run_mean     += delta / run_count;
  double absval = abs((double)s - run_mean);
  if (absval > run_max) run_max = absval;

  ring_buf[ring_head & (FRAME_LENGTH * 2 - 1)] = s;
  ring_head++;
  samples_received++;

  // A new frame is ready every FRAME_STEP samples
  if ((samples_received % FRAME_STEP == 0) &&
      (samples_received >= FRAME_LENGTH)    &&
      (frames_written < TOTAL_FRAMES)) {

    // Copy the last FRAME_LENGTH samples out of the ring
    uint16_t start = ring_head - FRAME_LENGTH;
    for (int j = 0; j < FRAME_LENGTH; j++) {
      double centered = ring_buf[(start + j) & (FRAME_LENGTH * 2 - 1)] - run_mean;
      vReal[j] = centered / run_max;
      vImag[j] = 0.0;
    }

    FFT.Windowing(vReal, FRAME_LENGTH, FFT_WIN_TYP_HANN, FFT_FORWARD);
    FFT.Compute(vReal, vImag, FRAME_LENGTH, FFT_FORWARD);
    FFT.ComplexToMagnitude(vReal, vImag, FRAME_LENGTH);

    int row = frames_written;
    for (int col = 0; col < 129; col++) {
      input->data.f[row * 129 + col] = (float)vReal[col];
    }
    frames_written++;
  }
}

// ---------------------------------------------------------------
// Reset streaming state between inferences
// ---------------------------------------------------------------
void reset_audio_state() {
  ring_head        = 0;
  samples_received = 0;
  frames_written   = 0;
  run_mean         = 0.0;
  run_max          = 0.1;
  run_count        = 0;
  memset(ring_buf, 0, sizeof(ring_buf));
}

void setup() {
  Serial.begin(115200);
  Serial.setTimeout(50);   // short timeout — we read in small chunks
  delay(1000);

  // --- HARDWARE ---
  pinMode(RELAY_PIN,      OUTPUT); digitalWrite(RELAY_PIN, HIGH);
  pinMode(SWITCH_PIN,     INPUT_PULLUP);
  pinMode(MOISTURE_PIN,   INPUT);
  pinMode(LED_MANUAL_PIN, OUTPUT);
  pinMode(LED_AUTO_PIN,   OUTPUT);
  pinMode(LED_PUMP_ON_PIN,  OUTPUT);
  pinMode(LED_PUMP_OFF_PIN, OUTPUT);
  pinMode(ledPin, OUTPUT);  digitalWrite(ledPin, LOW);

  digitalWrite(LED_PUMP_ON_PIN,  LOW);
  digitalWrite(LED_PUMP_OFF_PIN, HIGH);

  Serial.println("\n--- Initializing Smart Irrigation System ---");

  // --- TENSORFLOW ---
  // tensor_arena is static — no heap allocation needed
  tfl_model = tflite::GetModel(model_tflite);
  static tflite::MicroInterpreter static_interp(
      tfl_model, resolver, tensor_arena, kTensorArenaSize, &micro_error_reporter);
  interpreter = &static_interp;
  interpreter->AllocateTensors();

  input  = interpreter->input(0);
  output = interpreter->output(0);

  Serial.println("System Ready!");
  Serial.println("READY");
}

// ---------------------------------------------------------------
// Receive exactly `n` bytes from Serial into buf[], non-blocking
// in chunks. Returns true when all bytes have arrived.
// ---------------------------------------------------------------
static uint8_t  serial_chunk[64];
static uint32_t bytes_needed    = 0;
static uint32_t bytes_collected = 0;
static bool     receiving       = false;

bool receive_audio_chunk() {
  while (Serial.available() > 0 && bytes_collected < bytes_needed) {
    int got = Serial.readBytes(
        (char*)serial_chunk,
        min((uint32_t)sizeof(serial_chunk), bytes_needed - bytes_collected));

    for (int i = 0; i + 1 < got; i += 2) {
      int16_t sample;
      memcpy(&sample, &serial_chunk[i], 2);
      push_sample(sample);
    }
    bytes_collected += got;
  }
  return bytes_collected >= bytes_needed;
}

void loop() {
  bool isManualMode = (digitalRead(SWITCH_PIN) == HIGH);

  // ==========================================
  // MODE 1: MANUAL (VOICE CONTROL)
  // ==========================================
  if (isManualMode) {
    digitalWrite(LED_MANUAL_PIN, HIGH);
    digitalWrite(LED_AUTO_PIN,   LOW);

    if (!receiving) {
      // Wait for the AUDIO_START header
      if (Serial.available() > 0) {
        String header = Serial.readStringUntil('\n');
        header.trim();
        if (header == "AUDIO_START") {
          reset_audio_state();
          bytes_needed    = (uint32_t)TOTAL_SAMPLES * sizeof(int16_t);
          bytes_collected = 0;
          receiving       = true;
        } else {
          // Flush stale bytes, re-announce ready
          while (Serial.available()) Serial.read();
          Serial.println("READY");
        }
      }
    } else {
      // Streaming receive — process a chunk each loop iteration
      bool done = receive_audio_chunk();

      if (done) {
        receiving = false;

        // All frames are already in input tensor — just invoke
        interpreter->Invoke();

        float prob_on = output->data.f[0];
        Serial.print("ESP32 AI CONFIDENCE: ");
        Serial.print(prob_on * 100);
        Serial.println("%");

        if (prob_on > 0.70f) {
          Serial.println("*** MANUAL OVERRIDE: PUMP ACTIVATED ***");
          digitalWrite(RELAY_PIN,      LOW);
          digitalWrite(LED_PUMP_ON_PIN,  HIGH);
          digitalWrite(LED_PUMP_OFF_PIN, LOW);

          delay(2000);

          digitalWrite(RELAY_PIN,      HIGH);
          digitalWrite(LED_PUMP_ON_PIN,  LOW);
          digitalWrite(LED_PUMP_OFF_PIN, HIGH);

          Serial.println("Action: Pump Cycle Complete.");
        } else {
          Serial.println("Word not recognized. Say 'ON' clearly.");
        }

        while (Serial.available()) Serial.read();
        Serial.println("READY");
      }
    }

  // ==========================================
  // MODE 2: AUTOMATIC (SOIL MOISTURE SENSOR)
  // ==========================================
  } else {
    digitalWrite(LED_MANUAL_PIN, LOW);
    digitalWrite(LED_AUTO_PIN,   HIGH);

    int moistureLevel = analogRead(MOISTURE_PIN);
    Serial.print("Auto Mode Active | Moisture Level: ");
    Serial.println(moistureLevel);

    if (moistureLevel > DRY_THRESHOLD) {
      digitalWrite(RELAY_PIN,       LOW);
      digitalWrite(LED_PUMP_ON_PIN,  HIGH);
      digitalWrite(LED_PUMP_OFF_PIN, LOW);
      Serial.println("Action: Soil is dry. Pump is running...");
    } else {
      digitalWrite(RELAY_PIN,       HIGH);
      digitalWrite(LED_PUMP_ON_PIN,  LOW);
      digitalWrite(LED_PUMP_OFF_PIN, HIGH);
    }

    delay(1000);
  }
}