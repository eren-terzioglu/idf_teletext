#pragma once

/**
 * Start the background task that provides the pages: show the set saved in SPIFFS (if any),
 * then connect to Wi-Fi, download the ČT page index and every picture, show them and save
 * them for the next boot. Repeats every CONFIG_TELETEXT_REFRESH_MINUTES. Without a configured
 * SSID only the saved set is shown. Call after teletext_ui_init().
 */
void teletext_online_start(void);
