#pragma once

#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

namespace debug_tab {
    enum {
        wifi = 0,
        disk,
        mood,
        chat,
        count // keep last
    };
}

namespace sd_info_rows {
enum {
    inserted = 0,
    name,
    capacity,
    mount,

    count // keep last
};
}

namespace wifi_info_rows {
enum {
    ssid = 0,
    password,

    count // keep last
};
}

void screen_debug_init();
void screen_debug_loop();

#ifdef __cplusplus
}
#endif
