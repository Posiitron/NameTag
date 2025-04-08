/**
 * @file BleQrBadge_SingleChar_PartialText.ino
 * @brief E-Paper badge: Displays QR code from BLE URL via a single characteristic.
 *        Uses partial updates for text status, full updates for QR/Clear.
 *        Command "clear" on the same characteristic clears the screen.
 *        Landscape & Centered.
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
// TODO: Generate your own unique UUIDs for production!
#define SERVICE_UUID             "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define DATA_CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8" // Single characteristic for URL or "clear" command

const char* bleDeviceName = "EPaper QR Badge";

// --- Partial Update Configuration ---
// Define the area for status messages (centered)
const int STATUS_AREA_WIDTH = 200; // Adjust as needed
const int STATUS_AREA_HEIGHT = 50;  // Adjust as needed
// Calculate top-left corner for centering the status area
// Note: display dimensions might not be available yet, calculate in setup if needed
// For now, assume typical landscape small display like 250x122
// const int STATUS_AREA_X = (250 - STATUS_AREA_WIDTH) / 2; // Example, will calculate properly later
// const int STATUS_AREA_Y = (122 - STATUS_AREA_HEIGHT) / 2; // Example, will calculate properly later
int STATUS_AREA_X = 0; // Will be set in setup()
int STATUS_AREA_Y = 0; // Will be set in setup()


// ===================================================================================
// Global Variables
// ===================================================================================
// --- Display ---
// 'display' object created in GxEPD2_display_selection_new_style.h

// --- Data Handling ---
String currentDataString = "";   // Store the URL or command received via BLE
bool newQrDataReceived = false;  // Flag for new QR URL
bool clearDisplayRequested = false; // Flag for clear command
bool statusUpdateRequested = false; // Flag for new status message
String statusMessageToShow = "";   // Store status message text

// --- BLE ---
BLEServer *pServer = NULL;
BLECharacteristic *pDataCharacteristic = NULL;
bool deviceConnected = false;

// ===================================================================================
// Function Prototypes
// ===================================================================================
void setupBLE();
void showStatusMessage(const char* message); // Uses partial update
void updateQrDisplay(const char* textToEncode); // Uses full update
void drawQrScreen(const char* textToEncode);
void drawCenteredText(const char *text, int y, const GFXfont *font, int targetW = -1, int targetX = 0);
bool drawQrCode(int x_target_area, int y_target_area, int w_target_area, int h_target_area, const char *text);
void clearDisplay(); // Uses full update

// ===================================================================================
// BLE Callback Classes
// ===================================================================================

// --- Server Connection Callbacks ---
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) {
        deviceConnected = true;
        Serial.println("BLE Client Connected");
        // Set flag to show connected status in loop (using partial update)
        statusMessageToShow = "Connected.\nWaiting for data...";
        statusUpdateRequested = true;
    }

    void onDisconnect(BLEServer* pServer) {
        deviceConnected = false;
        Serial.println("BLE Client Disconnected");
        // Set flag to show disconnected status in loop (using partial update)
        statusMessageToShow = "Disconnected.\nAdvertising...";
        statusUpdateRequested = true;
        // Restart advertising
        delay(500); // Short delay before restarting
        BLEDevice::startAdvertising();
        Serial.println("Advertising restarted");
    }
};

// --- Data Characteristic Write Callback ---
class DataCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
        std::string value = pCharacteristic->getValue();
        Serial.print("Received Value: ");
        Serial.println(value.c_str());

        if (value == "clear") {
            // Received the clear command
            Serial.println("Clear command recognized.");
            clearDisplayRequested = true; // Set flag for main loop
            newQrDataReceived = false; // Ensure QR flag is off
            statusUpdateRequested = false; // Ensure status flag is off
        } else if (value.length() > 0) {
            // Received potential URL data
            if (value.length() > MAX_INPUT_STRING_LENGTH) {
                Serial.printf("Error: Received data length (%d) exceeds maximum (%d)\n", value.length(), MAX_INPUT_STRING_LENGTH);
                statusMessageToShow = "Error: Data Too Long";
                statusUpdateRequested = true;
                newQrDataReceived = false; // Ensure QR flag is off
                clearDisplayRequested = false; // Ensure clear flag is off
            } else {
                // Valid URL length, store it and set flag
                currentDataString = String(value.c_str());
                newQrDataReceived = true; // Set flag for QR update
                clearDisplayRequested = false; // Ensure clear flag is off
                statusUpdateRequested = false; // Ensure status flag is off
                Serial.println("New QR data flag set.");
                // Optionally show "Processing..." status?
                // statusMessageToShow = "Processing QR...";
                // statusUpdateRequested = true;
            }
        } else {
            Serial.println("Received empty value.");
            // Optionally show an error or ignore
        }
    }
};

// ===================================================================================
// Setup Function
// ===================================================================================
void setup() {
    Serial.begin(115200);
    while (!Serial && millis() < 2000);
    Serial.println("\nStarting BLE QR Badge (Single Char, Partial Text)");

    // --- Initialize Display ---
    display.init(115200);
    Serial.println("Display initialized");
    display.setRotation(1); // Landscape
    int screenW = display.width();
    int screenH = display.height();
    Serial.printf("Display rotation: %d. Width: %d, Height: %d\n", display.getRotation(), screenW, screenH);

    // --- Calculate Status Area Position ---
    STATUS_AREA_X = (screenW - STATUS_AREA_WIDTH) / 2;
    STATUS_AREA_Y = (screenH - STATUS_AREA_HEIGHT) / 2;
     if (STATUS_AREA_X < 0) STATUS_AREA_X = 0; // Ensure non-negative coords
     if (STATUS_AREA_Y < 0) STATUS_AREA_Y = 0;
    Serial.printf("Status message area: x=%d, y=%d, w=%d, h=%d\n", STATUS_AREA_X, STATUS_AREA_Y, STATUS_AREA_WIDTH, STATUS_AREA_HEIGHT);

    // --- Show Initial Message (using partial update) ---
    statusMessageToShow = "Initializing BLE...";
    statusUpdateRequested = true; // Trigger initial status update in loop

    // --- Initialize BLE ---
    setupBLE();

    // --- Update Waiting Message (after BLE setup) ---
    statusMessageToShow = "Waiting for Connection...";
    statusUpdateRequested = true; // Trigger update in loop

    Serial.println("Setup complete. Advertising...");
}

// ===================================================================================
// Loop Function
// ===================================================================================
void loop() {
    // Prioritize actions: Clear > QR Update > Status Update
    if (clearDisplayRequested) {
        clearDisplayRequested = false; // Reset flag
        clearDisplay();                // FULL update
        Serial.println("Display cleared via BLE command.");
        // Show confirmation status message (PARTIAL update)
        statusMessageToShow = "Display Cleared";
        statusUpdateRequested = true;
        // Hibernation happens after the subsequent status update finishes
    }
    else if (newQrDataReceived) {
        newQrDataReceived = false;    // Reset flag
        Serial.println("Processing new QR data...");
        // Optionally show "Generating..." status first (partial)
        // statusMessageToShow = "Generating QR...";
        // showStatusMessage(statusMessageToShow.c_str()); // Draw status immediately
        updateQrDisplay(currentDataString.c_str()); // FULL update (draws QR or error text)
        Serial.println("QR display update attempt complete.");
        display.hibernate();          // Hibernate after FULL update
    }
    else if (statusUpdateRequested) {
        statusUpdateRequested = false; // Reset flag
        showStatusMessage(statusMessageToShow.c_str()); // PARTIAL update
        Serial.println("Status message updated.");
        display.hibernate();           // Hibernate after PARTIAL update
    }

    // Allow BLE stack processing
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

    // --- Create the Single Data Characteristic ---
    pDataCharacteristic = pService->createCharacteristic(
                             DATA_CHARACTERISTIC_UUID,
                             BLECharacteristic::PROPERTY_WRITE // Only needs write
                         );
    pDataCharacteristic->setCallbacks(new DataCharacteristicCallbacks()); // Assign the callback

    // Add a User Description Descriptor (helps identify in some apps)
    BLEDescriptor* pDataDesc = new BLEDescriptor(BLEUUID((uint16_t)0x2901));
    pDataDesc->setValue("URL or 'clear' command");
    pDataCharacteristic->addDescriptor(pDataDesc);

    // --- Start Service & Advertising ---
    pService->start();
    BLEAdvertising *pAdvertising = BLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(SERVICE_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);
    pAdvertising->setMinPreferred(0x12);
    BLEDevice::startAdvertising();

    Serial.println("BLE Service Started, Advertising.");
    Serial.printf("Device Name: %s\n", bleDeviceName);
    Serial.printf("Service UUID: %s\n", SERVICE_UUID);
    Serial.printf("  Data Characteristic: %s (URL or 'clear')\n", DATA_CHARACTERISTIC_UUID);
}

// ===================================================================================
// Show Status Message Function (PARTIAL UPDATE)
// ===================================================================================
void showStatusMessage(const char* message) {
    if (!message || message[0] == '\0') return; // Skip empty messages

    Serial.printf("Display Status (Partial Update): %s\n", message);

    // Set the partial window for the status message area
    display.setPartialWindow(STATUS_AREA_X, STATUS_AREA_Y, STATUS_AREA_WIDTH, STATUS_AREA_HEIGHT);

    display.firstPage();
    do {
        // IMPORTANT for partial update: Fill the partial window area first to clear previous content
        display.fillScreen(GxEPD_WHITE); // This fills only the partial window

        // Use the existing centered text function, but ensure it draws within the partial window bounds
        // Calculate Y position relative to the *display* top, even though drawing happens in partial window
        int16_t x1, y1; uint16_t w, h;
        display.setFont(&FreeSans9pt7b); // Set font *before* getTextBounds
        display.getTextBounds(message, 0, 0, &x1, &y1, &w, &h);

        // Calculate baseline Y pos to roughly center vertically within the status area height
        int baselineY = STATUS_AREA_Y + (STATUS_AREA_HEIGHT / 2) + (h / 2) - 4; // Adjust baseline offset slightly if needed

        // Handle multi-line crude centering if newline exists
        const char* newline = strchr(message, '\n');
         if (newline != NULL) {
             int len = newline - message;
             char line1[len + 1];
             strncpy(line1, message, len);
             line1[len] = '\0';
             const char* line2 = newline + 1;
             // Draw lines relative to the calculated baseline, centered horizontally within the status area
             drawCenteredText(line1, baselineY - h, &FreeSans9pt7b, STATUS_AREA_WIDTH, STATUS_AREA_X);
             drawCenteredText(line2, baselineY + 5, &FreeSans9pt7b, STATUS_AREA_WIDTH, STATUS_AREA_X);
         } else {
            // Single line message, centered horizontally within the status area
            drawCenteredText(message, baselineY, &FreeSans9pt7b, STATUS_AREA_WIDTH, STATUS_AREA_X);
         }

    } while (display.nextPage());

    // IMPORTANT: Hibernation is handled by the main loop after this function returns
}


// ===================================================================================
// Clear Display Function (FULL UPDATE)
// ===================================================================================
void clearDisplay() {
    Serial.println("Executing clearDisplay() (Full Update)...");
    display.setFullWindow(); // Ensure full window context
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE); // Fill entire screen
    } while (display.nextPage());
    Serial.println("Display cleared.");
    // Confirmation message and hibernation handled by the loop
}

// ===================================================================================
// Update QR Display Function (FULL UPDATE)
// ===================================================================================
void updateQrDisplay(const char* textToEncode) {
    Serial.printf("Updating QR display (Full Update) for: '%s'\n", textToEncode);
    display.setFullWindow(); // Ensure full window context
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE); // Clear background fully
        drawQrScreen(textToEncode);      // Draw QR or error message
    } while (display.nextPage());
    // Hibernation handled by the loop
}

// ===================================================================================
// Draw QR Screen Function (Called during FULL UPDATE)
// ===================================================================================
void drawQrScreen(const char* textToEncode) {
    // Attempt QR code generation and drawing (centered on full display)
    bool qrSuccess = drawQrCode(0, 0, display.width(), display.height(), textToEncode);

    if (!qrSuccess) {
        Serial.println("QR Code drawing failed. Displaying error message (Full Update).");
        // Show a generic failure message centered on the full display
        int yPos = display.height() / 2 + 5; // Approx center Y
        drawCenteredText("QR Generation Failed", yPos, &FreeSans9pt7b); // Use full display width for centering
    } else {
       Serial.println("QR Code drawn successfully (Full Update).");
    }
}

// ===================================================================================
// Helper Function: Draw Centered Text (Used by both partial and full updates)
// Target W/X parameters allow specifying the area to center within.
// ===================================================================================
void drawCenteredText(const char *text, int baselineY, const GFXfont *font, int targetW /* = -1 */, int targetX /* = 0 */) {
    if (!font || !text || text[0] == '\0') return;

    int16_t x1, y1; uint16_t w, h;
    display.setFont(font);
    display.setTextColor(GxEPD_BLACK);
    display.setTextSize(1);

    // Get text bounds to calculate width and handle baseline offset (y1)
    display.getTextBounds(text, 0, 0, &x1, &y1, &w, &h);

    // Determine the centering area
    int areaWidth = (targetW == -1) ? display.width() : targetW;
    int areaOriginX = (targetW == -1) ? 0 : targetX;

    // Calculate cursor X position for horizontal centering within the target area
    int cursorX = areaOriginX + ((areaWidth - w) / 2) - x1;

    // Set cursor and print. baselineY is the desired vertical position for the text baseline.
    display.setCursor(cursorX, baselineY);
    display.print(text);
}


// ===================================================================================
// Draw QR Code Function (Used by FULL UPDATE)
// ===================================================================================
bool drawQrCode(int x_target_area, int y_target_area, int w_target_area, int h_target_area, const char *text) {
    // (Keep this function exactly as it was in the previous version)
    // ... (validation, buffer allocation, generation, scaling, drawing) ...
    // 1. Basic Validation
    if (text == NULL || text[0] == '\0') { Serial.println("QR Error: No text provided."); return false; }
    int inputLength = strlen(text);
    if (inputLength > MAX_INPUT_STRING_LENGTH) { Serial.printf("QR Error: Input text too long (%d > %d).\n", inputLength, MAX_INPUT_STRING_LENGTH); return false; }
    Serial.printf("Generating QR Code for: '%s' (Length: %d)\n", text, inputLength);

    // 2. QR Code Buffer (Stack Allocation)
    uint32_t bufferSize = qrcode_getBufferSize(FIXED_QR_VERSION);
    if (bufferSize == 0) { Serial.printf("QR Error: Could not get buffer size for version %d.\n", FIXED_QR_VERSION); return false; }
    uint8_t qrcodeData[bufferSize]; // Ensure stack space!

    // 3. Generate QR Code Data
    QRCode qrcode;
    esp_err_t err = qrcode_initText(&qrcode, qrcodeData, FIXED_QR_VERSION, ECC_LOW, text);
    if (err != ESP_OK) { Serial.printf("QR Error: qrcode_initText failed with error code %d.\n", err); return false; }
    Serial.printf("QR generated successfully: Version=%d, Size=%dx%d modules\n", FIXED_QR_VERSION, qrcode.size, qrcode.size);

    // 4. Calculate Scaling and Positioning
    int qr_modules_size = qrcode.size;
    int module_pixel_size = FIXED_QR_SCALE;
    int final_qr_pixel_size = qr_modules_size * module_pixel_size;
    int total_modules_needed = qr_modules_size + (2 * QR_QUIET_ZONE_MODULES);
    int total_pixel_size_needed = total_modules_needed * module_pixel_size;
    if (total_pixel_size_needed > w_target_area || total_pixel_size_needed > h_target_area) { Serial.printf("QR Error: Scaled QR code (%dpx) too large for target area (%dx%d).\n", total_pixel_size_needed, w_target_area, h_target_area); return false; }
    int x_offset = x_target_area + (w_target_area - final_qr_pixel_size) / 2;
    int y_offset = y_target_area + (h_target_area - final_qr_pixel_size) / 2;
    if (x_offset < 0) x_offset = 0; if (y_offset < 0) y_offset = 0;
    Serial.printf("Drawing QR Code at display offset (%d, %d) with scale %d.\n", x_offset, y_offset, module_pixel_size);

    // 5. Draw the QR Code Modules
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
    return true; // Success!
}