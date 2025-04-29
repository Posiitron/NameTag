/**
 * @file BleQrBadge_SingleChar_PartialText_V2.ino
 * @brief E-Paper badge: Displays QR code from BLE URL via a single characteristic.
 *        Uses partial updates for text status, full updates for QR/Clear.
 *        Command "clear" on the same characteristic clears the screen.
 *        Ensures full clear occurs when transitioning from full update (QR/Clear)
 *        to partial update (Status).
 *        Landscape & Centered. Single Service/Characteristic.
 * @author Amir Akrami
 */

// Core GxEPD2 library
#include <GxEPD2_BW.h>

// Font library for status messages
#include <Fonts/FreeSans9pt7b.h> // Using 9pt font

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

// ===================================================================================
// Configuration Constants
// ===================================================================================

// --- QR Code Configuration ---
const int FIXED_QR_VERSION = 7;
const int FIXED_QR_SCALE = 2;
const int MAX_INPUT_STRING_LENGTH = 90;
const int QR_QUIET_ZONE_MODULES = 4;

// --- BLE Configuration ---
// TODO: Generate my own unique UUIDs for production!
#define SERVICE_UUID             "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DATA_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8" // Single characteristic for URL or "clear" command

const char* bleDeviceName = "EPaper QR Badge";

// --- Partial Update Configuration ---
const int STATUS_AREA_WIDTH = 200;
const int STATUS_AREA_HEIGHT = 50;
int STATUS_AREA_X = 0;
int STATUS_AREA_Y = 0;

// ===================================================================================
// Global Variables
// ===================================================================================
// --- Display ---
// 'display' object created in GxEPD2_display_selection_new_style.h

// --- Data Handling ---
String currentDataString = "";
bool newQrDataReceived = false;
bool clearDisplayRequested = false;
bool statusUpdateRequested = false;
String statusMessageToShow = "";
// *** NEW FLAG ***: Tracks if the screen needs a full clear before the next status message
bool needsFullClearBeforeNextStatus = true; // Start true to ensure first status message clears screen properly

// --- BLE ---
BLEServer *pServer = NULL;
BLECharacteristic *pDataCharacteristic = NULL;
bool deviceConnected = false;

// ===================================================================================
// Function Prototypes
// ===================================================================================
void setupBLE();
void showStatusMessage(const char* message);       // Uses PARTIAL update
void updateQrDisplay(const char* textToEncode);    // Uses FULL update
void drawQrScreen(const char* textToEncode);       // Draws content for QR full update
void clearDisplay();                               // Uses FULL update (blank screen)
void performQuickFullClear();                      // Helper for full clear without hibernation
void drawCenteredText(const char *text, int baselineY, const GFXfont *font, int targetW = -1, int targetX = 0);
bool drawQrCode(int x_target_area, int y_target_area, int w_target_area, int h_target_area, const char *text);

// ===================================================================================
// BLE Callback Classes
// ===================================================================================

// --- Server Connection Callbacks ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("BLE Client Connected");
        statusMessageToShow = "Connected.\nWaiting for data...";
        statusUpdateRequested = true;
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("BLE Client Disconnected");
        statusMessageToShow = "Disconnected.\nAdvertising...";
        statusUpdateRequested = true;
        delay(500);
        BLEDevice::startAdvertising();
        Serial.println("Advertising restarted");
    }
};

// --- Data Characteristic Write Callback ---
class DataCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        String valueStr = String(value.c_str());
        valueStr.trim();

        Serial.print("Received Value: '");
        Serial.print(valueStr);
        Serial.println("'");

        if (valueStr.equalsIgnoreCase("clear")) {
            Serial.println("Clear command recognized.");
            clearDisplayRequested = true;
            newQrDataReceived = false;
            statusUpdateRequested = false;
        }
        else if (valueStr.length() > 0) {
            if (valueStr.length() > MAX_INPUT_STRING_LENGTH) {
                Serial.printf("Error: Received data length (%d) exceeds maximum (%d)\n", valueStr.length(), MAX_INPUT_STRING_LENGTH);
                statusMessageToShow = "Error: Data Too Long";
                statusUpdateRequested = true; // Trigger status update
                newQrDataReceived = false;
                clearDisplayRequested = false;
            } else {
                Serial.println("New QR data received.");
                currentDataString = valueStr;
                newQrDataReceived = true;
                clearDisplayRequested = false;
                statusUpdateRequested = false;
            }
        } else {
            Serial.println("Received empty value. Ignoring.");
        }
    }
};

// ===================================================================================
// Setup Function
// ===================================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000);
    Serial.println("\nStarting BLE QR Badge (Single Char, Partial Text V2)");

    // --- Initialize Display ---
    display.init(115200);
    Serial.println("Display initialized");
    display.setRotation(1);
    int screenW = display.width();
    int screenH = display.height();
    Serial.printf("Display rotation: %d. Width: %d, Height: %d\n", display.getRotation(), screenW, screenH);

    // --- Calculate Status Area Position ---
    STATUS_AREA_X = (screenW - STATUS_AREA_WIDTH) / 2;
    STATUS_AREA_Y = (screenH - STATUS_AREA_HEIGHT) / 2;
    if (STATUS_AREA_X < 0) STATUS_AREA_X = 0;
    if (STATUS_AREA_Y < 0) STATUS_AREA_Y = 0;
    Serial.printf("Status message area (for partial update): x=%d, y=%d, w=%d, h=%d\n",
                  STATUS_AREA_X, STATUS_AREA_Y, STATUS_AREA_WIDTH, STATUS_AREA_HEIGHT);

    // --- Initial Screen Clear (Full Update) ---
    Serial.println("Performing initial full screen clear.");
    performQuickFullClear(); // Use helper to clear without hibernating yet
    needsFullClearBeforeNextStatus = false; // Screen is now clean, reset flag

    // --- Show Initial Message (using partial update) ---
    statusMessageToShow = "Initializing BLE...";
    statusUpdateRequested = true; // Trigger initial status update in loop

    // --- Initialize BLE ---
    setupBLE();

    // --- Update Waiting Message ---
    statusMessageToShow = "Waiting for Connection...";
    statusUpdateRequested = true; // Trigger update in loop

    Serial.println("Setup complete. Advertising...");
}

// ===================================================================================
// Loop Function
// ===================================================================================
void loop() {
    // --- Handle Display Actions based on Flags (Priority: Clear > QR > Status) ---

    if (clearDisplayRequested) {
        clearDisplayRequested = false; // Reset flag *before* action
        Serial.println("Processing 'clear' command...");
        clearDisplay();                // FULL update to blank screen
        Serial.println("Display cleared via BLE command.");
        needsFullClearBeforeNextStatus = true; // Set flag: next status needs full clear
        display.hibernate();           // Hibernate after FULL update
    }
    else if (newQrDataReceived) {
        newQrDataReceived = false;    // Reset flag *before* action
        Serial.println("Processing new QR data...");
        updateQrDisplay(currentDataString.c_str()); // FULL update (draws QR or error)
        Serial.println("QR display update attempt complete.");
        needsFullClearBeforeNextStatus = true; // Set flag: next status needs full clear
        display.hibernate();          // Hibernate after FULL update
    }
    else if (statusUpdateRequested) {
        statusUpdateRequested = false; // Reset flag *before* action
        Serial.println("Processing status message update...");

        // *** CHECK if full clear is needed before this partial update ***
        if (needsFullClearBeforeNextStatus) {
            Serial.println("...Performing full screen clear before status update (previous update was full).");
            performQuickFullClear(); // Clear the whole screen
            needsFullClearBeforeNextStatus = false; // Reset the flag
        }

        // Now proceed with the partial update for the status message
        showStatusMessage(statusMessageToShow.c_str()); // PARTIAL update
        Serial.println("Status message updated.");
        // Note: needsFullClearBeforeNextStatus remains false after a partial update
        display.hibernate();           // Hibernate after PARTIAL update
    }

    // --- Allow BLE stack time to process ---
    delay(100);
}

// ===================================================================================
// BLE Setup Function
// ===================================================================================
void setupBLE() {
    Serial.println("Initializing BLE...");
    BLEDevice::init(bleDeviceName);

    pServer = BLEDevice::createServer();
    pServer->setCallbacks(new MyServerCallbacks());

    BLEService *pService = pServer->createService(SERVICE_UUID);

    pDataCharacteristic = pService->createCharacteristic(
                             DATA_CHARACTERISTIC_UUID,
                             BLECharacteristic::PROPERTY_WRITE
                         );
    pDataCharacteristic->setCallbacks(new DataCharacteristicCallbacks());

    // Add a User Description Descriptor (UUID 0x2901)
    // This is **highly recommended** for debugging with generic BLE tools (e.g., nRF Connect)
    // It allows tools to show a human-readable name for the characteristic.
    // While not strictly essential for function if using a dedicated app, it aids development greatly.
    BLEDescriptor* pDataDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pDataDesc->setValue("URL data or 'clear' command");
    pDataCharacteristic->addDescriptor(pDataDesc);

    pService->start();

    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    BLEDevice::startAdvertising();

    Serial.println("BLE Service Started, Advertising.");
    Serial.printf("Device Name: %s\n", bleDeviceName);
    Serial.printf("Service UUID: %s\n", SERVICE_UUID);
    Serial.printf("  Data Characteristic: %s (Write URL or 'clear')\n", DATA_CHARACTERISTIC_UUID);
}

// ===================================================================================
// Show Status Message Function (PARTIAL UPDATE)
// ===================================================================================
void showStatusMessage(const char* message) {
    // (Function remains the same as previous version)
    if (!message || message[0] == '\0') {
        Serial.println("showStatusMessage: Skipping empty message.");
        return;
    }
    Serial.printf("Display Status (Partial Update in area %d,%d %dx%d): %s\n",
                  STATUS_AREA_X, STATUS_AREA_Y, STATUS_AREA_WIDTH, STATUS_AREA_HEIGHT, message);

    display.setPartialWindow(STATUS_AREA_X, STATUS_AREA_Y, STATUS_AREA_WIDTH, STATUS_AREA_HEIGHT);
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE); // Clear only the partial window

        display.setFont(&FreeSans9pt7b);
        int16_t x1, y1; uint16_t w, h;
        const char* newline = strchr(message, '\n');

        if (newline != NULL) {
            // Two Line Message Centering
            int lenLine1 = newline - message;
            char line1[lenLine1 + 1];
            strncpy(line1, message, lenLine1);
            line1[lenLine1] = '\0';
            const char* line2 = newline + 1;

            display.getTextBounds(line1, 0, 0, &x1, &y1, &w, &h); // Use height of one line
            int totalTextHeight = h * 2 + 5;
            int baselineY1 = STATUS_AREA_Y + (STATUS_AREA_HEIGHT - totalTextHeight) / 2 + h;
            if (baselineY1 < STATUS_AREA_Y + h) baselineY1 = STATUS_AREA_Y + h;

            drawCenteredText(line1, baselineY1, &FreeSans9pt7b, STATUS_AREA_WIDTH, STATUS_AREA_X);
            drawCenteredText(line2, baselineY1 + h + 5, &FreeSans9pt7b, STATUS_AREA_WIDTH, STATUS_AREA_X);
        } else {
            // Single Line Message Centering
            display.getTextBounds(message, 0, 0, &x1, &y1, &w, &h);
            int topY = STATUS_AREA_Y + (STATUS_AREA_HEIGHT - h) / 2;
            int baselineY = topY - y1;
             if (baselineY < STATUS_AREA_Y - y1) baselineY = STATUS_AREA_Y - y1;
             if (baselineY > STATUS_AREA_Y + STATUS_AREA_HEIGHT ) baselineY = STATUS_AREA_Y + STATUS_AREA_HEIGHT;
            drawCenteredText(message, baselineY, &FreeSans9pt7b, STATUS_AREA_WIDTH, STATUS_AREA_X);
        }
    } while (display.nextPage());
}

// ===================================================================================
// Clear Display Function (FULL UPDATE)
// ===================================================================================
void clearDisplay() {
    Serial.println("Executing clearDisplay() (Full Update)...");
    performQuickFullClear(); // Use the helper function
    Serial.println("Display cleared (Full Update finished). Screen is blank.");
    // Hibernation handled by the loop
}

// ===================================================================================
// Helper Function: Perform Quick Full Clear (No Hibernate)
// ===================================================================================
/**
 * @brief Clears the entire display using a full update but DOES NOT hibernate.
 *        Used internally before showing a partial update after a full update.
 */
void performQuickFullClear() {
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
    } while (display.nextPage());
}


// ===================================================================================
// Update QR Display Function (FULL UPDATE)
// ===================================================================================
void updateQrDisplay(const char* textToEncode) {
    Serial.printf("Updating QR display (Full Update) for: '%s'\n", textToEncode);
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        drawQrScreen(textToEncode);
    } while (display.nextPage());
    // Hibernation handled by the loop
}

// ===================================================================================
// Draw QR Screen Function (Called during FULL UPDATE)
// ===================================================================================
void drawQrScreen(const char* textToEncode) {
    // (Function remains the same as previous version)
    bool qrSuccess = drawQrCode(0, 0, display.width(), display.height(), textToEncode);
    if (!qrSuccess) {
        Serial.println("QR Code drawing failed. Displaying error message (Full Update).");
        display.setFont(&FreeSans9pt7b);
        int16_t x1, y1; uint16_t w, h;
        const char* errMsg = "QR Generation Failed";
        display.getTextBounds(errMsg, 0, 0, &x1, &y1, &w, &h);
        int baselineY = (display.height() - h) / 2 - y1;
        drawCenteredText(errMsg, baselineY, &FreeSans9pt7b);
    } else {
       Serial.println("QR Code drawn successfully (Full Update).");
    }
}

// ===================================================================================
// Helper Function: Draw Centered Text
// ===================================================================================
void drawCenteredText(const char *text, int baselineY, const GFXfont *font, int targetW /* = -1 */, int targetX /* = 0 */) {
    // (Function remains the same as previous version)
    if (!font || !text || text[0] == '\0') return;
    int16_t x1, y1; uint16_t w, h;
    display.setFont(font);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);
    int areaWidth = (targetW <= 0) ? display.width() : targetW;
    int areaOriginX = (targetW <= 0) ? 0 : targetX;
    int cursorX = areaOriginX + (areaWidth - w) / 2 - x1;
    display.setCursor(cursorX, baselineY);
    display.print(text);
}

// ===================================================================================
// Draw QR Code Function (Used by FULL UPDATE's drawQrScreen)
// ===================================================================================
bool drawQrCode(int x_target_area, int y_target_area, int w_target_area, int h_target_area, const char *text) {
    // (Function remains the same as previous version)
    if (text == NULL || text[0] == '\0') { Serial.println("QR Error: No text provided."); return false; }
    int inputLength = strlen(text);
    if (inputLength > MAX_INPUT_STRING_LENGTH) { Serial.printf("QR Error: Input text too long (%d > %d).\n", inputLength, MAX_INPUT_STRING_LENGTH); return false; }
    Serial.printf("Generating QR Code for: '%s' (Length: %d)\n", text, inputLength);

    uint32_t bufferSize = qrcode_getBufferSize(FIXED_QR_VERSION);
    if (bufferSize == 0) { Serial.printf("QR Error: Could not get buffer size for version %d.\n", FIXED_QR_VERSION); return false; }
    const int MAX_STACK_QR_BUFFER = 4096;
    if (bufferSize > MAX_STACK_QR_BUFFER) { Serial.printf("QR Error: Calculated buffer size (%d) may be too large for stack.\n", bufferSize); return false; }
    uint8_t qrcodeData[bufferSize];

    QRCode qrcode;
    esp_err_t err = qrcode_initText(&qrcode, qrcodeData, FIXED_QR_VERSION, ECC_LOW, text);
    if (err != ESP_OK) { Serial.printf("QR Error: qrcode_initText failed. Error code: %d. Input may be too long for Version %d/ECC_LOW.\n", err, FIXED_QR_VERSION); return false; }
    Serial.printf("QR generated successfully: Version=%d, Size=%dx%d modules\n", qrcode.version, qrcode.size, qrcode.size);

    int qr_modules_size = qrcode.size;
    int module_pixel_size = FIXED_QR_SCALE;
    int final_qr_pixel_size = qr_modules_size * module_pixel_size;

    if (final_qr_pixel_size > w_target_area || final_qr_pixel_size > h_target_area) { Serial.printf("QR Warning: Scaled QR code data area (%dpx) might be larger than target area (%dx%d). Clipping might occur.\n", final_qr_pixel_size, w_target_area, h_target_area); }

    int x_offset = x_target_area + (w_target_area - final_qr_pixel_size) / 2;
    int y_offset = y_target_area + (h_target_area - final_qr_pixel_size) / 2;
    if (x_offset < x_target_area) x_offset = x_target_area;
    if (y_offset < y_target_area) y_offset = y_target_area;
    Serial.printf("Drawing QR Code at display offset (%d, %d) with scale %d. Target area: (%d,%d %dx%d)\n", x_offset, y_offset, module_pixel_size, x_target_area, y_target_area, w_target_area, h_target_area);

    display.startWrite();
    for (int y = 0; y < qr_modules_size; y++) {
        for (int x = 0; x < qr_modules_size; x++) {
            if (qrcode_getModule(&qrcode, x, y)) {
                int moduleX = x_offset + x * module_pixel_size;
                int moduleY = y_offset + y * module_pixel_size;
                if (moduleX >= 0 && (moduleX + module_pixel_size) <= display.width() && moduleY >= 0 && (moduleY + module_pixel_size) <= display.height()) {
                    display.fillRect(moduleX, moduleY, module_pixel_size, module_pixel_size, GxEPD_BLACK);
                }
            }
        }
    }
    display.endWrite();
    return true;
}