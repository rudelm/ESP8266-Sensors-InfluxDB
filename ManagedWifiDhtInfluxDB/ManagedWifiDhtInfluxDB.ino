/*
code taken from https://github.com/tzapu/WiFiManager/blob/master/examples/AutoConnectWithFSParameters/AutoConnectWithFSParameters.ino
and from https://github.com/kentaylor/WiFiManager/blob/master/examples/ConfigOnDoubleReset/ConfigOnDoubleReset.ino
and from https://github.com/kentaylor/WiFiManager/blob/master/examples/ConfigOnSwitchFS/ConfigOnSwitchFS.ino

This sketch will open a configuration portal when the reset button is pressed twice. 
This method works well on Wemos boards which have a single reset button on board. It avoids using a pin for launching the configuration portal.
How It Works
When the ESP8266 loses power all data in RAM is lost but when it is reset the contents of a small region of RAM is preserved. So when the device starts up it checks this region of ram for a flag to see if it has been recently reset. If so it launches a configuration portal, if not it sets the reset flag. After running for a while this flag is cleared so that it will only launch the configuration portal in response to closely spaced resets.
Settings
There are two values to be set in the sketch.
DRD_TIMEOUT - Number of seconds to wait for the second reset. Set to 10 in the example.
DRD_ADDRESS - The address in RTC RAM to store the flag. This memory must not be used for other purposes in the same sketch. Set to 0 in the example.
This example, contributed by DataCute needs the Double Reset Detector library from https://github.com/datacute/DoubleResetDetector .
*/

#include <FS.h>                   // this needs to be first, or it all crashes and burns...
#include <ESP8266WiFi.h>          // ESP8266 Core WiFi Library (you most likely already have this in your sketch)

#include <DNSServer.h>            // Local DNS Server used for redirecting all requests to the configuration portal
#include <ESP8266WebServer.h>     // Local WebServer used to serve the configuration portal
#include <WiFiManager.h>          // https://github.com/tzapu/WiFiManager WiFi Configuration Magic

#include <DoubleResetDetector.h>  //https://github.com/datacute/DoubleResetDetector
#include <ESPinfluxdb.h>          // InfluxDB wrapper
#include <DHT.h>                  // Library for DHT22 temperature sensor
#include <DHT_U.h>
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson

#define DHTPIN 12                 // DHT connection on GPIO Pin 12 or D6 of NodeMCU LoLin V3
#define DHTTYPE DHT22             // DHT 22
#define DHT_READ_INTERVAL 60000   // Read temp info every 60s

// Number of seconds after reset during which a 
// subseqent reset will be considered a double reset.
#define DRD_TIMEOUT 10

// RTC Memory Address for the DoubleResetDetector to use
#define DRD_ADDRESS 0

// Constants
const char* CONFIG_FILE = "/config.json";

DoubleResetDetector drd(DRD_TIMEOUT, DRD_ADDRESS);
// Indicates whether ESP has WiFi credentials saved from previous session, or double reset detected
bool initialConfig = false;

//#define RESET_DATA              // comment this out to reset stored data and configuration

DHT dht(DHTPIN, DHTTYPE);
Influxdb influxdb;

// define your default values here, if there are different values in config.json, they are overwritten.
char influxdb_server[60];
char influxdb_port[6] = "8086";
char influxdb_db[30] = "default";
char influxdb_user[30];
char influxdb_password[30];
char measurement[30] = "climate";
char node[30];

// flag for saving data
bool shouldSaveConfig = false;

// Function Prototypes

bool readConfigFile();
bool writeConfigFile();

void setup() {
  Serial.begin(115200);
  Serial.println();

  dht.begin();

  if (drd.detectDoubleReset()) {
    Serial.println("Double Reset Detected");
    initialConfig = true;
  }

  if (WiFi.SSID()==""){
    Serial.println("We haven't got any access point credentials, so get them now");   
    initialConfig = true;
  } else {
    WiFi.mode(WIFI_STA); // Force to station mode because if device was switched off while in access point mode it will start up next time in access point mode.
    unsigned long startedAt = millis();
    Serial.print("After waiting ");
    int connRes = WiFi.waitForConnectResult();
    float waited = (millis()- startedAt);
    Serial.print(waited/1000);
    Serial.print(" secs in setup() connection result is ");
    Serial.println(connRes);
  }

  #if defined(RESET_DATA)
    // clean FS, for testing
    Serial.println("cleaning FS...");
    SPIFFS.format();
  #endif

  // initialize SPIFFS
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    //read configuration from FS json
    Serial.println("mounting FS...");
    if (SPIFFS.exists(CONFIG_FILE)) {
      //file exists, reading and loading
      Serial.println("reading config file");
      readConfigFile();
    } else {
      Serial.println("Configuration file did not exist, using defaults");
    }
    //end read
  } else {
    Serial.println("failed to mount FS");
  }
}
//end setup

void loop() {
  // Call the double reset detector loop method every so often,
  // so that it can recognise when the timeout expires.
  // You can also call drd.stop() when you wish to no longer
  // consider the next reset as a double reset.
  drd.loop();

  if (initialConfig) {
    // initialize the WifiManager portal
    // The extra parameters to be configured (can be either global or just in the setup)
    // After connecting, parameter.getValue() will get you the configured value
    // id/name placeholder/prompt default length
    WiFiManagerParameter custom_influxdb_server("server", "InfluxDB server", influxdb_server, 60);
    WiFiManagerParameter custom_influxdb_port("port", "InfluxDB server port", influxdb_port, 6);
    WiFiManagerParameter custom_influxdb_db("db", "InfluxDB database", influxdb_db, 30);
    WiFiManagerParameter custom_influxdb_user("user", "InfluxDB username", influxdb_user, 30);
    WiFiManagerParameter custom_influxdb_password("password", "InfluxDB userpassword", influxdb_password, 30);
    WiFiManagerParameter custom_measurement("measurement", "InfluxDB measurement", measurement, 30);
    WiFiManagerParameter custom_node("node", "Node name for better identification", node, 30);

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
    wifiManager.addParameter(&custom_node);

    #if defined(RESET_DATA)
      // reset settings - for testing
      wifiManager.resetSettings();
    #endif

    // set minimu quality of signal so it ignores AP's under that quality
    // defaults to 8%
    //wifiManager.setMinimumSignalQuality();

    // sets timeout until configuration portal gets turned off
    // useful to make it all retry or go to sleep
    // in seconds
    wifiManager.setConfigPortalTimeout(300);

    // fetches stored ssid and pass and tries to connect
    // if it does not connect it starts an access point with the specified name
    // here  "DHT22-Sensor"
    // and goes into a blocking loop awaiting configuration
    if (!wifiManager.startConfigPortal("DHT22-Sensor", "configureMe")) {
      Serial.println("failed to connect and hit timeout");
      delay(3000);
      // reset and try again, or maybe put it to deep sleep
      ESP.reset();
      delay(5000);
    } else {
      // if you get here you have connected to the WiFi
      Serial.println("connected to the selected WiFi...");
    }

    // read updated parameters
    strcpy(influxdb_server, custom_influxdb_server.getValue());
    strcpy(influxdb_port, custom_influxdb_port.getValue());
    strcpy(influxdb_db, custom_influxdb_db.getValue());
    strcpy(influxdb_user, custom_influxdb_user.getValue());
    strcpy(influxdb_password, custom_influxdb_password.getValue());
    strcpy(measurement, custom_measurement.getValue());
    strcpy(node, custom_node.getValue());

    // save the custom parameters to FS
    if (shouldSaveConfig) {
      writeConfigFile();
    }

    if (WiFi.status()!=WL_CONNECTED) {
      Serial.println("failed to connect, finishing setup anyway");
    } else {
      Serial.print("local ip: ");
      Serial.println(WiFi.localIP());
    }

    // reset config mode for next loop iteration
    initialConfig = false;
  } else {
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
    
    String data = String(measurement) + ",node=" + String(node) + " t=" + String(t) + ",h=" + String(h) + ",hic=" + String(hic);

    Serial.println("Writing data to host " + String(influxdb_server) + ":" +
                  String(influxdb_port) + "'s database=" + String(influxdb_db));
    Serial.println(data);
    influxdb.write(data);
    Serial.println(influxdb.response() == DB_SUCCESS ? "HTTP write success"
                  : "Writing failed");

    delay(DHT_READ_INTERVAL);
  }
}

// callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}

bool readConfigFile() {
  // this opens the config file in read-mode
  File f = SPIFFS.open(CONFIG_FILE, "r");
  
  if (!f) {
    Serial.println("Configuration file not found");
    return false;
  } else {
    File configFile = SPIFFS.open(CONFIG_FILE, "r");
    if (configFile) {
      Serial.println("opened config file");
      size_t size = configFile.size();
      // Allocate a buffer to store contents of the file.
      std::unique_ptr<char[]> buf(new char[size]);

      configFile.readBytes(buf.get(), size);
      configFile.close();

      // see https://arduinojson.org/v6/assistant/
      // we use a json hash with 6 keys, the JSON_OBJECT_SIZE macro takes the number of keys
      const size_t capacity = JSON_OBJECT_SIZE(6) + 255;
      DynamicJsonDocument json(capacity);
      DeserializationError err = deserializeJson(json, buf.get());
      if (err) {
        Serial.print("deserialization of JSON failed with code ");
        Serial.println(err.c_str());
        return false;
      } else {
        Serial.println("\nparsed json");
        serializeJsonPretty(json, Serial);

        if (json.containsKey("influxdb_server")) {
          strcpy(influxdb_server, json["influxdb_server"]);
        }
        if (json.containsKey("influxdb_port")) {
          strcpy(influxdb_port, json["influxdb_port"]);
        }
        if (json.containsKey("influxdb_db")) {
          strcpy(influxdb_db, json["influxdb_db"]);
        }
        if (json.containsKey("influxdb_user")) {
          strcpy(influxdb_user, json["influxdb_user"]);
        }
        if (json.containsKey("influxdb_password")) {
          strcpy(influxdb_password, json["influxdb_password"]);
        }
        if (json.containsKey("measurement")) {
          strcpy(measurement, json["measurement"]);
        }
        if (json.containsKey("node")) {
          strcpy(node, json["node"]);
        }
        Serial.println("\nConfig file was successfully parsed");
        return true;
      }
    } else {
      Serial.println("\nCould not open config file for reading");
      return false;
    }
  }
}

bool writeConfigFile() {
  Serial.println("Saving config file");

  const size_t capacity = JSON_OBJECT_SIZE(6) + 255;
  DynamicJsonDocument json(capacity);

  json["influxdb_server"] = influxdb_server;
  json["influxdb_port"] = influxdb_port;
  json["influxdb_db"] = influxdb_db;
  json["influxdb_user"] = influxdb_user;
  json["influxdb_password"] = influxdb_password;
  json["measurement"] = measurement;
  json["node"] = node;

  File configFile = SPIFFS.open(CONFIG_FILE, "w");
  if (!configFile) {
    Serial.println("Could not open config file for writing");
    return false;
  }

  serializeJsonPretty(json, Serial);
  serializeJson(json, configFile);
  configFile.close();

  Serial.println("\nConfig file was successfully saved");
  return true;
}
