/**
 * A weather forecast display system
 * using the Elecrow CrowPanel ESP32 E-Paper HMI 5.79-inch Display.
 *
 * Two display modes (toggled by pressing the side button):
 *   - HOURLY : current conditions + 4 forecasts at +3 h, +6 h, +9 h, +12 h
 *              (wakes at the top of each hour)
 *   - DAILY  : 5-day forecast (today + 4 following days)
 *              (wakes every INTERVAL_IN_MINUTES minutes)
 *
 * The weather forecast data is obtained using the OpenWeatherMap API.
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include "EPD.h"
#include "icons.h"
#include "config.h"
#include "../test/testdata.h" // data for offline test
#include <map>

//=============================================================================
// RTC-retained State  (survives deep sleep)
//=============================================================================

/**
 * Display mode enumeration
 * Stored in RTC RAM so it persists across deep-sleep cycles.
 */
enum DisplayMode : uint8_t {
  MODE_HOURLY = 0,  // Current + 3-hour-step hourly forecast
  MODE_DAILY  = 1   // 5-day daily forecast
};

RTC_DATA_ATTR DisplayMode currentMode = MODE_HOURLY;  ///< Active display mode

//=============================================================================
// Hardware Configuration
//=============================================================================

/** GPIO pin connected to the side button (active-low, use INPUT_PULLUP). */
const int SIDE_BUTTON_PIN = 2;  // Adjust to the actual GPIO on your board

//=============================================================================
// Constants
//=============================================================================

// Test Settings
const bool TEST_MODE = false;      // Test mode (uses test data instead of the API when set to true)

// Display Settings
const size_t FORECAST_COUNT = 5;   // Number of forecast periods to display

// E-Paper Settings
const int EPD_BUFFER_SIZE = 27200; // Size of E-Paper display buffer

//=============================================================================
// Type Definitions
//=============================================================================

/**
 * Weather icon enumeration
 * Maps to indices in the Weather_Num array in icons.h
 */
enum WeatherIconNumber {
  ICON_CLEAR_DAY = 0,
  ICON_CLEAR_NIGHT = 1,
  ICON_CLOUDS = 2,
  ICON_RAIN = 3,
  ICON_THUNDERSTORM = 4,
  ICON_SNOW = 5,
  ICON_MIST = 6
};

/**
 * Forecast information structure
 * Stores weather forecast data for a specific time period.
 * The `label` field holds "HH:MM" in hourly mode and a weekday name in daily mode.
 */
struct ForecastInfo {
  String label;       // Time (HH:MM) in hourly mode, weekday name in daily mode
  int iconNumber;     // Weather icon number
  float temperature;  // Temperature (C or F depending on TEMPERATURE_UNIT)
  float pop;          // Probability of precipitation (%)
};

//=============================================================================
// Global Constants
//=============================================================================

/**
 * Weather mapping table
 * Maps OpenWeatherMap icon codes to our icon enumeration
 */
std::map<String, WeatherIconNumber> WEATHER_MAPPINGS = {
  {"01d", ICON_CLEAR_DAY},
  {"01n", ICON_CLEAR_NIGHT},
  {"02d", ICON_CLOUDS},
  {"02n", ICON_CLOUDS},
  {"03d", ICON_CLOUDS},
  {"03n", ICON_CLOUDS},
  {"04d", ICON_CLOUDS},
  {"04n", ICON_CLOUDS},
  {"09d", ICON_RAIN},
  {"09n", ICON_RAIN},
  {"10d", ICON_RAIN},
  {"10n", ICON_RAIN},
  {"11d", ICON_THUNDERSTORM},
  {"11n", ICON_THUNDERSTORM},
  {"13d", ICON_SNOW},
  {"13n", ICON_SNOW},
  {"50d", ICON_MIST},
  {"50n", ICON_MIST}
};

//=============================================================================
// Global Variables
//=============================================================================

// E-Paper Display Buffer
uint8_t ImageBW[EPD_BUFFER_SIZE];

// API Related Variables
String jsonBuffer;
int httpResponseCode = 0;

// Array to Store Forecast Data
ForecastInfo forecasts[FORECAST_COUNT];


//=============================================================================
// Deep-sleep Functions
//=============================================================================

/**
 * Returns the number of microseconds until the top of the next full hour.
 * Used in hourly mode so the display refreshes right at the hour mark.
 */
uint64_t microsecondsUntilNextHour() {
  // Use getLocalTime instead of time() — on ESP32, time() can remain 0
  // even after SNTP has synced, while getLocalTime always reflects the
  // SNTP-updated clock. configTime(0,0,...) gives us UTC; apply the
  // timezone offset manually to get local min/sec.
  struct tm utcTm;
  if (!getLocalTime(&utcTm, 5000)) {
    Serial.println("microsecondsUntilNextHour: clock not ready, defaulting to 60 min");
    return 3600ULL * 1000000ULL;
  }

  // Reconstruct a time_t from the UTC struct, add offset, decompose again
  time_t utcNow = mktime(&utcTm);
  time_t localNow = utcNow + (time_t)(TIMEZONE_OFFSET * 3600);
  struct tm *lt = gmtime(&localNow);

  int secsElapsed = lt->tm_min * 60 + lt->tm_sec;
  int secsUntilHour = 3600 - secsElapsed;
  if (secsUntilHour <= 0) secsUntilHour = 3600;

  Serial.print("Local time: ");
  Serial.print(lt->tm_hour); Serial.print(":"); Serial.println(lt->tm_min);
  Serial.print("Seconds until next hour: "); Serial.println(secsUntilHour);
  return (uint64_t)secsUntilHour * 1000000ULL;
}

/**
 * Function to Enter Deep-Sleep Mode
 *
 * Always enables EXT0 wakeup on SIDE_BUTTON_PIN (active-low) so the user
 * can switch display modes by pressing the side button at any time.
 *
 * In hourly mode the timer wakes the board at the top of the next hour.
 * In daily mode the timer uses the configured INTERVAL_IN_MINUTES.
 *
 * @param wakeup true if a timer wakeup should also be scheduled
 */
void enterDeepSleep(bool wakeup) {
  Serial.println("Entering Deep-sleep mode.");
  Serial.flush();

  // Put EPD in Sleep Mode Before Entering Deep-Sleep Mode
  EPD_DeepSleep();

  delay(4000);

  // Always enable wakeup on the side button (falling edge = button press)
  esp_sleep_enable_ext0_wakeup((gpio_num_t)SIDE_BUTTON_PIN, 0);

  if (wakeup) {
    uint64_t sleepUs;
//    if (currentMode == MODE_HOURLY) {
      sleepUs = microsecondsUntilNextHour();
      Serial.print("Hourly mode: sleeping for ");
      Serial.print((unsigned long)(sleepUs / 1000000ULL));
      Serial.println(" seconds until next hour.");
//    } else {
//      sleepUs = INTERVAL_IN_MINUTES * 60UL * 1000UL * 1000ULL;
//      Serial.print("Daily mode: sleeping for ");
//      Serial.print(INTERVAL_IN_MINUTES);
//      Serial.println(" minutes.");
//    }
    esp_sleep_enable_timer_wakeup(sleepUs);
  }

  esp_deep_sleep_start();
}

//=============================================================================
// Display Functions
//=============================================================================

/**
 * Displays weather forecast on the E-Paper display.
 *
 * In hourly mode the top row shows "HH:MM"; in daily mode it shows the
 * weekday name (e.g. "Mon").  A small mode label is drawn in the first
 * column so the user can see which view is active.
 *
 * Renders icon, temperature and probability of precipitation for each
 * forecast period in a column layout.
 */
void displayWeatherForecast()
{
  const int textBufferSize = 40;    // Size of text buffer for formatting
  const int columnWidth = 158;      // Width of each forecast column in pixels
  char buffer[textBufferSize];

  // Initialize Display
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();

  // Display Each Forecast Data
  for (int i = 0; i < FORECAST_COUNT; i++) {
    if (forecasts[i].label.length() > 0) {
      // Calculate x position for this column
      int baseX = columnWidth * i;

      // Display label (time or day name)
      memset(buffer, 0, sizeof(buffer));
      snprintf(buffer, sizeof(buffer), "%s", forecasts[i].label.c_str());
      EPD_ShowString(26 + baseX, 18, buffer, 44, BLACK);

      // Display Weather Icon
      EPD_ShowPicture(16 + baseX, 60, 128, 128, Weather_Num[forecasts[i].iconNumber], WHITE);

      // Display Temperature with appropriate unit
      memset(buffer, 0, sizeof(buffer));
      if (TEMPERATURE_UNIT == 0) {
        snprintf(buffer, sizeof(buffer), "%3d C", (int)round(forecasts[i].temperature));
      } else {
        snprintf(buffer, sizeof(buffer), "%3d F", (int)round(forecasts[i].temperature));
      }
      EPD_ShowString(30 + baseX, 190, buffer, 36, BLACK);
      EPD_DrawCircle(100 + baseX, 201, 2, BLACK, false);
      EPD_DrawCircle(100 + baseX, 201, 3, BLACK, false);

      if (i == 0) {
        // First column: draw a small mode indicator where the pop line would be
        char micro[64];
        snprintf(micro, sizeof(micro), "%u", microsecondsUntilNextHour()/1000000);

        EPD_ShowString(30 + baseX, 225,
          micro, //currentMode == MODE_HOURLY ? "HOURLY" : "DAILY",
          24, BLACK);
      } else {
        // Display Probability of precipitation
        memset(buffer, 0, sizeof(buffer));
        snprintf(buffer, sizeof(buffer), "%3d %%", (int)round(100 * forecasts[i].pop));
        EPD_ShowString(30 + baseX, 225, buffer, 36, BLACK);
      }
    }
  }

  // Draw Separator Lines
  for (int i = 1; i < FORECAST_COUNT; i++) {
    EPD_DrawLine(2 + columnWidth * i, 0, 2 + columnWidth * i, 271, BLACK);
  }

  // Update Display
  EPD_Display(ImageBW);
  EPD_PartUpdate();

  Serial.print("Weather forecast displayed successfully (mode: ");
  Serial.println(currentMode == MODE_HOURLY ? "HOURLY)" : "DAILY)");
}

/**
 * Displays an error message on the E-Paper display
 * 
 * @param message Error message to display
 */
void displayErrorMessage(const char* message) {
  Serial.print("ERROR: ");
  Serial.println(message);

  // Initialize Display
  Paint_NewImage(ImageBW, EPD_W, EPD_H, Rotation, WHITE);
  Paint_Clear(WHITE);
  EPD_FastMode1Init();
  EPD_Display_Clear();
  EPD_Update();
  EPD_Clear_R26A6H();

  // Display Error Message
  EPD_ShowString(30, 30, "ERROR:", 24, BLACK);
  
  // Process to add line breaks every 56 characters
  const int maxCharsPerLine = 56;
  const int lineHeight = 30;
  int currentLine = 0;
  int messageLength = strlen(message);
  int startPos = 0;
  
  while (startPos < messageLength) {
    char lineBuffer[maxCharsPerLine + 1];
    int charsToDisplay = messageLength - startPos;
    
    // Determine the number of characters to display in this line (maximum 56)
    if (charsToDisplay > maxCharsPerLine) {
      charsToDisplay = maxCharsPerLine;
    }
    
    // Copy the line text to buffer
    strncpy(lineBuffer, message + startPos, charsToDisplay);
    lineBuffer[charsToDisplay] = '\0'; // Add null terminator
    
    // Display the current line
    EPD_ShowString(60, 70 + (lineHeight * currentLine), lineBuffer, 24, BLACK);
    
    // Prepare for the next line
    startPos += charsToDisplay;
    currentLine++;
  }

  // Update Display
  EPD_Display(ImageBW);
  EPD_PartUpdate();
}

//=============================================================================
// Network Functions
//=============================================================================

/**
 * Connects to WiFi network using credentials from config.h
 * 
 * @return true if connection successful, false if failed
 */
bool connectToWiFi() {
  Serial.print("Connecting to WiFi network: ");
  Serial.println(WIFI_SSID);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  
  // Connection Timeout (10 seconds)
  const int wifiTimeoutMs = 10000;
  const int delayMs = 500;
  int attempts = wifiTimeoutMs / delayMs;
  
  // Wait until Connection is Complete
  while (WiFi.status() != WL_CONNECTED && attempts > 0) {
    delay(delayMs);
    Serial.print(".");
    attempts--;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("");
    Serial.print("Connected to WiFi with IP Address: ");
    Serial.println(WiFi.localIP());

    // Synchronize system clock via NTP.
    // Use UTC (offset=0) so time() returns a clean Unix timestamp.
    // Timezone offset is applied manually wherever local time is needed.
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    Serial.print("Waiting for NTP sync");
    struct tm ntpTime;
    bool synced = false;
    for (int i = 0; i < 40; i++) {   // up to 20 seconds
      delay(500);
      Serial.print(".");
      if (getLocalTime(&ntpTime, 0) && ntpTime.tm_year > 100) {
        synced = true;
        break;
      }
    }
    Serial.println(synced ? " synced." : " timed out!");
    if (synced) {
      Serial.print("UTC time: ");
      Serial.print(ntpTime.tm_hour); Serial.print(":");
      Serial.println(ntpTime.tm_min);
    }

    return true;
  } else {
    Serial.println("");
    Serial.println("Failed to connect to WiFi");
    return false;
  }
}

/**
 * Disconnects from WiFi to save power
 */
void disconnectWiFi() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi disconnected for power saving");
}

/**
 * Sends an HTTP GET request to the specified URL
 * 
 * @param url URL to send the request to
 * @return Response content as String
 */
String httpGETRequest(const char* url) {
  WiFiClient client;
  HTTPClient http;

  // Initialize HTTP Client and Specify Server URL
  http.begin(client, url);

  // Set HTTP Request Timeout
  const int httpTimeoutMs = 20000; // HTTP request timeout in milliseconds
  http.setTimeout(httpTimeoutMs);

  Serial.print("Sending HTTP GET request to: ");
  Serial.println(url);

  // Send HTTP GET Request
  httpResponseCode = http.GET();

  // Initialize Response Content
  String payload = "{}";

  // Check Response Code and Process Response Content
  if (httpResponseCode > 0) {
    Serial.print("HTTP Response code: ");
    Serial.println(httpResponseCode);
    
    if (httpResponseCode == HTTP_CODE_OK) {
      // Copy Response to Buffer
      payload = http.getString();
    } else {
      Serial.println("Request failed with non-200 status code");
    }
  } else {
    Serial.print("HTTP Request failed, error code: ");
    Serial.println(httpResponseCode);
  }
  
  // Release HTTP Client Resources
  http.end();
  
  return payload;
}

//=============================================================================
// Weather Forecast Data Processing Functions
//=============================================================================

/**
 * Maps OpenWeatherMap icon code to our internal icon number
 * 
 * @param OpenWeatherMapIcon OpenWeatherMap icon code (e.g., "01d")
 * @return Corresponding internal icon number
 */
int getWeatherIconNum(String OpenWeatherMapIcon) {
  // Search for a Match from the Mapping Table
  if (WEATHER_MAPPINGS.find(OpenWeatherMapIcon) != WEATHER_MAPPINGS.end()) {
    return WEATHER_MAPPINGS[OpenWeatherMapIcon];
  }
  
  // Default is Cloudy if no match found
  Serial.println("Warning: No icon match found for " + OpenWeatherMapIcon + ", using default");
  return ICON_THUNDERSTORM;
}

/**
 * Stores hourly weather information in the forecast array.
 * The label is formatted as "HH:MM" local time.
 *
 * @param index      Index in the forecast array
 * @param unixTime   Unix timestamp (UTC)
 * @param iconCode   OpenWeatherMap icon code
 * @param temperature Temperature
 * @param pop        Probability of precipitation
 */
void storeHourlyInfo(int index, long unixTime, String iconCode, float temperature, float pop) {
  if (index < 0 || index >= FORECAST_COUNT) {
    displayErrorMessage("Invalid forecast index");
    enterDeepSleep(true);
    return;
  }

  // Convert UTC Unix Timestamp to Local Time
  time_t localTime = unixTime + (time_t)(TIMEZONE_OFFSET * 3600);
  struct tm *timeinfo = gmtime(&localTime);  // gmtime on pre-offset value

  char tempLabel[6];
  strftime(tempLabel, sizeof(tempLabel), "%k:%M", timeinfo);
  forecasts[index].label       = String(tempLabel);
  forecasts[index].iconNumber  = getWeatherIconNum(iconCode);
  forecasts[index].temperature = temperature;
  forecasts[index].pop         = pop;
}

/**
 * Stores daily weather information in the forecast array.
 * The label is a three-letter weekday abbreviation (e.g. "Mon").
 * Temperature is taken from the daily midday (feels_like.day or temp.day).
 *
 * @param index      Index in the forecast array
 * @param unixTime   Unix timestamp (UTC) of the day
 * @param iconCode   OpenWeatherMap icon code
 * @param temperature Daytime temperature
 * @param pop        Probability of precipitation
 */
void storeDailyInfo(int index, long unixTime, String iconCode, float temperature, float pop) {
  if (index < 0 || index >= FORECAST_COUNT) {
    displayErrorMessage("Invalid forecast index");
    enterDeepSleep(true);
    return;
  }

  // Convert UTC Unix Timestamp to Local Time
  time_t localTime = unixTime + (time_t)(TIMEZONE_OFFSET * 3600);
  struct tm *timeinfo = gmtime(&localTime);  // gmtime on pre-offset value

  char tempLabel[5]; // "Mon\0"
  strftime(tempLabel, sizeof(tempLabel), " %a", timeinfo);
  forecasts[index].label       = String(tempLabel);
  forecasts[index].iconNumber  = getWeatherIconNum(iconCode);
  forecasts[index].temperature = temperature;
  forecasts[index].pop         = pop;
}

/**
 * Prints weather forecast data to Serial for debugging
 */
void printWeatherData() {
  Serial.println("\n--- Weather Forecast Data ---");
  for (int i = 0; i < FORECAST_COUNT; i++) {
    if (forecasts[i].label.length() > 0) {
      Serial.print("Label: ");
      Serial.print(forecasts[i].label);
      Serial.print(" | IconNumber: ");
      Serial.print(forecasts[i].iconNumber);
      Serial.print(" | Temperature: ");
      Serial.print(forecasts[i].temperature);
      Serial.print(" | POP: ");
      Serial.print(forecasts[i].pop);
      Serial.println("%");
    }
  }
}

/**
 * Fetches weather forecast data from OpenWeatherMap API or test data.
 *
 * The `exclude` parameter is adjusted per mode:
 *   - HOURLY mode excludes daily data
 *   - DAILY  mode excludes hourly data
 *
 * @param useTestData If true, uses test data instead of API
 * @return JSON string containing weather data, or empty string on failure
 */
String fetchWeatherData(bool useTestData = TEST_MODE) {
  // Returns Test Data When in test mode
  if (useTestData) {
    Serial.println("Using test data instead of API");
    return String(TEST_WEATHER_DATA);
  }

  // Check WiFi Connection Status
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi Disconnected. Attempting to reconnect...");
    if (!connectToWiFi()) {
      Serial.println("WiFi reconnection failed");
      return "";
    }
  }

  // Build OpenWeatherMap API Request URL
  // Exclude the data array that is NOT needed for the current mode
  const char* excludeParam = (currentMode == MODE_HOURLY)
    ? "minutely,daily,alerts"
    : "minutely,hourly,alerts";

  String url = "http://api.openweathermap.org";
  url += "/data/3.0/onecall";
  url += "?lat=" + String((float) LATITUDE, 5);
  url += "&lon=" + String((float) LONGITUDE, 5);
  url += "&units=" + String(TEMPERATURE_UNIT == 0 ? "metric" : "imperial") + "&lang=en&exclude=" + excludeParam;
  url += "&appid=" + (String) OPENWEATHERMAP_API_KEY;

  Serial.print("Fetching weather forecast data (mode: ");
  Serial.print(currentMode == MODE_HOURLY ? "HOURLY" : "DAILY");
  Serial.println(") from OpenWeatherMap...");

  // Send HTTP Request and Get Response
  int retryCount = 0;
  bool exitLoop = false;
  String responseData = "";

  const int maxRetryCount = 3;     // Maximum number of retry attempts
  while (retryCount < maxRetryCount && !exitLoop) {
    responseData = httpGETRequest(url.c_str());

    switch (httpResponseCode) {
      case HTTP_CODE_OK:
        exitLoop = true;
        break;
      case HTTP_CODE_BAD_REQUEST:
        exitLoop = true;
        displayErrorMessage("Either some mandatory parameters in the request are missing or some of request parameters have incorrect format or values out of allowed range.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_UNAUTHORIZED:
        exitLoop = true;
        displayErrorMessage("Unauthorized. API token was not provided in the request or API token provided in the request is not granted access.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_NOT_FOUND:
        exitLoop = true;
        displayErrorMessage("Data with requested parameters (lat, lon, date etc) does not exist in service database.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_TOO_MANY_REQUESTS:
        exitLoop = true;
        displayErrorMessage("Key quota of requests for provided API to this API was exceeded.");
        enterDeepSleep(false);
        break;
      case HTTP_CODE_INTERNAL_SERVER_ERROR:
      case HTTP_CODE_BAD_GATEWAY:
      case HTTP_CODE_SERVICE_UNAVAILABLE:
      case HTTP_CODE_GATEWAY_TIMEOUT:
      case HTTPC_ERROR_READ_TIMEOUT:
        Serial.println("Unexpected Error.");
        retryCount++;
        Serial.print("Retry attempt ");
        Serial.print(retryCount);
        Serial.print(" of ");
        Serial.println(maxRetryCount);
        delay(60000);
        break;
      default:
        exitLoop = true;
        char numchar[10];
        std::sprintf(numchar, "Unknown Error. httpResponseCode: %d", httpResponseCode);
        displayErrorMessage(numchar);
        enterDeepSleep(false);
        break;
    }
  }

  if (!exitLoop) {
    displayErrorMessage("Failed to fetch weather forecast data after multiple attempts");
    enterDeepSleep(false);
    return "";
  }

  return responseData;
}

/**
 * Analyzes hourly weather data JSON and stores it in the forecast array.
 *
 * Slot 0 = current conditions.
 * Slots 1–4 = hourly[3], hourly[6], hourly[9], hourly[12]
 * (i.e. +3 h, +6 h, +9 h, +12 h from now, starting at the next round hour).
 *
 * @param jsonData JSON string containing weather data
 * @return true if analysis was successful, false otherwise
 */
bool analyzeHourlyWeatherData(const String& jsonData) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonData);

  if (error != DeserializationError::Ok) {
    Serial.print("JSON parsing failed! Error: ");
    Serial.println(error.c_str());
    String errorMsg = "JSON parsing failed! Error: " + String(error.c_str());
    displayErrorMessage(errorMsg.c_str());
    return false;
  }

  // Slot 0: current conditions
  storeHourlyInfo(0,
    doc["current"]["dt"],
    doc["current"]["weather"][0]["icon"].as<String>(),
    doc["current"]["temp"].as<float>(),
    doc["current"]["pop"].as<float>());

  // Determine the offset into the hourly array for the NEXT full hour.
  // The OWC hourly array starts at the current hour, so index 1 is already
  // the next full-hour entry.  We then step every 3 hours from there.
  // Slots: hourly[1], hourly[4], hourly[7], hourly[10]  → +3 h steps
  const int hourlyIndices[] = {1, 4, 7, 10};

  for (int i = 0; i < 4; i++) {
    int idx = hourlyIndices[i];
    if (idx < (int)doc["hourly"].size()) {
      storeHourlyInfo(i + 1,
        doc["hourly"][idx]["dt"],
        doc["hourly"][idx]["weather"][0]["icon"].as<String>(),
        doc["hourly"][idx]["temp"].as<float>(),
        doc["hourly"][idx]["pop"].as<float>());
    } else {
      Serial.print("Warning: hourly index ");
      Serial.print(idx);
      Serial.println(" is out of range");
    }
  }

  printWeatherData();
  Serial.println("Hourly weather data analyzed successfully");
  return true;
}

/**
 * Analyzes daily weather data JSON and stores it in the forecast array.
 *
 * Slots 0–4 = daily[0] … daily[4]  (today + next 4 days).
 * Temperature shown is the daytime high (temp.day).
 *
 * @param jsonData JSON string containing weather data
 * @return true if analysis was successful, false otherwise
 */
bool analyzeDailyWeatherData(const String& jsonData) {
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, jsonData);

  if (error != DeserializationError::Ok) {
    Serial.print("JSON parsing failed! Error: ");
    Serial.println(error.c_str());
    String errorMsg = "JSON parsing failed! Error: " + String(error.c_str());
    displayErrorMessage(errorMsg.c_str());
    return false;
  }

  for (int i = 0; i < FORECAST_COUNT; i++) {
    if (i < (int)doc["daily"].size()) {
      storeDailyInfo(i,
        doc["daily"][i]["dt"],
        doc["daily"][i]["weather"][0]["icon"].as<String>(),
        doc["daily"][i]["temp"]["day"].as<float>(),
        doc["daily"][i]["pop"].as<float>());
    } else {
      Serial.print("Warning: daily index ");
      Serial.print(i);
      Serial.println(" is out of range");
    }
  }

  printWeatherData();
  Serial.println("Daily weather data analyzed successfully");
  return true;
}

/**
 * Dispatches JSON parsing to the correct analyzer for the active mode.
 *
 * @param jsonData JSON string containing weather data
 * @return true if analysis was successful, false otherwise
 */
bool analyzeWeatherData(const String& jsonData) {
  if (currentMode == MODE_HOURLY) {
    return analyzeHourlyWeatherData(jsonData);
  } else {
    return analyzeDailyWeatherData(jsonData);
  }
}

/**
 * Fetches and analyzes weather forecast data
 * 
 * Retrieves current weather and hourly forecast data,
 * then stores it in the forecast array
 */
void fetchAndAnalyzeWeatherData() {
  // Fetch weather data
  String jsonData = fetchWeatherData();
  
  // Check if data was fetched successfully
  if (jsonData.isEmpty()) {
    return;
  }
  
  // Store JSON data in global buffer for potential debugging
  jsonBuffer = jsonData;
  
  // Analyze the weather data
  if (!analyzeWeatherData(jsonData)) {
    enterDeepSleep(true);
  }
}

/**
 * Function to Perform Initialization
 *
 * On every boot the wakeup cause is checked:
 *   - EXT0 (button press): toggle the display mode, then proceed normally.
 *   - Timer or first power-on:  use the current (persisted) mode.
 */
void setup() {
  // Initialize Serial Communication
  Serial.begin(115200);
  Serial.println("Weather Forecast Display System Starting...");

  // Configure the side button pin
  pinMode(SIDE_BUTTON_PIN, INPUT_PULLUP);

  // Check wakeup cause and toggle mode when the button was pressed
  esp_sleep_wakeup_cause_t wakeupCause = esp_sleep_get_wakeup_cause();
  if (wakeupCause == ESP_SLEEP_WAKEUP_EXT0) {
    // Button wakeup: flip the mode
    currentMode = (currentMode == MODE_HOURLY) ? MODE_DAILY : MODE_HOURLY;
    Serial.print("Button pressed – switching to mode: ");
    Serial.println(currentMode == MODE_HOURLY ? "HOURLY" : "DAILY");
  } else {
    Serial.print("Timer/power-on wakeup – current mode: ");
    Serial.println(currentMode == MODE_HOURLY ? "HOURLY" : "DAILY");
  }

  // Set E-Paper Display Power Pin
  const int epdPowerPin = 7;     // GPIO pin for E-Paper power control
  pinMode(epdPowerPin, OUTPUT);
  digitalWrite(epdPowerPin, HIGH);

  // Initialize E-Paper Display GPIO
  EPD_GPIOInit();

  // Connect to WiFi and synchronize the system clock via NTP.
  // This must happen even in TEST_MODE so that microsecondsUntilNextHour()
  // has a valid clock to work with.
  if (!connectToWiFi()) {
    displayErrorMessage("WiFi Connection Error");
    enterDeepSleep(false);
    return;
  }

  // Fetch and Analyze Weather forecast Data (uses test JSON when TEST_MODE=true)
  fetchAndAnalyzeWeatherData();

  // Disconnect WiFi to save power
  disconnectWiFi();

  // Display Weather Forecast
  displayWeatherForecast();

  // Enter Deep-Sleep Mode (timer aligned to next hour in HOURLY mode)
  enterDeepSleep(true);
}

/**
 * Main Loop Function
 * Not Executed When Using Deep-Sleep Mode
 */
void loop() {
}
