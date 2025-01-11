#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <AccelStepper.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>
#include <Update.h>

// BLE and Preferences
Preferences preferences;
BLEServer *pServer = NULL;
BLECharacteristic *pTxCharacteristic;
bool deviceConnected = false;
bool oldDeviceConnected = false;


// Motor pins
#define motorInterfaceType 1  // Stepper motor driver type
#define stepPin 10
#define dirPin 12
#define enablePin 11
#define stepPin2 13
#define dirPin2 14
#define enablePin2 21
#define stepPin3 6
#define dirPin3 5
#define enablePin3 4

// Motor parameters
const long Speed = 8000;
const long Acceleration = 5000;
const long MotorStepsPerRev = 600;
const float Microstepping = 4.0;
const float GearRatio = 4.167;
const long Range = MotorStepsPerRev * Microstepping * GearRatio;

// Global Variables
float float_w = 0.0, float_s = 0.0, float_v = 0.0;
String data = "";
String Ssid = "Surfnet";
String Password = "surf4life";
String tide_station = "46239";
String current_station = "9413450";
String tide_base = "https://www.ndbc.noaa.gov/data/5day2/";
String tide_end = "_5day.txt";
String current_base = "https://api.tidesandcurrents.noaa.gov/api/prod/datagetter?date=latest&station=";
String current_end = "&product=water_level&datum=MLLW&time_zone=gmt&units=english&application=DataAPI_Sample&format=xml";
bool updateRequired = false;

// Periodic Update Variables
const unsigned long updateInterval = 60000; // 60 seconds in milliseconds
unsigned long lastUpdateTime = -updateInterval; // Track the last update time, set to trigger immediate update

// Motor objects
AccelStepper stepper(motorInterfaceType, stepPin, dirPin);
AccelStepper stepper2(motorInterfaceType, stepPin2, dirPin2);
AccelStepper stepper3(motorInterfaceType, stepPin3, dirPin3);

// BLE UUIDs
#define SERVICE_UUID "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_RX "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
#define CHARACTERISTIC_UUID_TX "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"

class MyServerCallbacks : public BLEServerCallbacks {
    void onConnect(BLEServer *pServer) override {
        deviceConnected = true;
        Serial.println("BLE client connected.");
    }

    void onDisconnect(BLEServer *pServer) override {
        deviceConnected = false;
        Serial.println("BLE client disconnected. Restarting advertising...");
        pServer->startAdvertising(); // Restart advertising after disconnection
    }
};

class MyCallbacks : public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) override {
        String rxValue = pCharacteristic->getValue();
        Serial.println("Received BLE Data: " + rxValue);

        // Handle OTA trigger command
        if (rxValue.startsWith("OTA,")) {
            // Extract the size of the firmware from the received string
            String sizeStr = rxValue.substring(4);  // Get everything after "OTA,"
            unsigned long firmwareSize = sizeStr.toInt();  // Convert it to an integer

            if (firmwareSize > 0) {
                Serial.print("Triggering OTA update. Size: ");
                Serial.println(firmwareSize);

                // Start OTA process
                startOTAUpdate(firmwareSize);
            } else {
                Serial.println("Invalid firmware size received.");
            }
        }
        // Handle WiFi credentials update
        else if (rxValue.length() > 0) {
            int firstComma = rxValue.indexOf(',');
            int secondComma = rxValue.indexOf(',', firstComma + 1);
            int thirdComma = rxValue.indexOf(',', secondComma + 1);

            Ssid = rxValue.substring(0, firstComma);
            Password = rxValue.substring(firstComma + 1, secondComma);
            tide_station = rxValue.substring(secondComma + 1, thirdComma);
            current_station = rxValue.substring(thirdComma + 1);
            current_station.trim();

            Serial.println("Received BLE Data:");
            Serial.println("SSID: " + Ssid);
            Serial.println("Password: " + Password);
            Serial.println("TStation: " + tide_station);
            Serial.println("CStation: " + current_station);

            preferences.putString("ssid", Ssid);
            preferences.putString("password", Password);
            preferences.putString("tide_station", tide_station);
            preferences.putString("current_station", current_station);

            updateRequired = true;
        }
    }
};

void startOTAUpdate(unsigned long firmwareSize) {
    // Use HTTPClient to download the firmware binary and start the update
    if (WiFi.status() == WL_CONNECTED) {
        HTTPClient http;
        String firmwareURL = "http://your-server.com/firmware.bin"; // URL to your firmware file

        http.begin(firmwareURL);  // Specify the URL
        int httpCode = http.GET(); // Make the request

        if (httpCode == HTTP_CODE_OK) {
            WiFiClient *client = http.getStreamPtr();

            // Start the OTA update
            Update.begin(firmwareSize);  // Indicate the update size
            size_t written = Update.writeStream(*client);  // Write the incoming data to flash memory

            if (written == firmwareSize) {
                if (Update.end()) {
                    Serial.println("Update successfully completed. Rebooting...");
                    ESP.restart();  // Restart the ESP32 to apply the new firmware
                } else {
                    Serial.println("Update failed.");
                }
            } else {
                Serial.println("Failed to write update.");
            }
        } else {
            Serial.print("Failed to get update: ");
            Serial.println(httpCode);
        }
        http.end();
    } else {
        Serial.println("WiFi not connected for OTA update.");
    }
}

void moveStepperTo(AccelStepper &stepper, int target) {
    stepper.moveTo(target);
    while (stepper.distanceToGo() != 0) {
        stepper.run();
    }
}

void zeroMotors() {
    Serial.println("Zeroing motors...");

    moveStepperTo(stepper, -Range);
    moveStepperTo(stepper2, -Range);
    moveStepperTo(stepper3, -Range);

    stepper.setCurrentPosition(0);
    stepper2.setCurrentPosition(0);
    stepper3.setCurrentPosition(0);

    Serial.println("Motors zeroed successfully.");
}

void fetchData() {
    Serial.println("Fetching data...");

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient https;

    String tide_link = tide_base + tide_station + tide_end;
    String current_link = current_base + current_station + current_end;

    Serial.println("Tide Link: " + tide_link);
    Serial.println("Current Link: " + current_link);

    // Fetch wave data
    if (https.begin(client, tide_link)) {
        int httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
            Serial.println("Wave Data Retrieved. Parsing...");
            WiFiClient *stream = https.getStreamPtr();
            int lineCount = 0;

            float_w = 0.0;
            float_s = 0.0;

            while (stream->connected() && stream->available()) {
                String line = stream->readStringUntil('\n'); // Correct syntax for delimiter
                line.trim();  // Remove leading/trailing whitespace

                // Skip empty lines and headers
                if (line.isEmpty() || line.startsWith("#")) continue;

                Serial.print("Line ");
                Serial.print(lineCount + 1);
                Serial.print(": ");
                Serial.println(line);

                // Split line into tokens
                String tokens[20];
                int colIndex = 0;

                char *cLine = const_cast<char *>(line.c_str());
                char *token = strtok(cLine, " \t");
                while (token != NULL) {
                    tokens[colIndex++] = String(token);
                    token = strtok(NULL, " \t");
                }

                // Ensure there are enough columns
                if (colIndex < 10) continue;

                // Extract WVHT (9th column) and DPD (10th column)
                String WVHT = tokens[8];
                String DPD  = tokens[9];

                if (WVHT != "MM" && DPD != "MM") {
                    float_w = WVHT.toFloat();
                    float_s = DPD.toFloat();
                    Serial.println("Valid Data Found:");
                    Serial.print("Wave Height: ");
                    Serial.println(float_w);
                    Serial.print("Wave Period: ");
                    Serial.println(float_s);
                    break;
                }
            }

            if (float_w == 0.0 || float_s == 0.0) {
                Serial.println("No valid wave data found.");
            }
        } else {
            Serial.print("Wave Data Error: ");
            Serial.println(https.errorToString(httpCode));
        }
        https.end();
    } else {
        Serial.println("Failed to connect to Wave Data URL.");
    }

    // Fetch tide data
    if (https.begin(client, current_link)) {
        int httpCode = https.GET();
        if (httpCode == HTTP_CODE_OK) {
            String payload = https.getString();
            int startIndex = payload.indexOf("v=\"") + 3;
            int endIndex = payload.indexOf("\"", startIndex);
            String tideLevelStr = payload.substring(startIndex, endIndex);
            float_v = tideLevelStr.toFloat();
            Serial.println("Tide Data Retrieved:");
            Serial.print("Tide Level: ");
            Serial.println(float_v);
        } else {
            Serial.print("Tide Data Error: ");
            Serial.println(https.errorToString(httpCode));
        }
        https.end();
    } else {
        Serial.println("Failed to connect to Tide Data URL.");
    }
}

void set_status_motor() {
    Serial.println("Updating motors...");

    moveStepperTo(stepper, 0);
    moveStepperTo(stepper2, 0);
    moveStepperTo(stepper3, 0);

    int wave_height = float_w / 22 * Range * 3.281;
    int wave_period = float_s / 22 * Range;
    int tide = 0;

    if (float_v < 0)
        tide = Range - (float_v / 6 * -Range / 2);
    else
        tide = (float_v / 6 * Range / 2);

    moveStepperTo(stepper, wave_height);
    moveStepperTo(stepper2, wave_period);
    moveStepperTo(stepper3, tide);

    Serial.println("Motors updated.");
}

void initializeWiFi() {
    Serial.println("Initializing WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(Ssid.c_str(), Password.c_str());

    for (int i = 0; i < 10 && WiFi.status() != WL_CONNECTED; i++) {
        delay(1000);
        Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi Connected!");
        Serial.print("IP Address: ");
        Serial.println(WiFi.localIP());
    } else {
        Serial.println("\nWiFi Connection Failed!");
    }
}

void setup() {
    Serial.begin(115200);

    preferences.begin("credentials", false);
    Ssid = preferences.getString("ssid", Ssid);
    Password = preferences.getString("password", Password);
    tide_station = preferences.getString("tide_station", tide_station);
    current_station = preferences.getString("current_station", current_station);

    // Initialize WiFi
    initializeWiFi();

    // Initialize motor enable pins
    pinMode(enablePin, OUTPUT);
    pinMode(enablePin2, OUTPUT);
    pinMode(enablePin3, OUTPUT);
    digitalWrite(enablePin, LOW);  // Enable motor 1
    digitalWrite(enablePin2, LOW); // Enable motor 2
    digitalWrite(enablePin3, LOW); // Enable motor 3

    // Configure stepper motors
    stepper.setMaxSpeed(Speed);
    stepper.setAcceleration(Acceleration);
    stepper.setPinsInverted(true, false, false); // Reverse direction for stepper 1

    stepper2.setMaxSpeed(Speed);
    stepper2.setAcceleration(Acceleration);
    stepper2.setPinsInverted(true, false, false); // Reverse direction for stepper 2

    stepper3.setMaxSpeed(Speed);
    stepper3.setAcceleration(Acceleration);
    stepper3.setPinsInverted(true, false, false); // Reverse direction for stepper 3

    zeroMotors();

    BLEDevice::init("Wave Clock");
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());
    BLEService *pService = pServer->createService(SERVICE_UUID);

    pTxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_TX, BLECharacteristic::PROPERTY_NOTIFY);
    pTxCharacteristic->addDescriptor(new BLE2902());
    BLECharacteristic *pRxCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID_RX, BLECharacteristic::PROPERTY_WRITE);
    pRxCharacteristic->setCallbacks(new MyCallbacks());

    pService->start();
    pServer->getAdvertising()->start(); // Start advertising initially

    Serial.println("OTA BLE Initialized and advertising started.");
}

void loop() {
    // Check if an update is required
    if (updateRequired) {
        updateRequired = false; // Reset the flag

        initializeWiFi(); // Reinitialize WiFi with new credentials

        if (WiFi.status() == WL_CONNECTED) {
            fetchData();
            set_status_motor();
        } else {
            Serial.println("Failed to reconnect to WiFi.");
        }
    }

    // Check if it's time for an automatic update
    if (millis() - lastUpdateTime >= updateInterval) {
        lastUpdateTime = millis(); // Reset the timer

        Serial.println("Automatic update triggered...");
        if (WiFi.status() == WL_CONNECTED) {
            fetchData();
            set_status_motor();
        } else {
            Serial.println("WiFi not connected for automatic update.");
        }
    }

    delay(100); // Allow other tasks to run
}
