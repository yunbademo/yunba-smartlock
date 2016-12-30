#include <ArduinoJson.h>
#include <vmcell.h>
#include <LGPRS.h>
#include <LGPRSClient.h>
#include <LBattery.h>
#include <LGPS.h>
#include <LTask.h>
#include <MQTTClient.h>

#define JSON_BUF_SIZE 1024

#define PIN_MOTOR 0
#define PIN_LOCK_STATUS 5
#define PIN_LOCK_STEP 6
#define PIN_BUZZER 7

typedef enum {
  STATUS_INVALID,
  STATUS_INIT_GPRS,
  STATUS_INIT_YUNBA,
  STATUS_IDLE,
} STATUS;

typedef enum {
  LOCK_LOCKED,
  LOCK_LOCKING,
  LOCK_UNLOCKED,
  LOCK_UNLOCKING
} LOCK;

static STATUS g_status = STATUS_INVALID;
static LOCK g_lock_status = LOCK_LOCKED;
static int g_lock_unlock_step = 0;
static unsigned long g_unlocked_ms = 0;

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

static bool g_report_lock = true;
static bool g_report_other = true;

static gpsSentenceInfoStruct g_gps_info;
static unsigned long g_last_report_ms = 0;
static unsigned long g_buzzer_on_ms = 0;
static uint32_t g_buzzer_duration = 0;
static bool g_buzzer_on = false;

static unsigned long g_check_net_ms = 0;

/*
  Wrapper to the low-level function vm_cell_open()
  Initialize the Cell register on LinkIt ONE
*/
static boolean cell_open(void *userData) {
  *(VMINT *)userData = vm_cell_open();
  return TRUE;
}

/*
  Wrapper to the low-level function vm_cell_get_cur_cell_info()
  Get the current cell info. Result is stored into a struct
*/
static boolean get_current_cell_info(void *userData) {
  *(vm_cell_info_struct **)userData = vm_cell_get_cur_cell_info();
  return TRUE;
}

/*
  Wrapper to the low-level function vm_cell_get_nbr_num()
  Get the number of neighbor cells
*/
static boolean get_neighbor_cell_number(void *userData) {
  *(VMINT **)userData = vm_cell_get_nbr_num();
  return TRUE;
}

/*
  Wrapper to the low-level function vm_cell_get_nbr_cell_info()
  Get the neighbor cells info. Result is stored into a struct array
*/
static boolean get_neighbor_cell_info(void *userData) {
  *(vm_cell_info_struct***) userData = vm_cell_get_nbr_cell_info();
  return TRUE;
}

static inline void start_motor() {
  digitalWrite(PIN_MOTOR, LOW);
}

static inline void stop_motor() {
  digitalWrite(PIN_MOTOR, HIGH);
}

static inline void start_buzzer() {
  g_buzzer_on = true;
  digitalWrite(PIN_BUZZER, HIGH);
}

static inline void stop_buzzer() {
  digitalWrite(PIN_BUZZER, LOW);
  g_buzzer_on = false;
}

static inline bool lock_status_on() {
  if (digitalRead(PIN_LOCK_STATUS) == LOW) {
    return true;
  } else {
    return false;
  }
}

static inline bool lock_step_on() {
  if (digitalRead(PIN_LOCK_STEP) == LOW) {
    return true;
  } else {
    return false;
  }
}

static void buzzer(uint32_t duration) {
  g_buzzer_on_ms = millis();
  g_buzzer_duration = duration;
  start_buzzer();
}

static void sync_buzzer(uint32_t duration) {
  start_buzzer();
  delay(duration);
  stop_buzzer();
}

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
        Serial.println("ip: " + String(ip) + ", port: " + String(*port));
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
  Serial.println("connect tick");
  while (!net_client.connect("tick-t.yunba.io", 9977)) {
    Serial.println(".");
    delay(1000);
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
    delay(200);
  }

  memset(buf, 0, 256);
  int v = net_client.read(buf, 256);
  if (v > 0) {
    len = (uint16_t)(((uint8_t)buf[1] << 8) | (uint8_t)buf[2]);
    if (len == strlen((char *)(buf + 3))) {
      Serial.println((char *)(&buf[3]));
      StaticJsonBuffer<JSON_BUF_SIZE> json_buf;
      JsonObject& root = json_buf.parseObject((char *)&buf[3]);
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

  Serial.println("connect reg");
  while (!net_client.connect("reg-t.yunba.io", 9944)) {
    Serial.println(".");
    delay(1000);
  }

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
    delay(200);
  }

  memset(buf, 0, 256);
  int v = net_client.read(buf, 256);
  if (v > 0) {
    len = (uint16_t)(((uint8_t)buf[1] << 8) | (uint8_t)buf[2]);
    if (len == strlen((char *)(buf + 3))) {
      Serial.println((char *)(&buf[3]));
      StaticJsonBuffer<JSON_BUF_SIZE> json_buf;
      JsonObject& root = json_buf.parseObject((char *)&buf[3]);
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
  Serial.println("vm_cell_open");
  VMINT rc;
  do {
    LTask.remoteCall(&cell_open, (void *)&rc);
    Serial.println(".");
    delay(1000);
  } while (rc != VM_CELL_OPEN_SUCCESS && rc != VM_CELL_OPEN_ALREADY_OPEN);

  Serial.println("attaching gprs");
  while (!LGPRS.attachGPRS(g_gprs_apn, g_gprs_username, g_gprs_password)) {
    Serial.println(".");
    delay(1000);
  }
  Serial.println("gprs ok");
  LGPS.powerOn();
  delay(100);
  sync_buzzer(400);
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
  g_mqtt_client.begin(ip, 1883, g_net_client);

  Serial.println("mqtt connect");
  while (!g_mqtt_client.connect(g_client_id, g_username, g_password)) {
    Serial.println(".");
    LGPRS.attachGPRS(g_gprs_apn, g_gprs_username, g_gprs_password);
    delay(200);
  }

  Serial.println("yunba ok");
  sync_buzzer(200);
  set_alias(g_alias);

  g_status = STATUS_IDLE;
}

static void check_need_report() {
  if (millis() - g_last_report_ms > 240000) {
    g_last_report_ms = millis();
    g_report_other = true;
  }
}

static void add_cell_info(JsonArray& json_array, vm_cell_info_struct *info) {
  JsonObject& cell = json_array.createNestedObject();
  cell["mcc"] = info->mcc;
  cell["mnc"] = info->mnc;
  cell["lac"] = info->lac;
  cell["ci"] = info->ci;
  cell["sig"] = info->rxlev;
}

static void handle_report() {
  if (!g_report_lock && !g_report_other) {
    return;
  }

  StaticJsonBuffer<JSON_BUF_SIZE> json_buf;
  JsonObject& root = json_buf.createObject();

  if (g_report_lock) {
    root["lock"] = (g_lock_status == LOCK_LOCKED);
    g_report_lock = false;
  }

  if (g_report_other) {
    LGPS.getData(&g_gps_info);
    String gps = String((char *)g_gps_info.GPGGA);
    gps.trim();
    root["gps"] = gps;
    root["battery"] = LBattery.level();
    root["charge"] = (LBattery.isCharging() == true);

    VMINT *neighbor_cell_number;
    vm_cell_info_struct *current_cell_ptr;
    vm_cell_info_struct **neighbor_cell_ptr;
    LTask.remoteCall(&get_current_cell_info, (void *)&current_cell_ptr);
    LTask.remoteCall(&get_neighbor_cell_number, (void *)&neighbor_cell_number);
    LTask.remoteCall(&get_neighbor_cell_info, (void *)&neighbor_cell_ptr);

    JsonArray& json_array = root.createNestedArray("cell");
    add_cell_info(json_array, current_cell_ptr);
    if (neighbor_cell_number && *neighbor_cell_number > 0) {
      for (int i = 0; i < *neighbor_cell_number && i < 4; i++) {
        add_cell_info(json_array, neighbor_cell_ptr[i]);
      }
    }

    g_report_other = false;
  }

  String json;
  root.printTo(json);

  Serial.println("publish: " + json);
  g_mqtt_client.publish(g_report_topic, json);
  g_report_lock = false;
}

static void unlock() {
  if (g_lock_status != LOCK_LOCKED) {
    Serial.println("not locked now");
    return;
  }

  g_lock_status = LOCK_UNLOCKING;
  g_lock_unlock_step = 0;
  start_motor();
  Serial.println("unlocking");
}

static void handle_msg(String &msg) {
  StaticJsonBuffer<JSON_BUF_SIZE> json_buf;
  JsonObject& root = json_buf.parseObject(msg);
  if (!root.success()) {
    Serial.println("parse json failed");
    return;
  }

  String cmd = root["cmd"];
  if (cmd == "unlock") {
    unlock();
  } else if (cmd == "report") {
    g_report_lock = true;
    g_report_other = true;
  } else if (cmd == "buzzer") {
    buzzer(1000);
  }
}

static void handle_lock() {
  switch (g_lock_status) {
    case LOCK_UNLOCKING:
      if (!lock_step_on()) {
        if (g_lock_unlock_step == 0) {
          g_lock_unlock_step = 1;
        }
      } else {
        if (g_lock_unlock_step == 1) {
          stop_motor();
          g_report_lock = true;
          g_lock_status = LOCK_UNLOCKED;
          buzzer(200);
          g_unlocked_ms = millis();
          Serial.println("unlocked");
        }
      }
      break;
    case LOCK_LOCKING:
      if (!lock_step_on()) {
        if (g_lock_unlock_step == 0) {
          g_lock_unlock_step = 1;
        }
      } else {
        if (g_lock_unlock_step == 1) {
          stop_motor();
          g_report_lock = true;
          g_lock_status = LOCK_LOCKED;
          buzzer(400);
          Serial.println("locked");
        }
      }
      break;
    case LOCK_UNLOCKED:
      if (millis() - g_unlocked_ms < 400) {
        break;
      }
      if (lock_status_on()) {
        g_lock_status = LOCK_LOCKING;
        g_lock_unlock_step = 0;
        start_motor();
        Serial.println("locking");
      }
      break;
    default:
      break;
  }
}

static void check_network() {
  if (millis() - g_check_net_ms > 30000) {
    if (!g_mqtt_client.connected()) {
      Serial.println("mqtt connection failed, try reconnect");
      LGPS.powerOff();
      sync_buzzer(2000);
      g_status = STATUS_INIT_YUNBA;
    } else {
      Serial.println("mqtt connection is ok");
    }
    g_check_net_ms = millis();
  }
}

/* messageReceived and extMessageReceived must be defined with no 'static', because MQTTClient use them */
void messageReceived(String topic, String payload, char *bytes, unsigned int length) {
  Serial.println("msg: " + topic + ", " + payload);
  handle_msg(payload);
}

void extMessageReceived(EXTED_CMD cmd, int status, String payload, unsigned int length) {
  //  Serial.println("ext msg: " + String(cmd) + ", " + payload);
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(9600);
  //  delay(10000);
  Serial.println("setup...");

  pinMode(PIN_MOTOR, OUTPUT);
  digitalWrite(PIN_MOTOR, HIGH);
  pinMode(PIN_LOCK_STATUS, INPUT);
  pinMode(PIN_LOCK_STEP, INPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  digitalWrite(PIN_BUZZER, LOW);

  if (lock_status_on()) {
    g_lock_status = LOCK_LOCKED;
  } else {
    g_lock_status = LOCK_UNLOCKED;
  }
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
      g_mqtt_client.loop();
      check_need_report();
      handle_report();
      handle_lock();
      check_network();
      break;
    default:
      Serial.println("unknown status: " + g_status);
      break;
  }

  if (g_buzzer_on && (millis() - g_buzzer_on_ms > g_buzzer_duration)) {
    stop_buzzer();
  }
  delay(20);
}

