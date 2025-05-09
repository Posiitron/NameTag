/**
 * @file BleQrBadge_MultiScreen_V1.ino
 * @brief E-Paper badge: Displays Personal Info or QR Code screens.
 *        Switches screens via BLE command or physical button.
 *        Receives data for each screen type via a single BLE characteristic.
 *        Uses Full updates for screen changes.
 *        Landscape & Centered. Single Service/Characteristic.
 * @author Amir Akrami (modified based on user request)
 */

// Core GxEPD2 library
#include <GxEPD2_BW.h>

// Font library for messages
#include <Fonts/FreeSans9pt7b.h>  // Using 9pt font for info/status
#include <Fonts/FreeSans12pt7b.h> // Larger font for info screen maybe?

// QR Code Generation Library
#include <qrcode.h>

// Include Arduino core
#include <Arduino.h>

// === IMPORTANT: Display Configuration Header ===
// Selected display type is in this file
#include "GxEPD2_display_selection_new_style.h"
// === IMPORTANT ===

// --- BLE Includes ---
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEServer.h>
#include <BLECharacteristic.h>
#include <BLEAdvertising.h>
#include <BLE2902.h>

// *** ADD BLE SECURITY INCLUDE ***
#include <BLESecurity.h>

// *** INCLUDE OneButton LIBRARY ***
#include <OneButton.h>

// ... other includes ...
#include <Preferences.h> // For Non-Volatile Storage

// NVS Keys
const char *NVS_NAMESPACE = "badgeData";
const char *NVS_KEY_INFO = "persInfo";
const char *NVS_KEY_QR = "qrData";
const char *NVS_KEY_MODE = "dispMode";

// New Characteristic UUIDs (Derive from your service UUID or generate new ones)
#define NAME_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26aa"  // Example: +1
#define EMAIL_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ab" // Example: +2
#define PHONE_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ac" // Example: +3
#define QRURL_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26ad" // Example: +4
// Note: Battery Service/Characteristic have standard UUIDs

// Battery Monitoring (Adjust pin if needed - common on LilyGo boards)
#define BATT_ADC_PIN 35
#define BATT_VOLTAGE_MAX 4.2
#define BATT_VOLTAGE_MIN 3.0

// Global Preferences object
Preferences preferences;

// Global Characteristic pointers for reading/notifications
BLECharacteristic *pNameCharacteristic = NULL;
BLECharacteristic *pEmailCharacteristic = NULL;
BLECharacteristic *pPhoneCharacteristic = NULL;
BLECharacteristic *pQrUrlCharacteristic = NULL;
BLECharacteristic *pBatteryLevelCharacteristic = NULL;

// Battery Notification Timer
unsigned long lastBatteryUpdateTime = 0;
const unsigned long BATTERY_UPDATE_INTERVAL_MS = 15000; // Update every 15 seconds

// Wake Timer Control
unsigned long wakeStartTime = 0;

// ===================================================================================
// Configuration Constants
// ===================================================================================

// --- Display Modes ---
enum DisplayMode
{
    INFO,
    QR_CODE,
    BLANK // Represents a cleared state
};

// --- QR Code Configuration ---
const int FIXED_QR_VERSION = 7;
const int FIXED_QR_SCALE = 2;                 // Adjust scale based on your display size and desired QR size
const int MAX_QR_INPUT_STRING_LENGTH = 90;    // Max length for QR data
const int MAX_INFO_INPUT_STRING_LENGTH = 150; // Max length for personal info data
const int QR_QUIET_ZONE_MODULES = 4;          // Standard quiet zone

// --- BLE Configuration ---
// TODO: Generate my own unique UUIDs for production!
#define SERVICE_UUID "4fafc201-1fb5-459e-8fcc-c5c9c331914c"             // Changed last char for distinction
#define DATA_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a9" // Changed last char

const char *bleDeviceName = "PixelTag";

// --- Button Configuration ---
#define BUTTON_PIN 39 // GPIO0 is often the 'BOOT' button on ESP32 dev boards. Change if needed.

// ===================================================================================
// Global Variables
// ===================================================================================
// --- Display ---
// 'display' object created in GxEPD2_display_selection_new_style.h

// --- State & Data Handling ---
DisplayMode currentMode = INFO;   // Start by showing info screen
DisplayMode requestedMode = INFO; // Mode requested by BLE or button
String personalInfo = "No Info Received Yet.\nUse BLE to send data.";
String qrCodeData = "";                 // Start with no QR data
bool displayUpdateRequestNeeded = true; // Trigger initial display update
bool clearDisplayRequested = false;     // Flag for clear command

// --- Data Received Flags (set by BLE callback) ---
bool newInfoDataReceived = false;
bool newQrDataReceived = false;

// --- Button State ---
unsigned long lastButtonActionTime = 0;        // <<< ADD THIS LINE (Timestamp of last action)
const unsigned long BUTTON_COOLDOWN_MS = 5000; // <<< ADD THIS LINE (5 seco
// --- BLE ---
BLEServer *pServer = NULL;
BLECharacteristic *pDataCharacteristic = NULL;
bool deviceConnected = false;

// *** CREATE OneButton INSTANCE ***
// Param 1: Pin, Param 2: active LOW (true for INPUT_PULLUP), Param 3: enable internal pull-up (true)
OneButton button(BUTTON_PIN, true, true);

// ===================================================================================
// Function Prototypes
// ===================================================================================
void setupBLE();
void updateDisplay();    // Main function to refresh screen based on currentMode (FULL UPDATE)
void drawInfoScreen();   // Draws the personal info content
void drawQrScreen();     // Draws the QR code content or error message
void performFullClear(); // Clears screen fully (FULL UPDATE)
void drawCenteredText(const char *text, int baselineY, const GFXfont *font, uint16_t color = GxEPD_BLACK, int targetW = -1, int targetX = 0);
bool drawQrCode(int x_target_area, int y_target_area, int w_target_area, int h_target_area, const char *text);

uint8_t readBatteryLevel();
void sendBatteryNotification();
// *** ADD NEW CALLBACK PROTOTYPE ***
void handleButtonClick(); // Callback function for OneButton
// ===================================================================================
// BLE Callback Classes
// ===================================================================================

// Generic callback for READ requests on new characteristics
class ReadCharacteristicCallbacks : public BLECharacteristicCallbacks
{
    void onRead(BLECharacteristic *pCharacteristic)
    {
        Serial.printf("Read request for characteristic: %s\n", pCharacteristic->getUUID().toString().c_str());

        // Determine which characteristic is being read and set its value
        if (pCharacteristic == pNameCharacteristic)
        {
            // Extract first line from personalInfo
            int newlinePos = personalInfo.indexOf('\n');
            String name = (newlinePos != -1) ? personalInfo.substring(0, newlinePos) : personalInfo;
            pCharacteristic->setValue(name.c_str());
            Serial.printf(" Responding with Name: %s\n", name.c_str());
        }
        else if (pCharacteristic == pEmailCharacteristic)
        {
            // Extract second line
            int firstNewline = personalInfo.indexOf('\n');
            int secondNewline = (firstNewline != -1) ? personalInfo.indexOf('\n', firstNewline + 1) : -1;
            String emailTitle = "";
            if (firstNewline != -1)
            {
                emailTitle = (secondNewline != -1) ? personalInfo.substring(firstNewline + 1, secondNewline) : personalInfo.substring(firstNewline + 1);
            }
            pCharacteristic->setValue(emailTitle.c_str());
            Serial.printf(" Responding with Email/Title: %s\n", emailTitle.c_str());
        }
        else if (pCharacteristic == pPhoneCharacteristic)
        {
            // Extract third line
            int firstNewline = personalInfo.indexOf('\n');
            int secondNewline = (firstNewline != -1) ? personalInfo.indexOf('\n', firstNewline + 1) : -1;
            String phone = "";
            if (secondNewline != -1)
            {
                phone = personalInfo.substring(secondNewline + 1);
            }
            pCharacteristic->setValue(phone.c_str());
            Serial.printf(" Responding with Phone: %s\n", phone.c_str());
        }
        else if (pCharacteristic == pQrUrlCharacteristic)
        {
            pCharacteristic->setValue(qrCodeData.c_str());
            Serial.printf(" Responding with QR URL: %s\n", qrCodeData.c_str());
        }
        else if (pCharacteristic == pBatteryLevelCharacteristic)
        {
            uint8_t level = readBatteryLevel();
            pCharacteristic->setValue(&level, 1);
            Serial.printf(" Responding with Battery Level: %d\n", level);
        }
        // Add other characteristics if needed
    }
};

// --- Server Connection Callbacks ---
class MyServerCallbacks : public BLEServerCallbacks
{
    void onConnect(BLEServer *pServerInstance)
    {
        deviceConnected = true;
        pServer = pServerInstance;
        Serial.println("[DEBUG] === BLE Client Connected ===");
        // Optionally log client address if available:
        // BLEAddress clientAddress = pServerInstance->getConnInfo(pServerInstance->getConnId()).getAddress();
        // Serial.printf("[DEBUG] Client Address: %s\n", clientAddress.toString().c_str());
    }

    void onDisconnect(BLEServer *pServerInstance)
    {
        deviceConnected = false;
        Serial.println("[DEBUG] === BLE Client Disconnected ===");
        wakeStartTime = millis(); // <<< ADD THIS LINE to restart sleep timer
        Serial.println("[DEBUG] onDisconnect: Sleep timeout timer restarted.");
        // Advertising is stopped in the sleep logic before sleeping
    }
};

// --- Data Characteristic Write Callback ---
class DataCharacteristicCallbacks : public BLECharacteristicCallbacks
{
    void onWrite(BLECharacteristic *pCharacteristic)
    {
        std::string value = pCharacteristic->getValue();
        String valueStr = String(value.c_str());
        valueStr.trim();

        Serial.printf("[DEBUG] onWrite: Received BLE Value: '%s' (Length: %d)\n", valueStr.c_str(), value.length());

        bool dataChanged = false; // Flag to check if NVS needs update

        if (valueStr.equalsIgnoreCase("command:clear"))
        {
            Serial.println("Clear command received.");
            clearDisplayRequested = true;
            newInfoDataReceived = false;
            newQrDataReceived = false;
            // Optional: maybe set requestedMode to BLANK here too?
            // requestedMode = BLANK;
        }
        else if (valueStr.equalsIgnoreCase("display:info"))
        {
            Serial.println("Display Info command received.");
            requestedMode = INFO;
        }
        else if (valueStr.equalsIgnoreCase("display:qr"))
        {
            Serial.println("Display QR command received.");
            requestedMode = QR_CODE;
        }
        else if (valueStr.startsWith("data:personal:"))
        {
            String infoPayload = valueStr.substring(strlen("data:personal:"));
            if (personalInfo != infoPayload)
            { // Check if data actually changed
                personalInfo = infoPayload;
                personalInfo.replace("\\n", "\n"); // Still useful if app sends literal \\n
                newInfoDataReceived = true;
                clearDisplayRequested = false;
                requestedMode = INFO;
                dataChanged = true; // Mark that NVS needs update
                Serial.println("Automatically requesting INFO mode.");
            }
        }
        else if (valueStr.startsWith("data:qr:"))
        {
            String qrPayload = valueStr.substring(strlen("data:qr:"));
            if (qrPayload.length() <= MAX_QR_INPUT_STRING_LENGTH)
            {
                if (qrCodeData != qrPayload)
                { // Check if data actually changed
                    qrCodeData = qrPayload;
                    newQrDataReceived = true;
                    clearDisplayRequested = false;
                    requestedMode = QR_CODE;
                    dataChanged = true; // Mark that NVS needs update
                    Serial.println("Automatically requesting QR_CODE mode.");
                }
            }
        }
        else
        {
            Serial.println("Received unrecognized command/data format. Ignoring.");
        }
        // --- Save to NVS if data changed ---
        if (dataChanged)
        {
            preferences.begin(NVS_NAMESPACE, false); // Open read/write
            if (newInfoDataReceived)
            {
                preferences.putString(NVS_KEY_INFO, personalInfo);
                Serial.println("Personal info saved to NVS.");
            }
            if (newQrDataReceived)
            {
                preferences.putString(NVS_KEY_QR, qrCodeData);
                Serial.println("QR data saved to NVS.");
            }
            preferences.end(); // Close NVS
        }
    }
};

// ===================================================================================
// Setup Function
// ===================================================================================
void setup()
{
    Serial.begin(115200);
    while (!Serial && millis() < 2000)
        Serial.println("\n[DEBUG] Starting BLE Multi-Screen Badge V2 (setup)");

    // --- Initialize NVS ---
    Serial.println("[DEBUG] setup: Initializing NVS...");
    // Read-only initially to load faster if data exists
    bool nvsOk = preferences.begin(NVS_NAMESPACE, true);
    if (!nvsOk)
    {
        Serial.println("NVS Read-Only failed, trying Read/Write...");
        // If read-only fails (maybe first boot?), try read/write
        nvsOk = preferences.begin(NVS_NAMESPACE, false);
    }

    if (nvsOk)
    {
        personalInfo = preferences.getString(NVS_KEY_INFO, "Default Name\nDefault Title\n"); // Load or default
        qrCodeData = preferences.getString(NVS_KEY_QR, "");                                  // Load or default (empty)
        currentMode = (DisplayMode)preferences.getUInt(NVS_KEY_MODE, (unsigned int)INFO);    // Load or default
        preferences.end();                                                                   // Close NVS after reading
        Serial.println("[DEBUG] setup: NVS Loaded.");
        Serial.printf(" Loaded Mode: %d\n", currentMode);
    }
    else
    {
        Serial.println("NVS failed to initialize. Using default values.");
        // Defaults are already set in global declarations
        currentMode = INFO;
    }
    requestedMode = currentMode; // Sync requested mode

    // --- Initialize Display ---
    display.init(115200);
    display.setRotation(1);
    Serial.println("[DEBUG] setup: Display initialized");

    // --- Button Setup (OneButton) ---
    button.attachClick(handleButtonClick);
    // Set pinMode explicitly just in case
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    Serial.printf("Button configured on GPIO %d\n", BUTTON_PIN);

    // --- Setup BLE (Done ONCE on boot) ---
    // We set it up fully, but only advertise when needed
    setupBLE();

    // --- Determine Wake Reason ---
    Serial.println("[DEBUG] setup: Determining wake reason...");
    esp_sleep_wakeup_cause_t wakeup_reason;
    wakeup_reason = esp_sleep_get_wakeup_cause();
    wakeStartTime = millis(); // Record when we woke up

    switch (wakeup_reason)
    {
    case ESP_SLEEP_WAKEUP_EXT0: // GPIO Wakeup
        Serial.println("[DEBUG] setup: Wakeup cause = Button Press (EXT0)");
        // Button press should keep us awake longer, maybe start advertising?
        displayUpdateRequestNeeded = true; // Show current screen immediately
        BLEDevice::startAdvertising();     // Start advertising immediately
        Serial.println("[DEBUG] setup: Advertising started (Button Wake).");

        break;

    default: // Includes power-on reset
        Serial.printf("[DEBUG] setup: Wakeup cause = Power On / Other (%d)\n", wakeup_reason);
        displayUpdateRequestNeeded = true; // Initial display update on power-on
        BLEDevice::startAdvertising();     // Start advertising on initial boot
        Serial.println("[DEBUG] setup: Advertising started (Power On).");

        break;
    }

    // Wake on Button Press (GPIO 39 = RTC GPIO 3)
    // Check ESP32 datasheet/pinout for RTC GPIO mapping if BUTTON_PIN changes
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_39, 0); // 0 = Wake on LOW level
    Serial.println("[DEBUG] setup: Button wakeup configured (EXT0 GPIO 39 LOW).");

    // Initial Display (only if needed based on wake reason)
    if (displayUpdateRequestNeeded)
    {
        Serial.println("[DEBUG] setup: Performing initial display update...");
        updateDisplay();
        display.hibernate();
        Serial.println("[DEBUG] setup: Display hibernated after initial update.");
    }
    else
    {
        Serial.println("[DEBUG] setup: Skipping initial display update.");
        // Hibernate display immediately if not updating? Important for power saving on timer wake.
        display.hibernate();
        Serial.println("[DEBUG] setup: Display hibernated (skipped initial update).");
    }
    Serial.println("[DEBUG] setup: Setup complete. Entering loop...");
}

// ===================================================================================
// Loop Function
// ===================================================================================
void loop()
{
    button.tick(); // Let the library process button state and call handleButtonClick if needed

    if (deviceConnected)
    {

        sendBatteryNotification();

        // --- Process state changes ---
        bool needsRedraw = false;
        DisplayMode previousMode = currentMode;

        // Priority 1: Clear Command (from BLE)
        if (clearDisplayRequested)
        {
            clearDisplayRequested = false;
            Serial.println("[DEBUG] loop(Connected): Processing Clear Request...");
            if (currentMode != BLANK)
            {
                currentMode = BLANK;
                performFullClear();
                needsRedraw = false; // Clear handled redraw
                display.hibernate();
            }
            else
            {
                Serial.println("[DEBUG] loop(Connected): ...already blank.");
            }
            newInfoDataReceived = false; // Reset flags
            newQrDataReceived = false;
        }
        // Priority 2: Mode Change Request (from Button or BLE callback)
        else if (requestedMode != currentMode)
        {
            Serial.printf("[DEBUG] loop(Connected): Processing Mode Change Request: %d -> %d\n", currentMode, requestedMode);
            bool allowSwitch = false;
            switch (requestedMode)
            {
            case INFO:
                allowSwitch = true;
                break;
            case QR_CODE:
                if (qrCodeData.length() > 0)
                {
                    allowSwitch = true;
                }
                else
                {
                    Serial.println("...QR mode requested, but no QR data. Reverting.");
                    requestedMode = currentMode;
                }
                break;
            case BLANK:
                allowSwitch = true;
                break;
            }

            if (allowSwitch)
            {
                currentMode = requestedMode;
                needsRedraw = true;
                Serial.printf("[DEBUG] loop(Connected): Mode changed to %d.\n", currentMode);
                if (currentMode == BLANK)
                {
                    performFullClear();
                    needsRedraw = false; // Clear handles redraw
                    display.hibernate();
                }
            }
        }
        // Priority 3: New Data Received (Redundant check, but harmless)
        else if (newInfoDataReceived && currentMode == INFO)
        {
            Serial.println("[DEBUG] loop(Connected): Processing New Info Data for Current Screen...");
            needsRedraw = true;
        }
        else if (newQrDataReceived && currentMode == QR_CODE)
        {
            Serial.println("[DEBUG] loop(Connected): Processing New QR Data for Current Screen...");
            needsRedraw = true;
        }

        // Consume data flags
        newInfoDataReceived = false;
        newQrDataReceived = false;

        // Save mode to NVS if changed
        if (currentMode != previousMode)
        {
            preferences.begin(NVS_NAMESPACE, false);
            preferences.putUInt(NVS_KEY_MODE, (unsigned int)currentMode);
            preferences.end();
            Serial.printf("[DEBUG] loop(Connected): Saved new mode (%d) to NVS.\n", currentMode);
        }

        // Trigger Display Update
        // Combine initial wake flag with redraw flag
        bool shouldUpdate = needsRedraw || displayUpdateRequestNeeded;
        displayUpdateRequestNeeded = false; // Consume initial flag regardless

        if (shouldUpdate && currentMode != BLANK)
        { // Don't redraw if just cleared
            Serial.println("[DEBUG] loop(Connected): Updating Display...");
            updateDisplay();
            Serial.println("[DEBUG] loop(Connected): Display Update Complete.");
            display.hibernate();
        }
    }
    else // --- DEVICE IS DISCONNECTED ---
    {
        // *** ADDED/MODIFIED SECTION FOR DISCONNECTED STATE ***
        bool needsRedrawDisconnected = false;
        DisplayMode previousModeDisconnected = currentMode;

        // Check for Mode Change Request triggered by Button press via button.tick() above
        if (requestedMode != currentMode)
        {
            Serial.printf("[DEBUG] loop(Disconnected): Processing Mode Change Request: %d -> %d\n", currentMode, requestedMode);
            bool allowSwitch = false;
            switch (requestedMode)
            {
            case INFO:
                allowSwitch = true;
                break;
            case QR_CODE:
                if (qrCodeData.length() > 0)
                {
                    allowSwitch = true;
                }
                else
                {
                    Serial.println("...QR mode requested, but no QR data. Reverting.");
                    requestedMode = currentMode;
                }
                break;
            case BLANK:
                allowSwitch = true;
                break;
            }

            if (allowSwitch)
            {
                currentMode = requestedMode;
                needsRedrawDisconnected = true; // Set flag for disconnected redraw
                Serial.printf("[DEBUG] loop(Disconnected): Mode changed to %d.\n", currentMode);
                if (currentMode == BLANK)
                {
                    performFullClear();
                    needsRedrawDisconnected = false; // Clear handled redraw
                    display.hibernate();
                }
            }
        }

        // Save mode to NVS if changed (also when disconnected)
        if (currentMode != previousModeDisconnected)
        {
            preferences.begin(NVS_NAMESPACE, false);
            preferences.putUInt(NVS_KEY_MODE, (unsigned int)currentMode);
            preferences.end();
            Serial.printf("[DEBUG] loop(Disconnected): Saved new mode (%d) to NVS.\n", currentMode);
        }

        // Trigger Display Update if needed (using disconnected flag)
        // Combine initial wake flag (might be true on button wake) with redraw flag
        bool shouldUpdateDisconnected = needsRedrawDisconnected || displayUpdateRequestNeeded;
        displayUpdateRequestNeeded = false; // Consume initial flag

        if (shouldUpdateDisconnected && currentMode != BLANK)
        { // Don't redraw if just cleared
            Serial.println("[DEBUG] loop(Disconnected): Updating Display...");
            updateDisplay();
            Serial.println("[DEBUG] loop(Disconnected): Display Update Complete.");
            display.hibernate();
        }
        // *** END ADDED/MODIFIED SECTION ***

        // Check Button/Power-On Wake Timeout
        if (millis() - wakeStartTime >= 60000) // Example: 60 second timeout
        {                                      // Example: 60 second timeout
            Serial.printf("[DEBUG] loop(Disconnected): Button/Power-on wake timeout reached (%lu ms elapsed).\n", millis() - wakeStartTime);
            Serial.println("[DEBUG] loop(Disconnected): Stopping advertising...");
            BLEDevice::stopAdvertising();
            Serial.println("[DEBUG] loop(Disconnected): Hibernating display...");
            display.hibernate();
            Serial.println("[DEBUG] loop(Disconnected): >>> ENTERING DEEP SLEEP (Button/Power-On Timeout) <<<");
            Serial.flush();
            esp_deep_sleep_start();
        }
    }

    delay(10);
}

// ===================================================================================
// BLE Setup Function (MODIFIED FOR SECURITY)
// ===================================================================================
void setupBLE()
{
    Serial.println("Initializing BLE...");
    BLEDevice::init(bleDeviceName);

    // --- Security Setup (Keep your working standard config) ---
    Serial.println("Setting up BLE Security...");
    BLEDevice::setEncryptionLevel(ESP_BLE_SEC_ENCRYPT);
    BLESecurity *pSecurity = new BLESecurity();
    pSecurity->setCapability(ESP_IO_CAP_NONE);
    pSecurity->setAuthenticationMode(ESP_LE_AUTH_REQ_SC_BOND);
    Serial.println("BLE Security configured.");

    // --- Create Server & Service ---
    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks()); // Handles connect/disconnect flags
    BLEService *pService = pServer->createService(SERVICE_UUID);

    // --- Existing WRITE Characteristic (for commands/data updates) ---
    pDataCharacteristic = pService->createCharacteristic(
        DATA_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_WRITE // Write is secured by connection encryption
    );
    pDataCharacteristic->setCallbacks(new DataCharacteristicCallbacks()); // Handles incoming writes
    // Add Descriptor
    BLEDescriptor *pDataDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pDataDesc->setValue("Badge Write Commands (Encrypted)");
    pDataCharacteristic->addDescriptor(pDataDesc);
    Serial.println(" Write characteristic created.");

    // --- NEW READABLE Characteristics ---
    // These require the connection to be encrypted due to device settings

    // Name Characteristic (assuming name is first line of personalInfo)
    pNameCharacteristic = pService->createCharacteristic(
        NAME_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ // Read is secured by connection encryption
    );
    pNameCharacteristic->setCallbacks(new ReadCharacteristicCallbacks()); // Use generic callback
    BLEDescriptor *pNameDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pNameDesc->setValue("Name (Read)");
    pNameCharacteristic->addDescriptor(pNameDesc);
    Serial.println(" Name characteristic created.");

    // Email/Title Characteristic (second line)
    pEmailCharacteristic = pService->createCharacteristic(
        EMAIL_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ);
    pEmailCharacteristic->setCallbacks(new ReadCharacteristicCallbacks());
    BLEDescriptor *pEmailDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pEmailDesc->setValue("Email/Title (Read)");
    pEmailCharacteristic->addDescriptor(pEmailDesc);
    Serial.println(" Email/Title characteristic created.");

    // Phone Characteristic (third line)
    pPhoneCharacteristic = pService->createCharacteristic(
        PHONE_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ);
    pPhoneCharacteristic->setCallbacks(new ReadCharacteristicCallbacks());
    BLEDescriptor *pPhoneDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pPhoneDesc->setValue("Phone (Read)");
    pPhoneCharacteristic->addDescriptor(pPhoneDesc);
    Serial.println(" Phone characteristic created.");

    // QR URL Characteristic
    pQrUrlCharacteristic = pService->createCharacteristic(
        QRURL_CHARACTERISTIC_UUID,
        BLECharacteristic::PROPERTY_READ);
    pQrUrlCharacteristic->setCallbacks(new ReadCharacteristicCallbacks());
    BLEDescriptor *pQrUrlDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pQrUrlDesc->setValue("QR URL (Read)");
    pQrUrlCharacteristic->addDescriptor(pQrUrlDesc);
    Serial.println(" QR URL characteristic created.");

    // --- Standard Battery Service & Characteristic ---
    BLEService *pBatteryService = pServer->createService(BLEUUID((uint16_t)0x180F));
    pBatteryLevelCharacteristic = pBatteryService->createCharacteristic(
        BLEUUID((uint16_t)0x2A19),
        // Read & Notify, secured by connection encryption
        BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
    // Add CCCD (Client Characteristic Configuration Descriptor - UUID 0x2902)
    // This is REQUIRED for notifications to work. The library adds it implicitly usually
    // but adding manually is safer. Needs BLESecurity.h included.
    pBatteryLevelCharacteristic->addDescriptor(new BLE2902()); // Use standard BLE2902 descriptor helper
    Serial.println(" Battery characteristic created.");

    // --- Start Services ---
    pService->start();
    pBatteryService->start(); // Start battery service too

    // --- Advertising (Setup, but don't start automatically here) ---
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->addServiceUUID(BLEUUID((uint16_t)0x180F)); // Advertise battery service too
    pAdvertising->setScanResponse(true);
    // BLEDevice::startAdvertising(); // <<< DO NOT START HERE - Start based on wake reason in setup()

    Serial.println("BLE Services Started. Advertising setup complete.");
}

// ===================================================================================
// Handle Button Press Function (Callback for OneButton)
// ===================================================================================
void handleButtonClick()
{
    // Cooldown Check
    if (millis() - lastButtonActionTime < BUTTON_COOLDOWN_MS)
    {
        Serial.println("[DEBUG] Button Click Ignored (Cooldown Active)");
        return; // Exit if within cooldown period
    }

    // If cooldown passed, proceed:
    Serial.println("[DEBUG] Button Click Detected (OneButton)");

    // Store the mode *before* potentially changing the request
    // to see if an actual state change is likely needed later.
    DisplayMode modeBeforeRequest = requestedMode;

    // --- Mode Switching Logic (INFO -> QR -> BLANK -> INFO) ---
    Serial.printf("[DEBUG] Button Check: currentMode=%d, qrCodeData.length()=%d\n", currentMode, qrCodeData.length());

    switch (currentMode)
    {
    case INFO:
        if (qrCodeData.length() > 0)
        {
            requestedMode = QR_CODE;
            Serial.println("[DEBUG] Button: Requesting QR_CODE mode.");
        }
        else
        {
            requestedMode = BLANK; // Go to blank if no QR data
            Serial.println("[DEBUG] Button: Requesting BLANK mode (QR data missing).");
        }
        break;
    case QR_CODE:
        requestedMode = BLANK; // Next state is BLANK
        Serial.println("[DEBUG] Button: Requesting BLANK mode (Clear).");
        break;
    case BLANK:
        requestedMode = INFO; // Next state is INFO
        Serial.println("[DEBUG] Button: Requesting INFO mode (from BLANK).");
        break;
    default:
        requestedMode = INFO; // Fallback
        break;
    }

    // --- UPDATE ACTION TIMESTAMP ---
    // Update only if a mode change was successfully requested AND
    // it's different from the current mode (meaning the loop will process it)
    // OR if the requested mode itself changed from before the switch logic ran
    if (requestedMode != currentMode || requestedMode != modeBeforeRequest)
    {
        lastButtonActionTime = millis(); // Start cooldown timer
        Serial.println("[DEBUG] Cooldown timer started.");
    }
    else
    {
        Serial.println("[DEBUG] Button press resulted in no mode change request, cooldown not started.");
    }
}

// ===================================================================================
// Update Display Function (FULL UPDATE)
// ===================================================================================
void updateDisplay()
{
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE); // Clear buffer for this page
        switch (currentMode)
        {
        case INFO:
            drawInfoScreen();
            break;
        case QR_CODE:
            // Only attempt to draw QR if data actually exists
            if (qrCodeData.length() > 0)
            {
                drawQrScreen();
            }
            else
            {
                // This case should ideally be prevented by the logic in loop()
                // but as a fallback, show an error message.
                Serial.println("Error: Tried to draw QR screen with no data!");
                drawCenteredText("No QR Data Available", display.height() / 2, &FreeSans9pt7b, GxEPD_BLACK);
            }
            break;
        case BLANK:
            // Already cleared by fillScreen, do nothing else
            break;
        }
    } while (display.nextPage());
    Serial.printf("Full display update performed for mode: %d\n", currentMode);
}

// ===================================================================================
// Perform Full Clear Function (FULL UPDATE)
// ===================================================================================
void performFullClear()
{
    Serial.println("Performing full screen clear...");
    display.setFullWindow();
    display.firstPage();
    do
    {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
    Serial.println("Screen cleared.");
    currentMode = BLANK;   // Ensure state reflects the cleared screen
    requestedMode = BLANK; // Sync requested mode too
}

// ===================================================================================
// Draw Info Screen Function (Called during FULL UPDATE)
// ===================================================================================
void drawInfoScreen()
{
    Serial.printf("Drawing Info Screen with data: '%s'\n", personalInfo.c_str());
    const GFXfont *infoFont = &FreeSans12pt7b; // Use a slightly larger font
    display.setFont(infoFont);
    display.setTextColor(GxEPD_BLACK);

    // Basic multi-line handling (split by '\n')
    int16_t x1, y1;
    uint16_t w, h;
    char textBuf[MAX_INFO_INPUT_STRING_LENGTH + 1]; // Buffer for manipulation
    strncpy(textBuf, personalInfo.c_str(), MAX_INFO_INPUT_STRING_LENGTH);
    textBuf[MAX_INFO_INPUT_STRING_LENGTH] = '\0'; // Ensure null termination

    int lineCount = 0;
    char *lines[10]; // Max 10 lines, adjust if needed
    char *ptr = strtok(textBuf, "\n");
    while (ptr != NULL && lineCount < 10)
    {
        lines[lineCount++] = ptr;
        ptr = strtok(NULL, "\n");
    }

    if (lineCount == 0)
    { // Handle empty string case
        drawCenteredText("No Info", display.height() / 2, infoFont, GxEPD_BLACK);
        return;
    }

    // Calculate total height needed
    display.getTextBounds("Aj", 0, 0, &x1, &y1, &w, &h); // Get height of a typical line
    int lineHeight = h + 5;                              // Add some spacing between lines
    int totalTextHeight = (lineCount * h) + ((lineCount - 1) * 5);

    // Calculate starting Y position for vertical centering
    int startY = (display.height() - totalTextHeight) / 2;
    // Ensure it doesn't start above the top edge (adjust baseline relative to top)
    int baselineY = startY - y1; // y1 is typically negative (offset from baseline up to top)
    if (baselineY < -y1)
        baselineY = -y1; // Prevent drawing off the top

    // Draw each line centered horizontally
    for (int i = 0; i < lineCount; i++)
    {
        drawCenteredText(lines[i], baselineY + (i * lineHeight), infoFont, GxEPD_BLACK);
    }
}

// ===================================================================================
// Draw QR Screen Function (Called during FULL UPDATE)
// ===================================================================================
void drawQrScreen()
{
    // Use the global qrCodeData
    Serial.printf("Drawing QR Screen for: '%s'\n", qrCodeData.c_str());
    bool qrSuccess = drawQrCode(0, 0, display.width(), display.height(), qrCodeData.c_str());

    if (!qrSuccess)
    {
        Serial.println("QR Code drawing failed. Displaying error message.");
        // Use a standard font for the error message
        drawCenteredText("QR Generation Failed", display.height() / 2, &FreeSans9pt7b, GxEPD_BLACK);
    }
    else
    {
        Serial.println("QR Code drawn successfully.");
    }
}

// ===================================================================================
// Helper Function: Draw Centered Text
// ===================================================================================
void drawCenteredText(const char *text, int baselineY, const GFXfont *font, uint16_t color /* = GxEPD_BLACK */, int targetW /* = -1 */, int targetX /* = 0 */)
{
    // (Function remains largely the same, added color parameter)
    if (!font || !text || text[0] == '\0')
        return;
    int16_t x1, y1;
    uint16_t w, h;
    display.setFont(font);
    display.setTextColor(color);
    display.setTextSize(1);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h); // x1,y1 are offsets from cursor pos to top-left; w,h are bounds size

    int areaWidth = (targetW <= 0) ? display.width() : targetW;
    int areaOriginX = (targetW <= 0) ? 0 : targetX;

    // Calculate cursor X to center the text's bounding box
    // cursorX = areaOriginX + (areaWidth / 2) - (w / 2) - x1; // This centers the bounding box
    // Simpler centering (often looks better for text):
    int cursorX = areaOriginX + (areaWidth - w) / 2;

    // Adjust Y baseline if needed (e.g., prevent drawing off screen)
    if (baselineY < -y1)
        baselineY = -y1; // Make sure top of text isn't above screen (y1 is negative)
    if (baselineY > display.height() - (h + y1))
        baselineY = display.height() - (h + y1); // Prevent bottom going off screen

    display.setCursor(cursorX, baselineY);
    display.print(text);
}

// ===================================================================================
// Draw QR Code Function (Used by drawQrScreen)
// ===================================================================================
bool drawQrCode(int x_target_area, int y_target_area, int w_target_area, int h_target_area, const char *text)
{
    // (Function remains the same as previous version, just ensure logging is clear)
    if (text == NULL || text[0] == '\0')
    {
        Serial.println("QR Error: No text provided.");
        return false;
    }
    int inputLength = strlen(text);
    if (inputLength > MAX_QR_INPUT_STRING_LENGTH)
    {
        Serial.printf("QR Error: Input text too long (%d > %d).\n", inputLength, MAX_QR_INPUT_STRING_LENGTH);
        return false;
    }
    Serial.printf("Generating QR Code for: '%s' (Length: %d)\n", text, inputLength);

    // --- QR Code Generation ---
    // Ensure sufficient buffer. qrcode_getBufferSize is recommended.
    uint32_t bufferSize = qrcode_getBufferSize(FIXED_QR_VERSION);
    if (bufferSize == 0)
    {
        Serial.printf("QR Error: Could not get buffer size for version %d.\n", FIXED_QR_VERSION);
        return false;
    }
    // Dynamically allocate if large, or use large static/stack buffer if feasible
    const int MAX_STACK_QR_BUFFER = 4096; // Adjust based on ESP32 memory
    if (bufferSize > MAX_STACK_QR_BUFFER)
    {
        Serial.printf("QR Error: Calculated buffer size (%d) too large for stack buffer (%d).\n", bufferSize, MAX_STACK_QR_BUFFER);
        // Consider dynamic allocation here if needed: uint8_t* qrcodeData = new uint8_t[bufferSize];
        // Remember to delete[] qrcodeData; later!
        return false;
    }
    uint8_t qrcodeData[bufferSize]; // Use stack allocation if size is acceptable

    QRCode qrcode;
    // ECC_LOW allows more data, ECC_MEDIUM/ECC_QUARTILE/ECC_HIGH provide better error correction
    esp_err_t err = qrcode_initText(&qrcode, qrcodeData, FIXED_QR_VERSION, ECC_LOW, text);
    if (err != ESP_OK)
    {
        Serial.printf("QR Error: qrcode_initText failed. Error code: %d. Input may be too long for Version %d/ECC_LOW.\n", err, FIXED_QR_VERSION);
        // If using dynamic allocation, delete[] qrcodeData here.
        return false;
    }
    Serial.printf("QR generated: Version=%d, Size=%dx%d modules\n", qrcode.version, qrcode.size, qrcode.size);

    // --- QR Code Drawing ---
    int qr_modules_size = qrcode.size;
    int module_pixel_size = FIXED_QR_SCALE; // Scale factor
    int final_qr_pixel_size = qr_modules_size * module_pixel_size;

    // Calculate centering offset within the target area
    int x_offset = x_target_area + (w_target_area - final_qr_pixel_size) / 2;
    int y_offset = y_target_area + (h_target_area - final_qr_pixel_size) / 2;

    // Basic boundary checks (prevent drawing outside display buffer)
    if (x_offset < 0)
        x_offset = 0;
    if (y_offset < 0)
        y_offset = 0;
    // Optional: Check if it *fits* at all
    if (final_qr_pixel_size > w_target_area || final_qr_pixel_size > h_target_area)
    {
        Serial.printf("QR Warning: Scaled QR (%dpx) larger than target area (%dx%d). Will be clipped.\n", final_qr_pixel_size, w_target_area, h_target_area);
    }

    Serial.printf("Drawing QR at offset (%d, %d), scale %d. Target area: (%d,%d %dx%d)\n",
                  x_offset, y_offset, module_pixel_size, x_target_area, y_target_area, w_target_area, h_target_area);

    // Draw the QR code module by module
    // display.startWrite(); // GxEPD2 manages this within firstPage/nextPage loop
    for (int y = 0; y < qr_modules_size; y++)
    {
        for (int x = 0; x < qr_modules_size; x++)
        {
            if (qrcode_getModule(&qrcode, x, y))
            { // Check if module is black
                int moduleX = x_offset + x * module_pixel_size;
                int moduleY = y_offset + y * module_pixel_size;
                // Draw the scaled module (rectangle) - check bounds!
                if (moduleX + module_pixel_size <= display.width() && moduleY + module_pixel_size <= display.height())
                {
                    display.fillRect(moduleX, moduleY, module_pixel_size, module_pixel_size, GxEPD_BLACK);
                }
            }
        }
    }
    // display.endWrite(); // GxEPD2 manages this

    // If using dynamic allocation: delete[] qrcodeData;
    return true; // Success
}

// Function to read battery voltage and convert to percentage
uint8_t readBatteryLevel()
{
    // Configure ADC (might be needed once in setup if not default)
    // analogReadResolution(12); // 12-bit ADC
    // analogSetAttenuation(ADC_11db); // Set attenuation based on voltage divider (if any)

    uint16_t adc_value = analogRead(BATT_ADC_PIN);
    // Simple mapping assuming direct connection or known divider
    // Adjust the multiplier based on your board's voltage divider (if any)
    // Example: If ADC pin reads half the battery voltage, multiply by 2.
    // Check LilyGo T5 V2.3.1 schematic for battery circuit! Often uses a voltage divider.
    // Common dividers result in needing to multiply ADC voltage reading by ~2 or slightly more.
    // Let's assume a rough factor for now - **THIS NEEDS CALIBRATION**
    float voltage = adc_value * (3.3 / 4095.0) * 2.0; // Example: *2 for half-voltage divider

    // Clamp voltage to expected min/max
    if (voltage < BATT_VOLTAGE_MIN)
        voltage = BATT_VOLTAGE_MIN;
    if (voltage > BATT_VOLTAGE_MAX)
        voltage = BATT_VOLTAGE_MAX;

    // Calculate percentage
    uint8_t percentage = (uint8_t)(((voltage - BATT_VOLTAGE_MIN) / (BATT_VOLTAGE_MAX - BATT_VOLTAGE_MIN)) * 100.0);

    // Serial.printf("ADC: %d, Voltage: %.2fV, Percentage: %d%%\n", adc_value, voltage, percentage); // Debug
    return percentage;
}

// Function to send battery notification if connected and interval passed
void sendBatteryNotification()
{
    if (deviceConnected && pBatteryLevelCharacteristic != NULL)
    {
        if (millis() - lastBatteryUpdateTime >= BATTERY_UPDATE_INTERVAL_MS)
        {
            uint8_t level = readBatteryLevel();
            Serial.printf("[DEBUG] sendBatteryNotification: Interval passed. Level=%d%%. Notifying...\n", level);
            pBatteryLevelCharacteristic->setValue(&level, 1); // Set value (pointer to byte, length 1)
            pBatteryLevelCharacteristic->notify();            // Send notification
            lastBatteryUpdateTime = millis();                 // Reset timer
        }
    }
}