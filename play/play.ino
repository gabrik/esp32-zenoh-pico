#include <Arduino.h>
#include <WiFi.h>

extern "C" {
    #include "zenoh-pico/include/zenoh-pico.h"
}

#define SSID "WIFI"
#define PASS "PWD"

// Zenoh-specific parameters
#define MODE "client"
#define PEER "tcp/192.168.1.168:7447"
#define URI "/demo/example/zenoh-pico-esp32"

zn_session_t *s = NULL;
zn_reskey_t *reskey = NULL;

void setup()
{


    // Set WiFi in STA mode and trigger attachment
    WiFi.mode(WIFI_STA);
    WiFi.begin(SSID, PASS);

    // Keep trying until connected
    while (WiFi.status() != WL_CONNECTED)
    { }
    delay(1000);

    zn_properties_t *config = zn_config_default();
    zn_properties_insert(config, ZN_CONFIG_MODE_KEY, z_string_make(MODE));
    zn_properties_insert(config, ZN_CONFIG_PEER_KEY, z_string_make(PEER));

    s = zn_open(config);
    if (s == NULL)
    {
        return;
    }

    znp_start_read_task(s);
    znp_start_lease_task(s);

    unsigned long rid = zn_declare_resource(s, zn_rname(URI));
    reskey = (zn_reskey_t*)malloc(sizeof(zn_reskey_t));
    *reskey = zn_rid(rid);

    zn_publisher_t *pub = zn_declare_publisher(s, *reskey);
    if (pub == NULL) {
        return;
    }
}

void loop()
{
    delay(5000);
    if (s == NULL)
        return;

    if (reskey == NULL)
        return;

    char *buf = "Publishing data from ESP32";
    zn_write(s, *reskey, (const uint8_t *)buf, strlen(buf));
}

