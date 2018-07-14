// code taken from https://github.com/tzapu/WiFiManager/blob/master/examples/AutoConnectWithFSParameters/AutoConnectWithFSParameters.ino

#include <FS.h>                   // this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>          // ESP8266 Core WiFi Library (you most likely already have this in your sketch)

#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager WiFi Configuration Magic

#include <ESPinfluxdb.h>          // InfluxDB wrapper
#include <DHT.h>                  // Library for DHT22 temperature sensor
#include <DHT_U.h>
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson

#define DHTPIN 12                 // DHT connection on GPIO Pin 12 or D6 of NodeMCU LoLin V3
#define DHTTYPE DHT22             // DHT 22

DHT dht(DHTPIN, DHTTYPE);
Influxdb influxdb;

// define your default values here, if there are different values in config.json, they are overwritten.
char influxdb_server[60];
char influxdb_port[6] = "8086";
char influxdb_db[30] = "default";
char influxdb_user[30];
char influxdb_password[30];
char measurement[30] = "climate";

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

void setup() {
  Serial.begin(115200);
  Serial.println();

  dht.begin();

  //clean FS, for testing
  // SPIFFS.format();

  //read configuration from FS json
  Serial.println("mounting FS...");

  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");

          strcpy(influxdb_server, json["influxdb_server"]);
          strcpy(influxdb_port, json["influxdb_port"]);
          strcpy(influxdb_db, json["influxdb_db"]);
          strcpy(influxdb_user, json["influxdb_user"]);
          strcpy(influxdb_password, json["influxdb_password"]);
          strcpy(measurement, json["measurement"]);

        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  //end read

  // The extra parameters to be configured (can be either global or just in the setup)
  // After connecting, parameter.getValue() will get you the configured value
  // id/name placeholder/prompt default length
  WiFiManagerParameter custom_influxdb_server("server", "InfluxDB server", influxdb_server, 60);
  WiFiManagerParameter custom_influxdb_port("port", "InfluxDB server port", influxdb_port, 6);
  WiFiManagerParameter custom_influxdb_db("db", "InfluxDB database", influxdb_db, 30);
  WiFiManagerParameter custom_influxdb_user("user", "InfluxDB username", influxdb_user, 30);
  WiFiManagerParameter custom_influxdb_password("password", "InfluxDB userpassword", influxdb_password, 30);
  WiFiManagerParameter custom_measurement("measurement", "InfluxDB measurement", measurement, 30);

  // WiFiManager
  // Local intialization. Once its business is done, there is no need to keep it around
  WiFiManager wifiManager;

  // set config save notify callback
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  // set static ip
  //wifiManager.setSTAStaticIPConfig(IPAddress(10,0,1,99), IPAddress(10,0,1,1), IPAddress(255,255,255,0));

  // add all your parameters here
  wifiManager.addParameter(&custom_influxdb_server);
  wifiManager.addParameter(&custom_influxdb_port);
  wifiManager.addParameter(&custom_influxdb_db);
  wifiManager.addParameter(&custom_influxdb_user);
  wifiManager.addParameter(&custom_influxdb_password);
  wifiManager.addParameter(&custom_measurement);

  // reset settings - for testing
  // wifiManager.resetSettings();

  // set minimu quality of signal so it ignores AP's under that quality
  // defaults to 8%
  //wifiManager.setMinimumSignalQuality();

  // sets timeout until configuration portal gets turned off
  // useful to make it all retry or go to sleep
  // in seconds
  wifiManager.setTimeout(300);

  // fetches ssid and pass and tries to connect
  // if it does not connect it starts an access point with the specified name
  // here  "DHT22-Sensor"
  // and goes into a blocking loop awaiting configuration
  if (!wifiManager.autoConnect("DHT22-Sensor", "configureMe")) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    // reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  // if you get here you have connected to the WiFi
  Serial.println("connected to the selected WiFi...");

  // read updated parameters
  strcpy(influxdb_server, custom_influxdb_server.getValue());
  strcpy(influxdb_port, custom_influxdb_port.getValue());
  strcpy(influxdb_db, custom_influxdb_db.getValue());
  strcpy(influxdb_user, custom_influxdb_user.getValue());
  strcpy(influxdb_password, custom_influxdb_password.getValue());
  strcpy(measurement, custom_measurement.getValue());

  // save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["influxdb_server"] = influxdb_server;
    json["influxdb_port"] = influxdb_port;
    json["influxdb_db"] = influxdb_db;
    json["influxdb_user"] = influxdb_user;
    json["influxdb_password"] = influxdb_password;
    json["measurement"] = measurement;
    
    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  Serial.println("local ip");
  Serial.println(WiFi.localIP());
}

void loop() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();

  influxdb.setHost(influxdb_server);
  influxdb.setPort(atoi(influxdb_port));
  influxdb.opendb(String(influxdb_db), String(influxdb_user), String(influxdb_password));
  
  if (isnan(h) || isnan(t)) {
    Serial.println("Failed to read from DHT sensor!");
    return;
  }
  float hic = dht.computeHeatIndex(t, h, false);
  
  String data = String(measurement) + " t=" + String(t) + ",h=" + String(h) + ",hic=" + String(hic);

  Serial.println("Writing data to host " + String(influxdb_server) + ":" +
                 String(influxdb_port) + "'s database=" + String(influxdb_db));
  Serial.println(data);
  influxdb.write(data);
  Serial.println(influxdb.response() == DB_SUCCESS ? "HTTP write success"
                 : "Writing failed");

  delay(30000);
}
