#include "display_ui.h"

#include <Arduino.h>
#include <esp_adc_cal.h>
#include <esp_heap_caps.h>

#include "cleanImage.h"
#include "cleaning_history.h"
#include "dirtyImage.h"
#include "epd_driver.h"
#include "opensans12b.h"
#include "opensans24b.h"
#include "utilities.h"

namespace {

enum DisplayMode { DisplayDirty, DisplayClean };

uint8_t *framebuffer = nullptr;
int adcVref = 1100;
DisplayMode currentMode = DisplayDirty;

float readBatteryVoltage()
{
    uint16_t value = analogRead(BATT_PIN);
    float voltage = ((float)value / 4095.0) * 2.0 * 3.3 *
                    (adcVref / 1000.0);
    if (voltage >= 4.2) voltage = 4.2;
    return voltage;
}

void drawBatteryWarning()
{
    constexpr float LowBatteryThreshold = 3.6;
    float voltage = readBatteryVoltage();
    if (voltage >= LowBatteryThreshold) return;

    char warningText[32];
    snprintf(warningText, sizeof(warningText),
             "low battery (%.1fv)", voltage);

    FontProperties props = {
        .fg_color = 0,
        .bg_color = 15,
        .fallback_glyph = 0,
        .flags = 0
    };

    int drawX = EPD_WIDTH - 390;  // Keep clear of the history panel.
    int drawY = 44;
    write_mode((GFXfont *)&OpenSans12B, warningText, &drawX, &drawY,
               framebuffer, BLACK_ON_WHITE, &props);
}

void drawCenteredText(const GFXfont *font, const char *text,
                      int boxX, int boxY, int boxWidth, int boxHeight,
                      const FontProperties *props)
{
    int32_t cursorX = 0;
    int32_t minX = 0;
    int32_t maxX = 0;
    int32_t minY = 100000;
    int32_t maxY = -100000;

    // The driver's get_text_bounds() reports vertical bounds in the opposite
    // direction from write_mode(). Calculate the actual drawn pixels from the
    // glyph metrics so the baseline lands in the visual center of the box.
    for (const char *character = text; *character != '\0'; ++character) {
        GFXglyph *glyph = nullptr;
        get_glyph(font, static_cast<uint8_t>(*character), &glyph);
        if (!glyph) continue;

        minX = min(minX, cursorX + glyph->left);
        maxX = max(maxX, cursorX + glyph->left + glyph->width);
        minY = min(minY, -glyph->top);
        maxY = max(maxY, -glyph->top + glyph->height);
        cursorX += glyph->advance_x;
    }

    if (minY > maxY) return;

    int textWidth = maxX - minX;
    int textHeight = maxY - minY;
    int drawX = boxX + (boxWidth - textWidth) / 2 - minX;
    int drawY = boxY + (boxHeight - textHeight) / 2 - minY;
    write_mode(font, text, &drawX, &drawY,
               framebuffer, BLACK_ON_WHITE, props);
}

const uint8_t *smallGlyph(char character)
{
    static const uint8_t A[] = {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11};
    static const uint8_t D[] = {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E};
    static const uint8_t F[] = {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10};
    static const uint8_t M[] = {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11};
    static const uint8_t S[] = {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t T[] = {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04};
    static const uint8_t W[] = {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11};
    static const uint8_t a[] = {0x00, 0x0E, 0x01, 0x0F, 0x11, 0x13, 0x0D};
    static const uint8_t d[] = {0x01, 0x01, 0x0D, 0x13, 0x11, 0x13, 0x0D};
    static const uint8_t e[] = {0x00, 0x0E, 0x11, 0x1F, 0x10, 0x11, 0x0E};
    static const uint8_t h[] = {0x10, 0x10, 0x16, 0x19, 0x11, 0x11, 0x11};
    static const uint8_t i[] = {0x04, 0x00, 0x0C, 0x04, 0x04, 0x04, 0x0E};
    static const uint8_t n[] = {0x00, 0x16, 0x19, 0x11, 0x11, 0x11, 0x11};
    static const uint8_t o[] = {0x00, 0x0E, 0x11, 0x11, 0x11, 0x11, 0x0E};
    static const uint8_t r[] = {0x00, 0x16, 0x19, 0x10, 0x10, 0x10, 0x10};
    static const uint8_t s[] = {0x00, 0x0F, 0x10, 0x0E, 0x01, 0x01, 0x1E};
    static const uint8_t t[] = {0x04, 0x04, 0x0E, 0x04, 0x04, 0x05, 0x02};
    static const uint8_t u[] = {0x00, 0x11, 0x11, 0x11, 0x11, 0x13, 0x0D};
    static const uint8_t y[] = {0x00, 0x11, 0x11, 0x0F, 0x01, 0x11, 0x0E};
    static const uint8_t unknown[] = {0x0E, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04};

    switch (character) {
        case 'A': return A;
        case 'D': return D;
        case 'F': return F;
        case 'M': return M;
        case 'S': return S;
        case 'T': return T;
        case 'W': return W;
        case 'a': return a;
        case 'd': return d;
        case 'e': return e;
        case 'h': return h;
        case 'i': return i;
        case 'n': return n;
        case 'o': return o;
        case 'r': return r;
        case 's': return s;
        case 't': return t;
        case 'u': return u;
        case 'y': return y;
        default:  return unknown;
    }
}

void drawSmallTextVerticallyCentered(const char *text, int x,
                                     int boxY, int boxHeight)
{
    constexpr int GlyphWidth = 5;
    constexpr int GlyphHeight = 7;
    constexpr int Scale = 2;
    constexpr int LetterSpacing = 1;
    int y = boxY + (boxHeight - GlyphHeight * Scale) / 2;

    for (const char *character = text; *character != '\0'; ++character) {
        const uint8_t *glyph = smallGlyph(*character);
        for (int row = 0; row < GlyphHeight; ++row) {
            for (int column = 0; column < GlyphWidth; ++column) {
                if (glyph[row] & (1 << (GlyphWidth - column - 1))) {
                    epd_fill_rect(x + column * Scale, y + row * Scale,
                                  Scale, Scale, 0x00, framebuffer);
                }
            }
        }
        x += GlyphWidth * Scale + LetterSpacing;
    }
}

void drawCleaningHistory()
{
    constexpr int PanelX = 772;
    constexpr int ContentX = PanelX + 17;
    constexpr int DayLabelX = ContentX;
    constexpr int RowTop = 52;
    constexpr int RowHeight = 55;
    constexpr int BoxX = 895;
    constexpr int BoxWidth = 54;
    constexpr int BoxHeight = 38;

    epd_fill_rect(PanelX, 0, EPD_WIDTH - PanelX, EPD_HEIGHT,
                  0xFF, framebuffer);

    FontProperties props = {
        .fg_color = 0,
        .bg_color = 15,
        .fallback_glyph = 0,
        .flags = 0
    };
    GFXfont *font = (GFXfont *)&OpenSans12B;

    int titleX = ContentX;
    int titleY = 44;
    write_mode(font, "who scooped", &titleX, &titleY,
               framebuffer, BLACK_ON_WHITE, &props);

    const CleaningHistory::Day *days = CleaningHistory::days();
    bool historyValid = CleaningHistory::isValid();

    for (size_t i = 0; i < CleaningHistory::DayCount; ++i) {
        int y = RowTop + static_cast<int>(i) * RowHeight;
        int boxY = y + 2;

        if (historyValid) {
            drawSmallTextVerticallyCentered(days[i].label, DayLabelX,
                                            boxY, BoxHeight);
        }

        epd_draw_rect(BoxX, boxY, BoxWidth, BoxHeight,
                      0x00, framebuffer);

        const char *marker = "";
        if (historyValid) {
            switch (days[i].cleanerMask) {
                case CleaningHistory::Oriana:  marker = "O"; break;
                case CleaningHistory::Matthew: marker = "M"; break;
                case CleaningHistory::Both:    marker = "MO"; break;
                default:                       marker = ""; break;
            }
        }

        if (marker[0] != '\0') {
            drawCenteredText(font, marker, BoxX, boxY,
                             BoxWidth, BoxHeight, &props);
        }
    }
}

const char *weekdayShort(int weekday)
{
    static const char *days[] = {
        "Sun", "Mon", "Tues", "Wed", "Thurs", "Fri", "Sat"
    };
    if (weekday < 0 || weekday > 6) return "???";
    return days[weekday];
}

void formatCleanStamp(char *labelLine, size_t labelLineSize,
                      char *timeLine, size_t timeLineSize,
                      char *dateLine, size_t dateLineSize,
                      const struct tm *timeInfo,
                      bool showReminderMarker)
{
    int hour12 = timeInfo->tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    const char *ampm = (timeInfo->tm_hour >= 12) ? "pm" : "am";

    snprintf(labelLine, labelLineSize,
             showReminderMarker ? "last scooped !" : "last scooped");
    snprintf(timeLine, timeLineSize,
             "%d:%02d%s", hour12, timeInfo->tm_min, ampm);
    snprintf(dateLine, dateLineSize, "%s %d/%d",
             weekdayShort(timeInfo->tm_wday),
             timeInfo->tm_mon + 1, timeInfo->tm_mday);
}

void drawLastCleanStamp(const DisplayUi::State &state)
{
    if (!state.lastCleanTimestampValid) return;

    bool showReminder = currentMode == DisplayDirty && state.reminderOverdue;
    char labelLine[40];
    char timeLine[40];
    char dateLine[24];
    formatCleanStamp(labelLine, sizeof(labelLine),
                     timeLine, sizeof(timeLine),
                     dateLine, sizeof(dateLine),
                     &state.lastCleanTime, showReminder);

    FontProperties props = {
        .fg_color = 0,
        .bg_color = 15,
        .fallback_glyph = 0,
        .flags = 0
    };

    int labelX = 14;
    int labelY = 44;
    write_mode((GFXfont *)&OpenSans12B, labelLine, &labelX, &labelY,
               framebuffer, BLACK_ON_WHITE, &props);

    int timeX = 14;
    int timeY = 99;
    write_mode((GFXfont *)&OpenSans24B, timeLine, &timeX, &timeY,
               framebuffer, BLACK_ON_WHITE, &props);

    int dateX = 14;
    int dateY = 139;
    write_mode((GFXfont *)&OpenSans12B, dateLine, &dateX, &dateY,
               framebuffer, BLACK_ON_WHITE, &props);
}

void copySelectedImageToFramebuffer(const DisplayUi::State &state)
{
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    bool dirty = currentMode == DisplayDirty;
    Rect_t area = {
        .x = 0,
        .y = 0,
        .width = static_cast<int32_t>(
            dirty ? dirty_image_width : clean_image_width),
        .height = static_cast<int32_t>(
            dirty ? dirty_image_height : clean_image_height)
    };

    epd_copy_to_framebuffer(
        area,
        dirty ? (uint8_t *)dirty_image_data : (uint8_t *)clean_image_data,
        framebuffer
    );

    drawLastCleanStamp(state);
    drawCleaningHistory();
    drawBatteryWarning();
}

void refreshDisplay(const DisplayUi::State &state)
{
    copySelectedImageToFramebuffer(state);
    epd_poweron();
    epd_clear();
    yield();
    uint32_t start = millis();
    epd_draw_grayscale_image(epd_full_screen(), framebuffer);
    Serial.printf("EPD draw took %lums\r\n", millis() - start);
    epd_poweroff();
    yield();
}

}  // namespace

namespace DisplayUi {

bool initialise()
{
    epd_init();
    framebuffer = (uint8_t *)heap_caps_malloc(
        EPD_WIDTH * EPD_HEIGHT / 2, MALLOC_CAP_SPIRAM
    );
    if (!framebuffer) {
        Serial.println("alloc memory failed !!!");
        return false;
    }
    memset(framebuffer, 0xFF, EPD_WIDTH * EPD_HEIGHT / 2);

    esp_adc_cal_characteristics_t adcCharacteristics;
    esp_adc_cal_value_t calibrationType = esp_adc_cal_characterize(
        ADC_UNIT_1,
        ADC_ATTEN_DB_12,
        ADC_WIDTH_BIT_12,
        1100,
        &adcCharacteristics
    );
    if (calibrationType == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        Serial.printf("eFuse Vref: %umV\r\n", adcCharacteristics.vref);
        adcVref = adcCharacteristics.vref;
    }

    return true;
}

void showDirty(const State &state)
{
    currentMode = DisplayDirty;
    Serial.println("Rendering DIRTY image");
    refreshDisplay(state);
}

void showClean(const State &state)
{
    currentMode = DisplayClean;
    Serial.println("Rendering CLEAN image");
    refreshDisplay(state);
}

}  // namespace DisplayUi
