// Harbuz — main.
// Dedicated firmware (Keira multiboot app): display, buttons, SD, browser UI.
#include <Arduino.h>
#include "fw/hw.h"
#include "fw/font.h"
#include "fw/ui.h"

// "Guest" OTA mode: mark the app as valid so the ESP-IDF auto-rollback mechanism
// never reverts the boot slot (we are not the primary/OS firmware).
extern "C" bool verifyRollbackLater() { return true; }

Arduino_GFX* g_disp = nullptr;
Arduino_Canvas* g_buf = nullptr;
Buttons g_btns;
ui::Browser g_browser;

void setup() {
    Serial.begin(115200);
    vTaskDelay(pdMS_TO_TICKS(300));

    Serial.println("[browser] display...");
    g_disp = createDisplay();
    g_buf = new Arduino_Canvas(g_disp->width(), g_disp->height(), g_disp, 0, 0, 0);
    if (!g_buf->begin(GFX_SKIP_OUTPUT_BEGIN)) {
        Serial.println("ERROR: canvas alloc failed");
        while (1) vTaskDelay(1000);
    }
    pinMode(hw::PIN_DISP_BL, OUTPUT);
    digitalWrite(hw::PIN_DISP_BL, HIGH);
    font::begin(g_buf);

    g_btns.begin();
    g_browser.setButtons(&g_btns);
    g_browser.begin(g_buf);

    Serial.printf("[browser] ok: %dx%d, heap %lu / PSRAM %lu\n",
                  g_disp->width(), g_disp->height(),
                  (unsigned long)ESP.getFreeHeap(),
                  (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // initial frame
    if (g_browser.loop()) g_buf->flush();
}

void loop() {
    while (Serial.available()) {
        char ch = (char)Serial.read();
        static std::string cmd;
        if (ch == '\n' || ch == '\r') {
            if (!cmd.empty()) g_browser.serialCmd(cmd);
            cmd.clear();
        } else {
            cmd += ch;
        }
    }
    if (g_browser.loop()) g_buf->flush();
    delay(15);
}
