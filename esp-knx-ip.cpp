/**
 * esp-knx-ip library for KNX/IP communication on an ESP8266
 * Author: Nico Weichbrodt <envy>
 * License: MIT
 */

#include "esp-knx-ip.h"

ESPKNXIP::ESPKNXIP() : server(nullptr), registered_callback_assignments(0), registered_callbacks(0), registered_configs(0), registered_feedbacks(0)
{
  DEBUG_PRINTLN();
  DEBUG_PRINTLN("ESPKNXIP starting up");
  // Default physical address is 1.1.0
  physaddr.bytes.high = (/*area*/1 << 4) | /*line*/1;
  physaddr.bytes.low = /*member*/0;
  memset(callback_assignments, 0, MAX_CALLBACK_ASSIGNMENTS * sizeof(callback_assignment_t));
  memset(callbacks, 0, MAX_CALLBACKS * sizeof(callback_fptr_t));
  memset(custom_config_data, 0, MAX_CONFIG_SPACE * sizeof(uint8_t));
  memset(custom_config_default_data, 0, MAX_CONFIG_SPACE * sizeof(uint8_t));
  memset(custom_configs, 0, MAX_CONFIGS * sizeof(config_t));
}

void ESPKNXIP::load()
{
  memcpy(custom_config_default_data, custom_config_data, MAX_CONFIG_SPACE);
  EEPROM.begin(EEPROM_SIZE);
  restore_from_eeprom();
}

// GGS67: added withPrefix paramweter to allow config free prefix decision from user code
void ESPKNXIP::start(ESP8266WebServer *srv, bool withPrefix)
{
  server = srv;
  __start(withPrefix);
}

void ESPKNXIP::start(bool withPrefix)
{
  server = new ESP8266WebServer(80);
  __start(withPrefix);
}

void ESPKNXIP::__start(bool withPrefix)
{
	__withPrefix = withPrefix;
  if (server != nullptr)
  {
    if(withPrefix) server->on(ROOT_PREFIX, [this](){
      __handle_root();
    });
    
    server->on(KNX_WEB_PATH(__ROOT_PATH), [this](){
      __handle_root();
    });
    server->on(KNX_WEB_PATH(__REGISTER_PATH), [this](){
      __handle_register();
    });
    server->on(KNX_WEB_PATH(__DELETE_PATH), [this](){
      __handle_delete();
    });
    server->on(KNX_WEB_PATH(__PHYS_PATH), [this](){
      __handle_set();
    });
#if !DISABLE_EEPROM_BUTTONS
    server->on(KNX_WEB_PATH(__EEPROM_PATH), [this](){
      __handle_eeprom();
    });
#endif
    server->on(KNX_WEB_PATH(__CONFIG_PATH), [this](){
      __handle_config();
    });
    server->on(KNX_WEB_PATH(__FEEDBACK_PATH), [this](){
      __handle_feedback();
    });
#if !DISABLE_RESTORE_BUTTON
    server->on(KNX_WEB_PATH(__RESTORE_PATH), [this](){
      __handle_restore();
    });
#endif
#if !DISABLE_REBOOT_BUTTON
    server->on(KNX_WEB_PATH(__REBOOT_PATH), [this](){
      __handle_reboot();
    });
#endif
    server->begin();
  }

  udp.beginMulticast(WiFi.localIP(),  MULTICAST_IP, MULTICAST_PORT);
}

void ESPKNXIP::save_to_eeprom()
{
  uint32_t address = 0;
  uint64_t magic = EEPROM_MAGIC;
  EEPROM.put(address, magic);
  address += sizeof(uint64_t);
  EEPROM.put(address++, registered_callback_assignments);
  for (uint8_t i = 0; i < MAX_CALLBACK_ASSIGNMENTS; ++i)
  {
    EEPROM.put(address, callback_assignments[i].address);
    address += sizeof(address_t);
  }
  for (uint8_t i = 0; i < MAX_CALLBACK_ASSIGNMENTS; ++i)
  {
    EEPROM.put(address, callback_assignments[i].callback_id);
    address += sizeof(callback_id_t);
  }
  EEPROM.put(address, physaddr);
  address += sizeof(address_t);

  EEPROM.put(address, custom_config_data);
  address += sizeof(custom_config_data);

  EEPROM.commit();
  DEBUG_PRINT("Wrote to EEPROM: 0x");
  DEBUG_PRINTLN(address, HEX);
}

void ESPKNXIP::restore_from_eeprom()
{
  uint32_t address = 0;
  uint64_t magic = 0;
  EEPROM.get(address, magic);
  if (magic != EEPROM_MAGIC)
  {
    DEBUG_PRINTLN("No valid magic in EEPROM, aborting restore.");
    DEBUG_PRINT("Expected 0x");
    DEBUG_PRINT((unsigned long)(EEPROM_MAGIC >> 32), HEX);
    DEBUG_PRINT(" 0x");
    DEBUG_PRINT((unsigned long)(EEPROM_MAGIC), HEX);
    DEBUG_PRINT(" got 0x");
    DEBUG_PRINT((unsigned long)(magic >> 32), HEX);
    DEBUG_PRINT(" 0x");
    DEBUG_PRINTLN((unsigned long)magic, HEX);
    return;
  }
  address += sizeof(uint64_t);
  EEPROM.get(address++, registered_callback_assignments);
  for (uint8_t i = 0; i < MAX_CALLBACK_ASSIGNMENTS; ++i)
  {
    EEPROM.get(address, callback_assignments[i].address);
    address += sizeof(address_t);
  }
  for (uint8_t i = 0; i < MAX_CALLBACK_ASSIGNMENTS; ++i)
  {
    EEPROM.get(address, callback_assignments[i].callback_id);
    address += sizeof(callback_id_t);
  }
  EEPROM.get(address, physaddr);
  address += sizeof(address_t); 

  //EEPROM.get(address, custom_config_data);
  //address += sizeof(custom_config_data);
  uint32_t conf_offset = address;
  for (uint8_t i = 0; i < registered_configs; ++i)
  {
    // First byte is flags.
    config_flags_t flags = CONFIG_FLAGS_NO_FLAGS;
    flags = (config_flags_t)EEPROM.read(address);
    DEBUG_PRINT("Flag in EEPROM @ ");
    DEBUG_PRINT(address - conf_offset);
    DEBUG_PRINT(": ");
    DEBUG_PRINTLN(flags, BIN);
    custom_config_data[custom_configs[i].offset] = flags;
    if (flags & CONFIG_FLAGS_VALUE_SET)
    {
      DEBUG_PRINTLN("Non-default value");
      for (int j = 0; j < custom_configs[i].len - sizeof(uint8_t); ++j)
      {
        custom_config_data[custom_configs[i].offset + sizeof(uint8_t) + j] = EEPROM.read(address + sizeof(uint8_t) + j);
      }
    }

    address += custom_configs[i].len;
  }

  DEBUG_PRINT("Restored from EEPROM: 0x");
  DEBUG_PRINTLN(address, HEX);
}

uint16_t ESPKNXIP::__ntohs(uint16_t n)
{
  return (uint16_t)((((uint8_t*)&n)[0] << 8) | (((uint8_t*)&n)[1]));
}

callback_assignment_id_t ESPKNXIP::__callback_register_assignment(address_t address, callback_id_t id)
{
  if (registered_callback_assignments >= MAX_CALLBACK_ASSIGNMENTS)
    return -1;

  callback_assignment_id_t aid = registered_callback_assignments;

  callback_assignments[aid].address = address;
  callback_assignments[aid].callback_id = id;
  registered_callback_assignments++;
  return aid;
}

void ESPKNXIP::__callback_delete_assignment(callback_assignment_id_t id)
{
  if (id >= registered_callback_assignments)
    return;

  uint32_t dest_offset = 0;
  uint32_t src_offset = 0;
  uint32_t len = 0;
  if (id == 0)
  {
    // start of array, so delete first entry
    src_offset = 1;
    // registered_ga_callbacks will be 1 in case of only one entry
    // registered_ga_callbacks will be 2 in case of two entries, etc..
    // so only copy anything, if there is it at least more then one element
    len = (registered_callback_assignments - 1);
  }
  else if (id == registered_callback_assignments - 1)
  {
    // last element, don't do anything, simply decrement counter
  }
  else
  {
    // somewhere in the middle
    // need to calc offsets

    // skip all prev elements
    dest_offset = id; // id is equal to how many element are in front of it
    src_offset = dest_offset + 1; // start after the current element
    len = (registered_callback_assignments - 1 - id);
  }

  if (len > 0)
  {
    memmove(callback_assignments + dest_offset, callback_assignments + src_offset, len * sizeof(callback_assignment_t));
  }

  registered_callback_assignments--;
}

callback_id_t ESPKNXIP::callback_register(String name, callback_fptr_t cb, void *arg, enable_condition_t cond)
{
  if (registered_callbacks >= MAX_CALLBACKS)
    return -1;

  callback_id_t id = registered_callbacks;

  callbacks[id].name = name;
  callbacks[id].fkt = cb;
  callbacks[id].cond = cond;
  callbacks[id].arg = arg;
  registered_callbacks++;
  return id;
}

void ESPKNXIP::callback_assign(callback_id_t id, address_t val)
{
  if (id >= registered_callbacks)
    return;

  __callback_register_assignment(val, id);
}

/**
 * Feedback functions start here
 */

feedback_id_t ESPKNXIP::feedback_register_int(String name, int32_t *value, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_INT;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;

  registered_feedbacks++;

  return id;
}

feedback_id_t ESPKNXIP::feedback_register_float(String name, float *value, uint8_t precision, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_FLOAT;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;
  feedbacks[id].options.float_options.precision = precision;

  registered_feedbacks++;

  return id;
}

feedback_id_t ESPKNXIP::feedback_register_bool(String name, bool *value, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_BOOL;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;

  registered_feedbacks++;

  return id;
}

feedback_id_t ESPKNXIP::feedback_register_action(String name, feedback_action_fptr_t value, void *arg, enable_condition_t cond)
{
  if (registered_feedbacks >= MAX_FEEDBACKS)
    return -1;

  feedback_id_t id = registered_feedbacks;

  feedbacks[id].type = FEEDBACK_TYPE_ACTION;
  feedbacks[id].name = name;
  feedbacks[id].cond = cond;
  feedbacks[id].data = (void *)value;
  feedbacks[id].options.action_options.arg = arg;

  registered_feedbacks++;

  return id;
}

void ESPKNXIP::loop()
{
  __loop_knx();
  if (server != nullptr)
  {
    __loop_webserver();
  }
}

void ESPKNXIP::__loop_webserver()
{
  server->handleClient();
}

void ESPKNXIP::__loop_knx()
{
  int read = udp.parsePacket();
  if (!read)
  {
    return;
  }
  DEBUG_PRINTLN(F(""));
  DEBUG_PRINT(F("LEN: "));
  DEBUG_PRINTLN(read);

  uint8_t buf[read];

  udp.read(buf, read);
  udp.flush();

  DEBUG_PRINT(F("Got packet:"));
  for (int i = 0; i < read; ++i)
  {
    DEBUG_PRINT(F(" 0x"));
    DEBUG_PRINT(buf[i], 16);
  }
  DEBUG_PRINTLN(F(""));

  knx_ip_pkt_t *knx_pkt = (knx_ip_pkt_t *)buf;

  DEBUG_PRINT(F("ST: 0x"));
  DEBUG_PRINTLN(__ntohs(knx_pkt->service_type), 16);

  if (knx_pkt->header_len != 0x06 && knx_pkt->protocol_version != 0x10 && knx_pkt->service_type != KNX_ST_ROUTING_INDICATION)
    return;

  cemi_msg_t *cemi_msg = (cemi_msg_t *)knx_pkt->pkt_data;

  DEBUG_PRINT(F("MT: 0x"));
  DEBUG_PRINTLN(cemi_msg->message_code, 16);

  if (cemi_msg->message_code != KNX_MT_L_DATA_IND)
    return;

  DEBUG_PRINT(F("ADDI: 0x"));
  DEBUG_PRINTLN(cemi_msg->additional_info_len, 16);

  cemi_service_t *cemi_data = &cemi_msg->data.service_information;

  if (cemi_msg->additional_info_len > 0)
    cemi_data = (cemi_service_t *)(((uint8_t *)cemi_data) + cemi_msg->additional_info_len);

  DEBUG_PRINT(F("C1: 0x"));
  DEBUG_PRINTLN(cemi_data->control_1.byte, 16);

  DEBUG_PRINT(F("C2: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.byte, 16);

  DEBUG_PRINT(F("DT: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.bits.dest_addr_type, 16);

  if (cemi_data->control_2.bits.dest_addr_type != 0x01)
    return;

  DEBUG_PRINT(F("HC: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.bits.hop_count, 16);

  DEBUG_PRINT(F("EFF: 0x"));
  DEBUG_PRINTLN(cemi_data->control_2.bits.extended_frame_format, 16);

  DEBUG_PRINT(F("Source: 0x"));
  DEBUG_PRINT(cemi_data->source.bytes.high, 16);
  DEBUG_PRINT(F(" 0x"));
  DEBUG_PRINTLN(cemi_data->source.bytes.low, 16);

  DEBUG_PRINT(F("Dest: 0x"));
  DEBUG_PRINT(cemi_data->destination.bytes.high, 16);
  DEBUG_PRINT(F(" 0x"));
  DEBUG_PRINTLN(cemi_data->destination.bytes.low, 16);

  knx_command_type_t ct = (knx_command_type_t)(((cemi_data->data[0] & 0xC0) >> 6) | ((cemi_data->pci.apci & 0x03) << 2));

  DEBUG_PRINT(F("CT: 0x"));
  DEBUG_PRINTLN(ct, 16);

  for (int i = 0; i < cemi_data->data_len; ++i)
  {
    DEBUG_PRINT(F(" 0x"));
    DEBUG_PRINT(cemi_data->data[i], 16);
  }

  DEBUG_PRINTLN(F("=="));

  // Call callbacks
  for (int i = 0; i < registered_callback_assignments; ++i)
  {
    DEBUG_PRINT(F("Testing: 0x"));
    DEBUG_PRINT(callback_assignments[i].address.bytes.high, 16);
    DEBUG_PRINT(F(" 0x"));
    DEBUG_PRINTLN(callback_assignments[i].address.bytes.low, 16);
    if (cemi_data->destination.value == callback_assignments[i].address.value)
    {
      DEBUG_PRINTLN(F("Found match"));
      if (callbacks[callback_assignments[i].callback_id].cond && !callbacks[callback_assignments[i].callback_id].cond())
      {
        DEBUG_PRINTLN(F("But it's disabled"));
#if ALLOW_MULTIPLE_CALLBACKS_PER_ADDRESS
        continue;
#else
        return;
#endif
      }
      uint8_t data[cemi_data->data_len];
      memcpy(data, cemi_data->data, cemi_data->data_len);
      data[0] = data[0] & 0x3F;
      message_t msg = {};
      msg.ct = ct;
      msg.received_on = cemi_data->destination;
      msg.data_len = cemi_data->data_len;
      msg.data = data;
      callbacks[callback_assignments[i].callback_id].fkt(msg, callbacks[callback_assignments[i].callback_id].arg);
#if ALLOW_MULTIPLE_CALLBACKS_PER_ADDRESS
      continue;
#else
      return;
#endif
    }
  }

  return;
}

// Global "singleton" object
ESPKNXIP knx;
