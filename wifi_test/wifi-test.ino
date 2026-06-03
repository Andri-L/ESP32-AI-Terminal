#include "WiFi.h"

const char* WIFI_SSID = "LINA SOFI";
const char* WIFI_PASS = "Lina0429";

void setup() {
  Serial.begin(115200);
  delay(1000);

  Serial.print("Connecting to WiFi...");
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("");
  Serial.println("Connected!");
  Serial.print("ESP32 IP address: ");
  Serial.println(WiFi.localIP());
}

void loop() {}