
#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266Influxdb.h>

#include <DHT.h>
#define DHTPIN 12     // DHT connection
#define DHTTYPE DHT22   // DHT 22


DHT dht(DHTPIN, DHTTYPE);

const char *INFLUXDB_HOST = "SUPER_SECRET";
const uint16_t INFLUXDB_PORT = 8086;

const char *DATABASE = "esp8266";
const char *DB_USER = "SUPER_SECRET";
const char *DB_PASSWORD = "SUPER_SECRET";

ESP8266WiFiMulti WiFiMulti;
Influxdb influxdb(INFLUXDB_HOST, INFLUXDB_PORT);

void setup() {
        Serial.begin(115200);
        WiFiMulti.addAP("SUPER_SECRET_SSID", "SUPER_SECRET_PASS");
        while (WiFiMulti.run() != WL_CONNECTED) {
            Serial.print(". ");
                delay(100);
        }
        Serial.println("WiFi Connected");
        dht.begin();
        influxdb.opendb(DATABASE, DB_USER, DB_PASSWORD);
}

void loop() {

        float h = dht.readHumidity();
        float t = dht.readTemperature();
      
        if (isnan(h) || isnan(t)) {
          Serial.println("Failed to read from DHT sensor!");
          return;
        }
        float hic = dht.computeHeatIndex(t, h, false);
        
        String data = "dht22 t=" + String(t) + ",h=" + String(h) + ",hic=" + String(hic);
  
        Serial.println("Writing data to host " + String(INFLUXDB_HOST) + ":" +
                       INFLUXDB_PORT + "'s database=" + DATABASE);
        Serial.println(data);
        influxdb.write(data);
        Serial.println(influxdb.response() == DB_SUCCESS ? "HTTP write success"
                       : "Writing failed");

        delay(30000);
}
