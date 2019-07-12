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
#include <WiFiManager.h>          

#include <DoubleResetDetector.h>  //https://github.com/datacute/DoubleResetDetector
#include <ESPinfluxdb.h>          // InfluxDB wrapper
#include <DHT.h>                  // Library for DHT22 temperature sensor
#include <DHT_U.h>
#include <ArduinoJson.h>          // https://github.com/bblanchon/ArduinoJson

// time
#include <time.h>                 // time() ctime()
#include <sys/time.h>             // struct timeval
#include <coredecls.h>            // settimeofday_cb()
#include <simpleDSTadjust.h>      // DST settings
#include <Ticker.h>               // for checking when its time to update ntp again

#include "SSD1306Wire.h"          // OLED Display drivers for SSD1306 displays
#include "OLEDDisplayUi.h"
#include "Wire.h"
#include "ManagedWifiDhtInfluxDBImages.h"
#include "ManagedWifiDhtInfluxDBFonts.h"
#include "ManagedWifiDhtInfluxDB.h"

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

float humidity = 0.0;
float temperature = 0.0;
char FormattedTemperature[10];
char FormattedHumidity[10];

const boolean IS_METRIC = true;

// Display Settings
const int I2C_DISPLAY_ADDRESS = 0x3c;
const int SDA_PIN = D3;
const int SDC_PIN = D4;

// Initialize the oled display for address 0x3c
// sda-pin=D3 and sdc-pin=D4
SSD1306Wire     display(I2C_DISPLAY_ADDRESS, SDA_PIN, SDC_PIN);
OLEDDisplayUi   ui( &display );

// Adjust according to your language
const String WDAY_NAMES[] = {"SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"};
const String MONTH_NAMES[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};

// Update time from NTP server every 5 hours
#define NTP_UPDATE_INTERVAL_SEC 5*3600
#define TZ              1         // (utc+) TZ in hours
#define NTP_SERVERS "time.nist.gov", "time.windows.com", "de.pool.ntp.org"

//Central European Time (Frankfurt, Paris)
struct dstRule StartRule = {"CEST", Last, Sun, Mar, 2, 3600};     //Central European Summer Time
struct dstRule EndRule = {"CET", Last, Sun, Oct, 3, 0};       //Central European Standard Time

// Setup simpleDSTadjust Library rules
simpleDSTadjust dstAdjusted(StartRule, EndRule);
char *dstAbbrev;

Ticker ticker1;
int32_t tick;

// flag changed in the ticker function to start NTP Update
bool readyForNtpUpdate = false;

// flag changed in the ticker function every 1 minute
bool readyForDHTUpdate = false;

time_t now;

//declaring prototypes
void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawIndoor(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y);
void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state);

// Add frames
// this array keeps function pointers to all frames
// frames are the single views that slide from right to left
FrameCallback frames[] = { drawDateTime, drawIndoor };
int numberOfFrames = 2;

OverlayCallback overlays[] = { drawHeaderOverlay };
int numberOfOverlays = 1;

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

  // initialize NTP and display
  updateNTP(); // Init the NTP time
  printTime(0); // print initial time time now.

  tick = NTP_UPDATE_INTERVAL_SEC; // Init the NTP update countdown ticker
  ticker1.attach(1, secTicker); // Run a 1 second interval Ticker
  Serial.print("Next NTP Update: ");
  printTime(tick);

  now = dstAdjusted.time(&dstAbbrev);
  Serial.println("system time set in setup: ");
  Serial.println(ctime(&now));

  // wait 2 minutes until DHT is ready to read
  ticker1.attach(120, setReadyForDHTUpdate);

  // initialize display
  display.init();
  display.clear();
  display.display();

  //display.flipScreenVertically();
  display.setFont(ArialMT_Plain_10);
  display.setTextAlignment(TEXT_ALIGN_CENTER);
  display.setContrast(255);

  ui.setTargetFPS(30);
  ui.setTimePerFrame(10*1000); // Setup frame display time to 10 sec

  ui.setActiveSymbol(activeSymbole);
  ui.setInactiveSymbol(inactiveSymbole);

  // You can change this to
  // TOP, LEFT, BOTTOM, RIGHT
  ui.setIndicatorPosition(BOTTOM);

  // Defines where the first frame is located in the bar.
  ui.setIndicatorDirection(LEFT_RIGHT);

  // You can change the transition that is used
  // SLIDE_LEFT, SLIDE_RIGHT, SLIDE_TOP, SLIDE_DOWN
  ui.setFrameAnimation(SLIDE_LEFT);

  ui.setFrames(frames, numberOfFrames);

  ui.setOverlays(overlays, numberOfOverlays);

  // Inital UI takes care of initalising the display too.
  ui.init();

  Serial.println("");
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
    if(readyForNtpUpdate) {
      readyForNtpUpdate = false;
      printTime(0);
      updateNTP();
      Serial.print("\nUpdated time from NTP Server: ");
      printTime(0);
      Serial.print("Next NTP Update: ");
      printTime(tick);
    }

    if (readyForDHTUpdate && ui.getUiState()->frameState == FIXED) {
      ClimateMeasurement sensorData = getClimateMeasurement();
      influxdb.setHost(influxdb_server);
      influxdb.setPort(atoi(influxdb_port));
      influxdb.opendb(String(influxdb_db), String(influxdb_user), String(influxdb_password));
      
      if (isnan(sensorData.humidity) || isnan(sensorData.temperature)) {
        Serial.println("Failed to read from DHT sensor!");
        return;
      }
      
      String data = String(measurement) + ",node=" + String(node) + " t=" + String(sensorData.temperature) + ",h=" + String(sensorData.humidity) + ",hic=" + String(sensorData.heatIndex);
    
      Serial.println("Writing data to host " + String(influxdb_server) + ":" +
                    String(influxdb_port) + "'s database=" + String(influxdb_db));
      Serial.println(data);
      influxdb.write(data);
      Serial.println(influxdb.response() == DB_SUCCESS ? "HTTP write success"
                    : "Writing failed");

      // Wait again for DHT Sensor to become ready
      readyForDHTUpdate = false;
    }

    int remainingTimeBudget = ui.update();

    if (remainingTimeBudget > 0) {
      // You can do some work here
      // Don't do stuff if you are below your
      // time budget.
      delay(remainingTimeBudget);
    }
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

ClimateMeasurement getClimateMeasurement() {
  float h = dht.readHumidity();
  float t = dht.readTemperature();
  float hic = dht.computeHeatIndex(t, h, false);
  temperature = t;
  humidity = h;

  return {h, t, 0, hic};
}

// NTP timer update ticker
void secTicker()
{
  tick--;
  if(tick<=0)
   {
    readyForNtpUpdate = true;
    tick= NTP_UPDATE_INTERVAL_SEC; // Re-arm
   }

  // printTime(0);  // Uncomment if you want to see time printed every second
}

void updateNTP() {
  configTime(TZ * 3600, 0, NTP_SERVERS);

  delay(500);
  while (!time(nullptr)) {
    Serial.print("#");
    delay(1000);
  }
}


void printTime(time_t offset) {
  char buf[30];
  char *dstAbbrev;
  time_t t = dstAdjusted.time(&dstAbbrev)+offset;
  struct tm *timeinfo = localtime (&t);
  
  int hour = (timeinfo->tm_hour+11)%12+1;  // take care of noon and midnight
  sprintf(buf, "%02d/%02d/%04d %02d:%02d:%02d%s %s\n",timeinfo->tm_mon+1, timeinfo->tm_mday, timeinfo->tm_year+1900, hour, timeinfo->tm_min, timeinfo->tm_sec, timeinfo->tm_hour>=12?"pm":"am", dstAbbrev);
  Serial.print(buf);
}

void drawDateTime(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  now = dstAdjusted.time(&dstAbbrev);
  struct tm* timeInfo;
  timeInfo = localtime(&now);
  char buff[16];


  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  String date = WDAY_NAMES[timeInfo->tm_wday];

  sprintf_P(buff, PSTR("%s, %02d/%02d/%04d"), WDAY_NAMES[timeInfo->tm_wday].c_str(), timeInfo->tm_mday, timeInfo->tm_mon+1, timeInfo->tm_year + 1900);
  display->drawString(64 + x, 5 + y, String(buff));
  display->setFont(ArialMT_Plain_24);

  sprintf_P(buff, PSTR("%02d:%02d:%02d"), timeInfo->tm_hour, timeInfo->tm_min, timeInfo->tm_sec);
  display->drawString(64 + x, 15 + y, String(buff));
  display->setTextAlignment(TEXT_ALIGN_LEFT);
}

void drawIndoor(OLEDDisplay *display, OLEDDisplayUiState* state, int16_t x, int16_t y) {
  display->setTextAlignment(TEXT_ALIGN_CENTER);
  display->setFont(ArialMT_Plain_10);
  display->drawString(64 + x, 0, "DHT22 Indoor Sensor" );
  display->setFont(ArialMT_Plain_16);
  dtostrf(temperature,4, 1, FormattedTemperature);
  display->drawString(64+x, 12, "Temp: " + String(FormattedTemperature) + (IS_METRIC ? "°C": "°F"));
  dtostrf(humidity,4, 1, FormattedHumidity);
  display->drawString(64+x, 30, "Humidity: " + String(FormattedHumidity) + "%");
}

void drawHeaderOverlay(OLEDDisplay *display, OLEDDisplayUiState* state) {
  now = dstAdjusted.time(&dstAbbrev);
  struct tm* timeInfo;
  timeInfo = localtime(&now);
  char buff[14];
  sprintf_P(buff, PSTR("%02d:%02d"), timeInfo->tm_hour, timeInfo->tm_min);

  display->setColor(WHITE);
  display->setFont(ArialMT_Plain_10);
  display->setTextAlignment(TEXT_ALIGN_LEFT);
  display->drawString(0, 54, String(buff));
  display->setTextAlignment(TEXT_ALIGN_RIGHT);
  //String temp = String(currentWeather.temp, 1) + (IS_METRIC ? "°C" : "°F");
  //display->drawString(128, 54, temp);
  display->drawHorizontalLine(0, 52, 128);
}

void setReadyForDHTUpdate() {
  Serial.println("Setting readyForDHTUpdate to true");
  readyForDHTUpdate = true;
}
