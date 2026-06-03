#include "driver/i2s.h"
#include <math.h>

#define I2S_DOUT    22
#define I2S_LRC     25
#define I2S_BCLK    26
#define SAMPLE_RATE 16000
#define AMPLITUDE   10000

void setup() {
  Serial.begin(115200);

  const i2s_config_t i2s_config = {
    .mode                 = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
    .sample_rate          = SAMPLE_RATE,
    .bits_per_sample      = I2S_BITS_PER_SAMPLE_16BIT,
    .channel_format       = I2S_CHANNEL_FMT_ONLY_LEFT,
    .communication_format = I2S_COMM_FORMAT_STAND_I2S,
    .intr_alloc_flags     = 0,
    .dma_buf_count        = 8,
    .dma_buf_len          = 64,
    .use_apll             = false,
  };

  const i2s_pin_config_t pin_config = {
    .bck_io_num   = I2S_BCLK,
    .ws_io_num    = I2S_LRC,
    .data_out_num = I2S_DOUT,
    .data_in_num  = I2S_PIN_NO_CHANGE,
  };

  i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
  i2s_set_pin(I2S_NUM_0, &pin_config);
  Serial.println("Playing Fur Elise!");
}

void playNote(float freq, int durationMs) {
  int totalSamples = (SAMPLE_RATE * durationMs) / 1000;
  int16_t buffer[64];
  int written = 0;
  size_t bytes_written;

  while (written < totalSamples) {
    int chunk = min(64, totalSamples - written);
    for (int i = 0; i < chunk; i++) {
      float t = (float)(written + i) / SAMPLE_RATE;
      // Fade out last 10% of note to avoid clicks
      float fade = 1.0;
      if (written + i > totalSamples * 0.9) {
        fade = (float)(totalSamples - written - i) / (totalSamples * 0.1);
      }
      if (freq == 0) {
        buffer[i] = 0; // silence/rest
      } else {
        buffer[i] = (int16_t)(sin(2.0 * PI * freq * t) * AMPLITUDE * fade);
      }
    }
    i2s_write(I2S_NUM_0, buffer, chunk * sizeof(int16_t), &bytes_written, portMAX_DELAY);
    written += chunk;
  }
}

// Note frequencies
#define C4   261.63
#define Cs4  277.18
#define D4   293.66
#define Ds4  311.13
#define E4   329.63
#define F4   349.23
#define Fs4  369.99
#define G4   392.00
#define GS4  415.30
#define A4   440.00
#define Bb4  466.16
#define B4   493.88
#define C5   523.25
#define Cs5  554.37
#define D5   587.33
#define Ds5  622.25
#define E5   659.25
#define F5   698.46
#define E3   164.81
#define F3   174.61
#define Fs3  185.00
#define G3   196.00
#define A3   220.00
#define Bb3  233.08
#define B3   246.94
#define As4  466.16
#define REST 0

/*
// Duration helpers (130 BPM)
#define Q  450   // quarter note
#define E  225   // eighth note
#define S  112   // sixteenth note
#define H  900   // half note
*/

// Para JAZZ LENTO MÁS RÁPIDO (100 BPM):
#define Q  600   // quarter note
#define E  300   // eighth note
#define S  150   // sixteenth note
#define H  1200  // half note

/*
// Para JAZZ LENTO (70 BPM)
#define Q  857   // quarter note (negra)
#define E  429   // eighth note (corchea)
#define S  214   // sixteenth note (semicorchea)
#define H  1714  // half note (blanca)
*/

/*
void loop() {
  // Fur Elise - main theme
  playNote(E5,  E);
  playNote(Ds5, E);
  playNote(E5,  E);
  playNote(Ds5, E);
  playNote(E5,  E);
  playNote(B4,  E);
  playNote(D5,  E);
  playNote(C5,  E);
  playNote(A4,  Q);
  playNote(REST,E);
  playNote(C4,  E);
  playNote(E4,  E);
  playNote(A4,  E);
  playNote(B4,  Q);
  playNote(REST,E);
  playNote(E4,  E);
  playNote(GS4, E);
  playNote(B4,  E);
  playNote(C5,  Q);
  playNote(REST,E);
  playNote(E4,  E);
  playNote(E5,  E);
  playNote(Ds5, E);
  playNote(E5,  E);
  playNote(Ds5, E);
  playNote(E5,  E);
  playNote(B4,  E);
  playNote(D5,  E);
  playNote(C5,  E);
  playNote(A4,  Q);
  playNote(REST,E);
  playNote(C4,  E);
  playNote(E4,  E);
  playNote(A4,  E);
  playNote(B4,  Q);
  playNote(REST,E);
  playNote(E4,  E);
  playNote(C5,  E);
  playNote(B4,  E);
  playNote(A4,  H);
  playNote(REST, Q);

  delay(1000); // pause before repeating
}
*/

// ==========================================
// JAZZ LENTO - 4/4
// Acorde: MIm
// ==========================================

void loop() {
// ==========================================
// THE PINK PANTHER THEME - Henry Mancini
// Fuente: github.com/robsoncouto/arduino-songs
// ==========================================

// --- Intro ---
  playNote(REST, H);
  playNote(REST, Q);
  playNote(REST, E);
  playNote(Ds4,  E);

  playNote(E4,   Q);
  playNote(REST, E);
  playNote(Fs4,  E);
  playNote(G4,   Q);
  playNote(REST, E);
  playNote(Ds4,  E);

  playNote(E4,   E);
  playNote(Fs4,  E);
  playNote(G4,   E);
  playNote(C5,   E);
  playNote(B4,   E);
  playNote(E4,   E);
  playNote(G4,   E);
  playNote(B4,   E);

  playNote(As4,  H);
  playNote(A4,   S);
  playNote(G4,   S);
  playNote(E4,   S);
  playNote(D4,   S);
  playNote(E4,   H);

// --- Segunda frase ---
  playNote(REST, Q);
  playNote(REST, E);
  playNote(Ds4,  Q);

  playNote(E4,   Q);
  playNote(REST, E);
  playNote(Fs4,  E);
  playNote(G4,   Q);
  playNote(REST, E);
  playNote(Ds4,  E);

  playNote(E4,   E);
  playNote(Fs4,  E);
  playNote(G4,   E);
  playNote(C5,   E);
  playNote(B4,   E);
  playNote(G4,   E);
  playNote(B4,   E);
  playNote(E5,   E);

  playNote(Ds5,  H);
  playNote(Ds5,  H);  // nota larga original = redonda

// --- Tercera frase ---
  playNote(D5,   H);
  playNote(REST, Q);
  playNote(REST, E);
  playNote(Ds4,  E);

  playNote(E4,   Q);
  playNote(REST, E);
  playNote(Fs4,  E);
  playNote(G4,   Q);
  playNote(REST, E);
  playNote(Ds4,  E);

  playNote(E4,   E);
  playNote(Fs4,  E);
  playNote(G4,   E);
  playNote(C5,   E);
  playNote(B4,   E);
  playNote(E4,   E);
  playNote(G4,   E);
  playNote(B4,   E);

  playNote(As4,  H);
  playNote(A4,   S);
  playNote(G4,   S);
  playNote(E4,   S);
  playNote(D4,   S);

// --- Puente (sección nueva) ---
  playNote(E4,   Q);
  playNote(REST, Q);

  playNote(REST, Q);
  playNote(E5,   E);
  playNote(D5,   E);
  playNote(B4,   E);
  playNote(A4,   E);
  playNote(G4,   E);
  playNote(E4,   E);

  // Motivo oscilante característico de la Pantera Rosa
  playNote(As4,  S);
  playNote(A4,   E);
  playNote(As4,  S);
  playNote(A4,   E);
  playNote(As4,  S);
  playNote(A4,   E);
  playNote(As4,  S);
  playNote(A4,   E);

// --- Final ---
  playNote(G4,   S);
  playNote(E4,   S);
  playNote(D4,   S);
  playNote(E4,   S);
  playNote(E4,   S);
  playNote(E4,   H);

  delay(2000);
}