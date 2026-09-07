#pragma once

#include "sdkconfig.h"

// Project Metadata
#define APP_NAME                    "VorotaBot"
#define APP_VERSION                 "1.0.0"

// Wi-Fi & Portal Defaults
#ifndef CONFIG_VOROTABOT_WIFI_SSID
#define CONFIG_VOROTABOT_WIFI_SSID  "VorotaBot-AP"
#endif

#ifndef CONFIG_VOROTABOT_WIFI_PASSWORD
#define CONFIG_VOROTABOT_WIFI_PASSWORD "12345678"
#endif

#ifndef CONFIG_VOROTABOT_PORTAL_DOMAIN
#define CONFIG_VOROTABOT_PORTAL_DOMAIN "vorota.local"
#endif

// AWS Route 53 Defaults
#ifndef CONFIG_VOROTABOT_ROOT_DOMAIN
#define CONFIG_VOROTABOT_ROOT_DOMAIN "glebos.click"
#endif

#ifndef CONFIG_VOROTABOT_FQDN
#define CONFIG_VOROTABOT_FQDN       "vorota.glebos.click"
#endif
