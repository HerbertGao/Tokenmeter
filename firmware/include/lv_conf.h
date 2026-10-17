#pragma once
#define LV_CONF_H
#define LV_COLOR_DEPTH 16
#define LV_USE_LOG 0
#define LV_USE_OS LV_OS_NONE
#define LV_USE_STDLIB_MALLOC LV_STDLIB_CLIB
#define LV_USE_SYSMON 0
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0
#define LV_FONT_MONTSERRAT_14 0
#define LV_FONT_DEFAULT &font_16
#ifndef __ASSEMBLER__
struct lv_font_t;
#ifdef __cplusplus
extern "C" {
#endif
extern const struct lv_font_t font_16;
#ifdef __cplusplus
}
#endif
#endif
