/*
 * Definitions cppcheck needs to parse the firmware without the ESP-IDF/LVGL headers
 * (string-literal macros used in concatenations). Used only by the Tests lint job.
 */
#define IPSTR "%d.%d.%d.%d"
#define IP2STR(addr) 0, 0, 0, 0
#define LV_SYMBOL_CLOSE "x"
#define LV_SYMBOL_OK "x"
#define LV_SYMBOL_WIFI "x"
#define LV_SYMBOL_RIGHT "x"
#define LV_SYMBOL_LEFT "x"
#define LV_SYMBOL_EYE_OPEN "x"
#define LV_SYMBOL_EYE_CLOSE "x"
#define LV_SYMBOL_MUTE "x"
#define LV_SYMBOL_VOLUME_MAX "x"
#define LV_SYMBOL_VOLUME_MID "x"
