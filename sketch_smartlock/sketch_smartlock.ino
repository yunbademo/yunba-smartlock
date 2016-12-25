#include "SPI.h"
#include <ArduinoJson.h>
#include <LGPRS.h>
#include <LGPRSClient.h>
#include <LGPS.h>
#include <MQTTClient.h>

typedef enum {
  STATUS_INVALID,
  STATUS_INIT_GPRS,
  STATUS_INIT_YUNBA,
  STATUS_IDLE
} STATUS;

STATUS g_status = STATUS_INVALID;

static const char *g_gprs_apn = "3gnet";
static const char *g_gprs_username = "";
static const char *g_gprs_password = "";

static const char *g_appkey = "56a0a88c4407a3cd028ac2fe";
static const char *g_alias = "lock_102030002";
static const char *g_report_topic = "lock_report";

static char g_client_id[64];
static char g_username[32];
static char g_password[32];

static LGPRSClient g_net_client;
static MQTTClient g_mqtt_client;
static StaticJsonBuffer<192> g_json_buf;

static bool get_ip_port(const char *url, char *ip, uint16_t *port) {
  char *p = strstr(url, "tcp://");
  if (p) {
    p += 6;
    char *q = strstr(p, ":");
    if (q) {
      int len = strlen(p) - strlen(q);
      if (len > 0) {
        memcpy(ip, p, len);
        *port = atoi(q + 1);
        Serial.print("ip: ");
        Serial.println(ip);
        Serial.print("port: ");
        Serial.println(*port);
        return true;
      }
    }
  }
  return false;
}

static bool get_host_v2(const char *appkey, char *url) {
  uint8_t buf[256];
  bool rc = false;
  LGPRSClient net_client;
  while (0 == net_client.connect("tick-t.yunba.io", 9977)) {
    Serial.println("reconnect tick");
    delay(500);
  }

  String data = "{\"a\":\"" + String(appkey) + "\",\"n\":\"1\",\"v\":\"v1.0\",\"o\":\"1\"}";
  int json_len = data.length();
  int len;

  buf[0] = 1;
  buf[1] = (uint8_t)((json_len >> 8) & 0xff);
  buf[2] = (uint8_t)(json_len & 0xff);
  len = 3 + json_len;
  memcpy(buf + 3, data.c_str(), json_len);
  net_client.flush();
  net_client.write(buf, len);

  Serial.println("wait data");
  while (!net_client.available()) {
    Serial.println(".");
    delay(100);
  }

  memset(buf, 0, 256);
  int v = net_client.read(buf, 256);
  if (v > 0) {
    len = (uint16_t)(((uint8_t)buf[1] << 8) | (uint8_t)buf[2]);
    if (len == strlen((char *)(buf + 3))) {
      Serial.println((char *)(&buf[3]));
      JsonObject& root = g_json_buf.parseObject((char *)&buf[3]);
      if (!root.success()) {
        Serial.println("parseObject() failed");
        goto exit;
      }
      strcpy(url, root["c"]);
      Serial.println(url);
      rc = true;
    }
  }
exit:
  net_client.stop();
  return rc;
}

static bool setup_with_appkey_and_device_id(const char* appkey, const char *device_id) {
  uint8_t buf[256];
  bool rc = false;
  LGPRSClient net_client;

  while (0 == net_client.connect("reg-t.yunba.io", 9944)) {
    Serial.println("reconnect reg");
    delay(1000);
  }
  delay(100);

  String data;
  if (device_id == NULL)
    data = "{\"a\": \"" + String(appkey) + "\", \"p\":4}";
  else
    data = "{\"a\": \"" + String(appkey) + "\", \"p\":4, \"d\": \"" + String(device_id) + "\"}";
  int json_len = data.length();
  int len;

  buf[0] = 1;
  buf[1] = (uint8_t)((json_len >> 8) & 0xff);
  buf[2] = (uint8_t)(json_len & 0xff);
  len = 3 + json_len;
  memcpy(buf + 3, data.c_str(), json_len);
  net_client.flush();
  net_client.write(buf, len);

  Serial.println("wait data");
  while (!net_client.available()) {
    Serial.println(".");
    delay(100);
  }

  memset(buf, 0, 256);
  int v = net_client.read(buf, 256);
  if (v > 0) {
    len = (uint16_t)(((uint8_t)buf[1] << 8) | (uint8_t)buf[2]);
    if (len == strlen((char *)(buf + 3))) {
      Serial.println((char *)(&buf[3]));
      JsonObject& root = g_json_buf.parseObject((char *)&buf[3]);
      if (!root.success()) {
        Serial.println("parseObject() failed");
        net_client.stop();
        return false;
      }
      strcpy(g_username, root["u"]);
      strcpy(g_password, root["p"]);
      strcpy(g_client_id, root["c"]);
      rc = true;
    }
  }

  net_client.stop();
  return rc;
}

static void set_alias(const char *alias) {
  g_mqtt_client.publish(",yali", alias);
}

static void init_gprs() {
  Serial.println("attaching gprs");
  while (!LGPRS.attachGPRS(g_gprs_apn, g_gprs_username, g_gprs_password)) {
    Serial.println(".");
    delay(1000);
  }
  Serial.println("gprs ok");
  LGPS.powerOn();
  delay(2000);
  g_status = STATUS_INIT_YUNBA;
}

static void init_yunba() {
  char url[32] = {0};
  char ip[32] = {0};
  uint16_t port = 0;

  Serial.println("get_host_v2");
  get_host_v2(g_appkey, url);
  Serial.println("setup_with_appkey_and_device_id");
  setup_with_appkey_and_device_id(g_appkey, g_alias);
  Serial.println("get_ip_port");
  get_ip_port(url, ip, &port);
  Serial.println("mqtt begin");
  g_mqtt_client.begin("182.92.180.87", 1883, g_net_client);

  Serial.println("mqtt connect");
  while (!g_mqtt_client.connect(g_client_id, g_username, g_password)) {
    Serial.println(".");
    LGPRS.attachGPRS(g_gprs_apn, g_gprs_username, g_gprs_password);
  }

  Serial.println("yunba ok");
  set_alias(g_alias);

  g_status = STATUS_IDLE;
}

/* messageReceived and extMessageReceived must be defined with no 'static', because MQTTClient use them */
void messageReceived(String topic, String payload, char * bytes, unsigned int length) {
  Serial.println("message:");
  Serial.print(topic);
  Serial.print(" - ");
  Serial.print(payload);
}

void extMessageReceived(EXTED_CMD cmd, int status, String payload, unsigned int length) {
  Serial.println("ext message:");
  Serial.print(cmd);
  Serial.print(" - ");
  Serial.print(payload);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  //  delay(10000);
  Serial.println("setup...");

  g_status = STATUS_INIT_GPRS;
}

void loop() {
  // put your main code here, to run repeatedly:
//  Serial.println("loop...");
  switch (g_status) {
    case STATUS_INIT_GPRS:
      init_gprs();
      break;
    case STATUS_INIT_YUNBA:
      init_yunba();
      break;
    case STATUS_IDLE:
      //      g_mqtt_client.loop();
      break;
    default:
      Serial.print("unknown status:");
      Serial.println(g_status);
      break;
  }
  delay(100);
}

