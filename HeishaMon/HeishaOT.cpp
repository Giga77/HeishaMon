#include "OpenTherm.h"
#include "HeishaOT.h"
#include "decode.h"
#include "rules.h"
#include "webfunctions.h"
#include "src/common/stricmp.h"
#include "src/common/progmem.h"

OpenTherm ot(inOTPin, outOTPin, true);

const char* mqtt_topic_opentherm PROGMEM = "opentherm";

unsigned long otResponse = 0;

struct heishaOTDataStruct_t heishaOTDataStruct[] = {
  //WRITE values
  { "chEnable", TBOOL, { .b = false }, 3 }, //is central heating enabled by thermostat
  { "dhwEnable", TBOOL, { .b = false }, 3 }, //is dhw heating enabled by thermostat
  { "roomTemp", TFLOAT, { .f = 0 }, 3 }, //what is measured room temp by thermostat
  { "roomTempSet", TFLOAT, { .f = 0 }, 3 }, //what is request room temp setpoint by thermostat
  { "chSetpoint", TFLOAT, { .f = 0 }, 3 }, //what is calculated Ta setpoint by thermostat
  //READ AND WRITE values
  { "dhwSetpoint", TFLOAT, { .f = 65 }, 2 }, //what is DHW setpoint by thermostat
  { "maxTSet", TFLOAT, { .f = 65 }, 2 }, //max ch setpoint
  //READ values
  { "outsideTemp", TFLOAT, { .f = 0 }, 1 }, //provides measured outside temp to thermostat
  { "inletTemp", TFLOAT, { .f = 0 }, 1 }, //provides measured Treturn temp to thermostat
  { "outletTemp", TFLOAT, { .f = 0 }, 1 }, //provides measured Tout (boiler) temp to thermostat
  { "dhwTemp", TFLOAT, { .f = 0 }, 1 }, //provides measured dhw water temp to to thermostat
  { "flameState", TBOOL, { .b = false }, 1 }, //provides current flame state to thermostat
  { "chState", TBOOL, { .b = false }, 1 }, //provides if boiler is in centrale heating state
  { "dhwState", TBOOL, { .b = false }, 1 }, //provides if boiler is in dhw heating state
  { "roomSetOverride", TFLOAT, { .f = 0 }, 1 }, //provides a room setpoint override ID9 (not implemented completly in heishamon)
  { NULL, 0, 0, 0 }
};

void mqttPublish(char* topic, char* subtopic, char* value);

struct heishaOTDataStruct_t *getOTStructMember(const char *name) {
  int i = 0;
  while(heishaOTDataStruct[i].name != NULL) {
    if(strcmp(heishaOTDataStruct[i].name, name) == 0) {
      return &heishaOTDataStruct[i];
    }
    i++;
  }
  return NULL;
}

void processOTRequest(unsigned long request, OpenThermResponseStatus status) {
 if (status != OpenThermResponseStatus::SUCCESS) {
    log_message(_F("OpenTherm: Request invalid!"));
 } else {
  char log_msg[512];
  {
    char str[200];
    sprintf_P(str, PSTR("%#010x"), request);
    mqttPublish((char*)mqtt_topic_opentherm, _F("raw"), str);
  }
  switch (ot.getDataID(request)) {
    case OpenThermMessageID::Status: { //mandatory
        unsigned long data = ot.getUInt(request);
        unsigned int CHEnable = (data >> 8) & (1 << 0);
        unsigned int DHWEnable = ((data >> 8) & (1 << 1)) >> 1;
        unsigned int Cooling = ((data >> 8) & (1 << 2)) >> 2;
        unsigned int OTCEnable = ((data >> 8) & (1 << 3)) >> 3;
        unsigned int CH2Enable = ((data >> 8) & (1 << 4)) >> 4;
        unsigned int SWMode = ((data >> 8) & (1 << 5)) >> 5;
        unsigned int DHWBlock = ((data >> 8) & (1 << 6)) >> 6;

        getOTStructMember(_F("chEnable"))->value.b = (bool)CHEnable;
        getOTStructMember(_F("dhwEnable"))->value.b = (bool)DHWEnable;

        sprintf_P(log_msg, PSTR(
                "OpenTherm: Received status check: %d, CH: %d, DHW: %d, Cooling, %d, OTC: %d, CH2: %d, SWMode: %d, DHWBlock: %d"),
                data >> 8, CHEnable, DHWEnable, Cooling, OTCEnable, CH2Enable, SWMode, DHWBlock
               );
        log_message(log_msg);
        //clean slave bits from 2-byte data
        data = ((data >> 8) << 8);

        unsigned int FaultInd = false;
        unsigned int CHMode = (unsigned int)getOTStructMember(_F("chState"))->value.b;
        unsigned int FlameStatus = (unsigned int)getOTStructMember(_F("flameState"))->value.b;
        unsigned int DHWMode = (unsigned int)getOTStructMember(_F("dhwState"))->value.b;
        unsigned int CoolingStatus = false;
        unsigned int CH2 = false;
        unsigned int DiagInd = false;
        sprintf_P(log_msg,
                PSTR("OpenTherm: Send status: CH: %d, Flame:%d, DHW: %d"),
                CHMode, FlameStatus, DHWMode
               );
        log_message(log_msg);
        unsigned int responsedata = FaultInd | (CHMode << 1) | (DHWMode << 2) | (FlameStatus << 3) | (CoolingStatus << 4) | (CH2 << 5) | (DiagInd << 6);
        otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Status, (data |= responsedata));
        rules_event_cb(_F("?"), _F("chEnable"));
        rules_event_cb(_F("?"), _F("dhwEnable"));
      } break;
    case OpenThermMessageID::TSet: { //mandatory
        getOTStructMember(_F("chSetpoint"))->value.f = ot.getFloat(request);
        char str[200];
        sprintf_P((char *)&str, PSTR("%.*f"), 4, getOTStructMember(_F("chSetpoint"))->value.f);
        sprintf_P(log_msg, PSTR("OpenTherm: control setpoint TSet: %s"), str);
        log_message(log_msg);
        mqttPublish((char*)mqtt_topic_opentherm, _F("chSetpoint"), str);
        otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::TSet, request & 0xffff);
        rules_event_cb(_F("?"), _F("chsetpoint"));
      } break;
    case OpenThermMessageID::MConfigMMemberIDcode: {
      unsigned long data = ot.getUInt(request);
      unsigned int SmartPower = (data >> 8) & (1 << 0);
      sprintf_P(log_msg,
			  PSTR("OpenTherm: Received master config: %d, Smartpower: %d"),
              data >> 8, SmartPower
             );
      log_message(log_msg);
      otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::MConfigMMemberIDcode, data);

      //ot.setSmartPower((bool)SmartPower); not working correctly yet
      } break;      
    case OpenThermMessageID::SConfigSMemberIDcode: { //mandatory
        log_message(_F("OpenTherm: Received read slave config"));
        unsigned int DHW = true;
        unsigned int Modulation = false;
        unsigned int Cool = false;
        unsigned int DHWConf = false;
        unsigned int Pump = false;
        unsigned int CH2 = false; // no 2nd zone yet

        unsigned int data = DHW | (Modulation << 1) | (Cool << 2) | (DHWConf << 3) | (Pump << 4) | (CH2 << 5);
        data <<= 8;
        otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::SConfigSMemberIDcode, data);

      } break;
    case OpenThermMessageID::MaxRelModLevelSetting: { //mandatory
        float data = ot.getFloat(request);
        sprintf_P(log_msg, PSTR("OpenTherm: Max relative modulation level: %f"), data);
        log_message(log_msg);
        otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::MaxRelModLevelSetting, request & 0xffff); //ACK for mandatory fields

      } break;
    case OpenThermMessageID::RelModLevel: { //mandatory
        log_message(_F("OpenTherm: Received read relative modulation level"));
        otResponse = ot.buildResponse(OpenThermMessageType::DATA_INVALID, OpenThermMessageID::RelModLevel, request & 0xffff); //invalid for now to fill mandatory fields
      } break;
    case OpenThermMessageID::Tboiler: { //mandatory
        log_message(_F("OpenTherm: Received read boiler flow temp (outlet)"));
        if (getOTStructMember(_F("outletTemp"))->value.f > -99) {
          unsigned long data = ot.temperatureToData(getOTStructMember(_F("outletTemp"))->value.f);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tboiler, data);

        } else {
          otResponse = ot.buildResponse(OpenThermMessageType::DATA_INVALID, OpenThermMessageID::Tboiler, request & 0xffff);
        }
      } break;
    // now adding some more useful, not mandatory, types
    case OpenThermMessageID::RBPflags: { //Pre-Defined Remote Boiler Parameters
        log_message(_F("OpenTherm: Received Remote Boiler parameters request"));
        //fixed settings for now - allow read and write DHWset and maxTset remote params
        const unsigned int DHWsetTransfer = true;
        const unsigned int maxCHsetTransfer = true;
        const unsigned int DHWsetReadWrite = true;
        const unsigned int maxCHsetReadWrite = true;
        const unsigned int responsedata = DHWsetReadWrite | (maxCHsetReadWrite << 1) | (DHWsetTransfer << 8) | (maxCHsetTransfer << 9);
        otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::RBPflags, responsedata);
      } break;
    case OpenThermMessageID::TdhwSetUBTdhwSetLB : { //DHW boundaries
        log_message(_F("OpenTherm: Received DHW set boundaries remote parameters request"));
        //fixed settings for now
        const unsigned int DHWsetUppBound = 75;
        const unsigned int DHWsetLowBound = 40;
        const unsigned int responsedata = DHWsetLowBound | (DHWsetUppBound << 8);
        otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::TdhwSetUBTdhwSetLB, responsedata);
      } break;
    case OpenThermMessageID::MaxTSetUBMaxTSetLB  : { //CHset boundaries
        log_message(_F("OpenTherm: Received CH set boundaries remote parameters request"));
        //fixed settings for now, seems valid for most heatpump types
        const unsigned int CHsetUppBound = 65;
        const unsigned int CHsetLowBound = 20;
        const unsigned int responsedata = CHsetLowBound | (CHsetUppBound << 8);
        otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::MaxTSetUBMaxTSetLB, responsedata);
      } break;
    case OpenThermMessageID::Tr: {
        getOTStructMember(_F("roomTemp"))->value.f = ot.getFloat(request);
        char str[200];
        sprintf_P((char *)&str, PSTR("%.*f"), 4, getOTStructMember(_F("roomTemp"))->value.f);
        sprintf_P(log_msg, PSTR("OpenTherm: Room temp: %s"), str);
        log_message(log_msg);
        mqttPublish((char*)mqtt_topic_opentherm, _F("roomTemp"), str);
        otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::Tr, request & 0xffff);
        rules_event_cb(_F("?"), _F("roomtemp"));
      } break;
    case OpenThermMessageID::TrSet: {
        getOTStructMember(_F("roomTempSet"))->value.f = ot.getFloat(request);
        char str[200];
        sprintf_P((char *)&str, PSTR("%.*f"), 4, getOTStructMember(_F("roomTempSet"))->value.f);
        sprintf_P(log_msg, PSTR("OpenTherm: Room setpoint: %s"), str);
        log_message(log_msg);
        mqttPublish((char*)mqtt_topic_opentherm, _F("roomTempSet"), str);
        otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::TrSet, request & 0xffff);
        rules_event_cb(_F("?"), _F("roomtempset"));
      } break;
    case OpenThermMessageID::TdhwSet: {
        if (ot.getMessageType(request) == OpenThermMessageType::WRITE_DATA) {
          getOTStructMember(_F("dhwSetpoint"))->value.f = ot.getFloat(request);
          char str[200];
          sprintf_P((char *)&str, PSTR("%.*f"), 4, getOTStructMember(_F("dhwSetpoint"))->value.f);
          sprintf_P(log_msg, PSTR("OpenTherm: Write request DHW setpoint: %s"), str);
          log_message(log_msg);
          mqttPublish((char*)mqtt_topic_opentherm, _F("dhwSetpoint"), str);
          otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::TdhwSet, ot.temperatureToData(getOTStructMember(_F("dhwSetpoint"))->value.f));
        } else { //READ_DATA
          sprintf_P(log_msg, PSTR("OpenTherm: Read request DHW setpoint"));
          log_message(log_msg);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::TdhwSet, ot.temperatureToData(getOTStructMember(_F("dhwSetpoint"))->value.f));
          rules_event_cb(_F("?"), _F("dhwsetpoint"));
        }
      } break;
    case OpenThermMessageID::MaxTSet: {
        if (ot.getMessageType(request) == OpenThermMessageType::WRITE_DATA) {
          getOTStructMember(_F("maxTSet"))->value.f = ot.getFloat(request);
          char str[200];
          sprintf_P((char *)&str, PSTR("%.*f"), 4, getOTStructMember(_F("maxTSet"))->value.f);
          sprintf_P(log_msg, PSTR("OpenTherm: Write request Max Ta-set setpoint: %s"), str);
          log_message(log_msg);
          mqttPublish((char*)mqtt_topic_opentherm, _F("maxTSet"), str);
          otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::MaxTSet, ot.temperatureToData(getOTStructMember(_F("maxTSet"))->value.f));
        } else { //READ_DATA
          sprintf_P(log_msg, PSTR("OpenTherm: Read request Max Ta-set setpoint"));
          log_message(log_msg);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::MaxTSet, ot.temperatureToData(getOTStructMember(_F("maxTSet"))->value.f));
          rules_event_cb(_F("?"), _F("maxtset"));
        }
      } break;
    case OpenThermMessageID::Tret: {
        log_message(_F("OpenTherm: Received read Tret"));
        if (getOTStructMember(_F("inletTemp"))->value.f > -99) {
          unsigned long data = ot.temperatureToData(getOTStructMember(_F("inletTemp"))->value.f);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tret, data);
        } else {
          otResponse = ot.buildResponse(OpenThermMessageType::DATA_INVALID, OpenThermMessageID::Tret, request & 0xffff);
        }
      } break;
    case OpenThermMessageID::Tdhw: {
        log_message(_F("OpenTherm: Received read DHW temp"));
        if (getOTStructMember(_F("dhwTemp"))->value.f > -99) {
          unsigned long data = ot.temperatureToData(getOTStructMember(_F("dhwTemp"))->value.f);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Tdhw, data);
        } else {
          otResponse = ot.buildResponse(OpenThermMessageType::DATA_INVALID, OpenThermMessageID::Tdhw, request & 0xffff);
        }
      } break;
    case OpenThermMessageID::Toutside: {
        log_message(_F("OpenTherm: Received read outside temp"));
        if (getOTStructMember(_F("outsideTemp"))->value.f > -99) {
          unsigned long data = ot.temperatureToData(getOTStructMember(_F("outsideTemp"))->value.f);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::Toutside, data);

        } else {
          otResponse = ot.buildResponse(OpenThermMessageType::DATA_INVALID, OpenThermMessageID::Toutside, request & 0xffff);
        }
      } break;
    case OpenThermMessageID::TrOverride: {
        log_message(_F("OpenTherm: Received read room set override temp"));
        if (getOTStructMember(_F("roomSetOverride"))->value.f > -99) {
          unsigned long data = ot.temperatureToData(getOTStructMember(_F("roomSetOverride"))->value.f);
          otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::TrOverride, data);

        } else {
          otResponse = ot.buildResponse(OpenThermMessageType::DATA_INVALID, OpenThermMessageID::TrOverride, request & 0xffff);
        }
      } break;

      
    /*
      case OpenThermMessageID::ASFflags: {
        log_message(_F("OpenTherm: Received read ASF flags"));
        otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::ASFflags, 0);

      } break;
      case OpenThermMessageID::MaxTSetUBMaxTSetLB: {
      log_message(_F("OpenTherm: Received read Ta-set bounds"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::MaxTSetUBMaxTSetLB, 0x5028);

      } break;
      case OpenThermMessageID::TdhwSetUBTdhwSetLB: {
      log_message(_F("OpenTherm: Received read DHW-set bounds"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::TdhwSetUBTdhwSetLB, 0x5028);

      } break;
      case OpenThermMessageID::NominalVentilationValue: {
      log_message(_F("OpenTherm: Received read nominal ventilation value"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::NominalVentilationValue, 0);

      } break;
      case OpenThermMessageID::RemoteParameterSettingsVH: {
      log_message(_F("OpenTherm: Received read remote parameters settings"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::RemoteParameterSettingsVH, 0);

      } break;
      
      case OpenThermMessageID::TrOverride: {
      log_message(_F("OpenTherm: Received read remote override setpoint"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::TrOverride, 0);

      } break;
      case OpenThermMessageID::CHPressure: {
      log_message(_F("OpenTherm: Received read CH pressure"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::CHPressure, 0);

      } break;
      case OpenThermMessageID::OpenThermVersionMaster: {
      float data = ot.getFloat(request);
      char str[200];
      sprintf_P((char *)&str, PSTR("%.*f"), 4, data);
      sprintf_P(log_msg, PSTR("OpenTherm: OT Master version: %s"), str);
      log_message(log_msg);
      otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::OpenThermVersionMaster, request & 0xffff);

      } break;
      case OpenThermMessageID::OpenThermVersionSlave: {
      log_message(_F("OpenTherm: Received read OT slave version"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::OpenThermVersionSlave, 0);

      } break;
      case OpenThermMessageID::MasterVersion: {
      float data = ot.getFloat(request);
      char str[200];
      sprintf_P((char *)&str, PSTR("%.*f"), 4, data);
      sprintf_P(log_msg, PSTR("OpenTherm: Master device version: %s"), str);
      log_message(log_msg);
      otResponse = ot.buildResponse(OpenThermMessageType::WRITE_ACK, OpenThermMessageID::MasterVersion, request & 0xffff);


      } break;
      case OpenThermMessageID::SlaveVersion: {
      log_message(_F("OpenTherm: Received read slave device version"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::SlaveVersion, 0);

      } break;

      case OpenThermMessageID::TSP: {
      log_message(_F("OpenTherm: Received read TSP"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::TSP, 0);

      } break;
      case OpenThermMessageID::FHBsize: {
      log_message(_F("OpenTherm: Received read fault buffer size"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::FHBsize, 0);

      } break;

      case OpenThermMessageID::RemoteOverrideFunction: {
      log_message(_F("OpenTherm: Received read remote override function"));
      otResponse = ot.buildResponse(OpenThermMessageType::READ_ACK, OpenThermMessageID::RemoteOverrideFunction, 0);

      } break;
    */
    default: {
        sprintf_P(log_msg, PSTR("OpenTherm: Unknown data ID: %d (%#010x)"), ot.getDataID(request), request);
        log_message(log_msg);
        otResponse = ot.buildResponse(OpenThermMessageType::UNKNOWN_DATA_ID, ot.getDataID(request), 0);
      } break;

  }
 }
}

void IRAM_ATTR handleOTInterrupt() {
  ot.handleInterrupt();
}

void HeishaOTSetup() {
  ot.begin(handleOTInterrupt, processOTRequest);
}

void HeishaOTLoop(char * actData, PubSubClient &mqtt_client, char* mqtt_topic_base) {
  // getOTStructMember(_F("outsideTemp"))->value.f = actData[0] == '\0' ? 0 : getDataValue(actData, 14).toFloat();
  // getOTStructMember(_F("inletTemp"))->value.f =  actData[0] == '\0' ? 0 : getDataValue(actData, 5).toFloat();
  // getOTStructMember(_F("outletTemp"))->value.f =  actData[0] == '\0' ? 0 : getDataValue(actData, 6).toFloat();
  // getOTStructMember(_F("flameState"))->value.b = actData[0] == '\0' ? 0 : ((getDataValue(actData, 8).toInt() > 0 ) ? true : false); //compressor freq as flame on state
  // getOTStructMember(_F("chState"))->value.b = actData[0] == '\0' ? 0 : (((getDataValue(actData, 8).toInt() > 0 ) && (getDataValue(actData, 20).toInt() == 0 )) ? true : false); // 3-way valve on room
  // getOTStructMember(_F("dhwState"))->value.b =  actData[0] == '\0' ? 0 : (((getDataValue(actData, 8).toInt() > 0 ) && (getDataValue(actData, 20).toInt() == 1 )) ? true : false); /// 3-way valve on dhw

  // opentherm loop
  if (otResponse && ot.isReady()) {
    ot.sendResponse(otResponse);
    otResponse = 0;
  }
  ot.process();
}

void mqttOTCallback(char* topic, char* value) {
  //only READ values(strcmp_P(PSTR("dhwTem can be overwritten using received mqtt messages
  //log_message(_F("OpenTherm: MQTT message received"));
  if (strcmp_P(PSTR("outsideTemp"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'outsideTemp'"));
    getOTStructMember(_F("outsideTemp"))->value.f = String(value).toFloat();
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("inletTemp"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'inletTemp'"));
    getOTStructMember(_F("inletTemp"))->value.f = String(value).toFloat();
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("outletTemp"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'outletTemp'"));
    getOTStructMember(_F("outletTemp"))->value.f = String(value).toFloat();
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("dhwTemp"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'dhwTemp'"));
    getOTStructMember(_F("dhwTemp"))->value.f = String(value).toFloat();
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("dhwSetpoint"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'dhwSetpoint'"));
    getOTStructMember(_F("dhwSetpoint"))->value.f = String(value).toFloat();
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("maxTSet"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'maxTSet'"));
    getOTStructMember(_F("maxTSet"))->value.f = String(value).toFloat();
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("flameState"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'flameState'"));
    getOTStructMember(_F("flameState"))->value.b = ((stricmp((char*)"true", value) == 0) || (String(value).toInt() == 1 ));
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("chState"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'chState'"));
    getOTStructMember(_F("chState"))->value.b = ((stricmp((char*)"true", value) == 0) || (String(value).toInt() == 1 ));
    rules_event_cb(_F("?"), topic);
  }
  else if (strcmp_P(PSTR("dhwState"), topic) == 0) {
    log_message(_F("OpenTherm: MQTT message received 'dhwState'"));
    getOTStructMember(_F("dhwState"))->value.b = ((stricmp((char*)"true", value) == 0) || (String(value).toInt() == 1 ));
    rules_event_cb(_F("?"), topic);
  }

}

void openthermTableOutput(struct webserver_t *client) {
  char str[64];
  //roomtemp
  webserver_send_content_P(client, PSTR("<tr><td>roomTemp</td><td>R</td><td>"), 35);
  dtostrf( getOTStructMember(_F("roomTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //roomtempset
  webserver_send_content_P(client, PSTR("<tr><td>roomTempSet</td><td>R</td><td>"), 38);
  dtostrf( getOTStructMember(_F("roomTempSet"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //chSetpoint
  webserver_send_content_P(client, PSTR("<tr><td>chSetpoint</td><td>R</td><td>"), 37);
  dtostrf( getOTStructMember(_F("chSetpoint"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //dhwSetpoint
  webserver_send_content_P(client, PSTR("<tr><td>dhwSetpoint</td><td>RW</td><td>"), 39);
  dtostrf( getOTStructMember(_F("dhwSetpoint"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //maxTSet
  webserver_send_content_P(client, PSTR("<tr><td>maxTSet</td><td>RW</td><td>"), 35);
  dtostrf( getOTStructMember(_F("maxTSet"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //outsideTemp
  webserver_send_content_P(client, PSTR("<tr><td>outsideTemp</td><td>W</td><td>"), 38);
  dtostrf( getOTStructMember(_F("outsideTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //inletTemp
  webserver_send_content_P(client, PSTR("<tr><td>inletTemp</td><td>W</td><td>"), 36);
  dtostrf( getOTStructMember(_F("inletTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //outletTemp
  webserver_send_content_P(client, PSTR("<tr><td>outletTemp</td><td>W</td><td>"), 37);
  dtostrf( getOTStructMember(_F("outletTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //dhwTemp
  webserver_send_content_P(client, PSTR("<tr><td>dhwTemp</td><td>W</td><td>"), 34);
  dtostrf( getOTStructMember(_F("dhwTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //flameState
  webserver_send_content_P(client, PSTR("<tr><td>flameState</td><td>W</td><td>"), 37);
  getOTStructMember(_F("flameState"))->value.b ? webserver_send_content_P(client, PSTR("on"), 2) : webserver_send_content_P(client, PSTR("off"), 3);
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //chState
  webserver_send_content_P(client, PSTR("<tr><td>chState</td><td>W</td><td>"), 34);
  getOTStructMember(_F("chState"))->value.b ? webserver_send_content_P(client, PSTR("on"), 2) : webserver_send_content_P(client, PSTR("off"), 3);
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
  //dhwState
  webserver_send_content_P(client, PSTR("<tr><td>dhwState</td><td>W</td><td>"), 35);
  getOTStructMember(_F("dhwState"))->value.b ? webserver_send_content_P(client, PSTR("on"), 2) : webserver_send_content_P(client, PSTR("off"), 3);
  webserver_send_content_P(client, PSTR("</td></tr>"), 10);
}

void openthermJsonOutput(struct webserver_t *client) {
  webserver_send_content_P(client, PSTR("{"), 1);

  char str[64];

  //roomtemp
  webserver_send_content_P(client, PSTR("\"roomTemp\":{\"type\": \"R\",\"value\":"), 32);
  dtostrf( getOTStructMember(_F("roomTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //roomtempset
  webserver_send_content_P(client, PSTR("\"roomTempSet\":{\"type\": \"R\",\"value\":"), 35);
  dtostrf( getOTStructMember(_F("roomTempSet"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //chSetpoint
  webserver_send_content_P(client, PSTR("\"chSetpoint\":{\"type\": \"R\",\"value\":"), 34);
  dtostrf( getOTStructMember(_F("chSetpoint"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //dhwSetpoint
  webserver_send_content_P(client, PSTR("\"dhwSetpoint\":{\"type\": \"RW\",\"value\":"), 36);
  dtostrf( getOTStructMember(_F("dhwSetpoint"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //maxTSet
  webserver_send_content_P(client, PSTR("\"maxTSet\":{\"type\": \"RW\",\"value\":"), 32);
  dtostrf( getOTStructMember(_F("maxTSet"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //outsideTemp
  webserver_send_content_P(client, PSTR("\"outsideTemp\":{\"type\": \"W\",\"value\":"), 35);
  dtostrf( getOTStructMember(_F("outsideTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //inletTemp
  webserver_send_content_P(client, PSTR("\"inletTemp\":{\"type\": \"W\",\"value\":"), 33);
  dtostrf( getOTStructMember(_F("inletTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //outletTemp
  webserver_send_content_P(client, PSTR("\"outletTemp\":{\"type\": \"W\",\"value\":"), 34);
  dtostrf( getOTStructMember(_F("outletTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //dhwTemp
  webserver_send_content_P(client, PSTR("\"dhwTemp\":{\"type\": \"W\",\"value\":"), 31);
  dtostrf( getOTStructMember(_F("dhwTemp"))->value.f, 0, 2, str);
  webserver_send_content(client, str, strlen(str));
  webserver_send_content_P(client, PSTR("},"), 2);
  //flameState
  webserver_send_content_P(client, PSTR("\"flameState\":{\"type\": \"W\",\"value\":"), 34);
  getOTStructMember(_F("flameState"))->value.b ? webserver_send_content_P(client, PSTR("true"), 4) : webserver_send_content_P(client, PSTR("false"), 5);
  webserver_send_content_P(client, PSTR("},"), 2);
  //chState
  webserver_send_content_P(client, PSTR("\"chState\":{\"type\": \"W\",\"value\":"), 31);
  getOTStructMember(_F("chState"))->value.b ? webserver_send_content_P(client, PSTR("true"), 4) : webserver_send_content_P(client, PSTR("false"), 5);
  webserver_send_content_P(client, PSTR("},"), 2);
  //dhwState
  webserver_send_content_P(client, PSTR("\"dhwState\":{\"type\": \"W\",\"value\":"), 32);
  getOTStructMember(_F("dhwState"))->value.b ? webserver_send_content_P(client, PSTR("true"), 4) : webserver_send_content_P(client, PSTR("false"), 5);
  webserver_send_content_P(client, PSTR("}"), 1);

  webserver_send_content_P(client, PSTR("}"), 1);
}
