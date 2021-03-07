#include "11_Config.h"
#include <assert.h>

#ifdef ESP8266 
#include <LittleFS.h>
#else
#include <FS.h>
#include <LITTLEFS.h>
#endif

#include "2_Signal.h"
#include "6_MQTT.h"
#include "1_Radio.h"
#include "10_Wifi.h"
#include "12_Portal.h"

#define BUILD_BUG_ON(condition) ((void)sizeof(char[1 - 2*!!(condition)]))

#if defined(DEBUG) || defined(RFLINK_DEBUG)
#define DEBUG_RFLINK_CONFIG
#endif

namespace RFLink { namespace Config {

namespace commands {
  String set("set");
  String reset("reset");
  String dump("dump");
}


const char configFileName[] = "/config.json";

const char * jsonSections[] = {
    "wifi",
    "ota",
    "core",
    "mqtt",
    "portal",
    "signal",
    "radio",
    "root" // this is always the last one and matches index SectionId::EOF_id
};
#define jsonSections_count sizeof(jsonSections)/sizeof(char *)

static_assert(sizeof(jsonSections)/sizeof(char *) == SectionId::EOF_id+1, "jsonSections has missing/extra sections names, please compare with SectionId enum declations");


ConfigItem* configItemLists[] = {
    #if defined(RFLINK_WIFI_ENABLED)
    &RFLink::Wifi::configItems[0],
    &RFLink::Mqtt::configItems[0],
    #endif
    &RFLink::Portal::configItems[0],
    &RFLink::Signal::configItems[0],
    &RFLink::Radio::configItems[0],
 };
 #define configItemListsSize (sizeof(configItemLists)/sizeof(ConfigItem*))

StaticJsonDocument<8192> doc;


void resetConfig() {
    #ifdef ESP32
     if( LITTLEFS.exists(configFileName) )
        LITTLEFS.remove(configFileName);
    #else
    if( LittleFS.exists(configFileName) )
        LittleFS.remove(configFileName);
    #endif

    Serial.println("Config has been reset and requires a reboot to complete");
}


void printFile() {
  // Open file for reading
  Serial.println("Now dumping JSON file content:");
  #ifdef ESP32
  File file = LITTLEFS.open(configFileName, "r");
  #else
   File file = LittleFS.open(configFileName, "r");
  #endif
  
  if (!file) {
    Serial.println(F("Failed to read file"));
    return;
  }

  // Extract each characters by one by one
  while (file.available()) {
    Serial.print((char)file.read());
  }
  Serial.println();

  // Close the file
  file.close();
}

ConfigItem * findConfigItem(const char* name, SectionId section) {
    for(int i=0; i<configItemListsSize; i++) {
        ConfigItem *item = configItemLists[i];

        while(!item->typeIsEOF()) {
            if(item->section == section && strcmp(item->json_name, name) == 0 ) {
                return item;
            }

            item++;
        }
    }

    return nullptr;
}

void init() {

    char tmp[100];

    Serial.print("Loading persistent filesystem... ");

    #ifdef ESP32
        if( !LITTLEFS.begin(true) ) {
            Serial.println(" FAILED!!");
            return;
        }
        Serial.print("OK. ");

        sprintf(tmp, "File system usage: %lu/%luKB.", LITTLEFS.usedBytes()/1024, LITTLEFS.totalBytes()/1024);
        Serial.println(tmp);
    #else // this is ESP8266
        if( !LittleFS.begin() ) {
            Serial.println(" FAILED!!");
            return;
        }
        Serial.print("OK. ");

        FSInfo info;
        LittleFS.info(info);
        sprintf(tmp, "File system usage: %lu/%luKB.", info.usedBytes/1024, info.totalBytes/1024);
        Serial.println(tmp);
    #endif


    // DEBUG ONLY? Let's iterate over all registered configItems and count them/sanitize
    for(int i=0; i<configItemListsSize; i++) {
        ConfigItem *item = configItemLists[i];

        while(!item->typeIsEOF()) {
            if (item->typeIsChar()) {
                #ifdef DEBUG_RFLINK_CONFIG
                sprintf(tmp, "added configitem '%s' with default_value=", item->json_name);
                Serial.print(tmp);
                Serial.println(item->getCharDefaultValue());
                #endif

            } else if (item->typeIsLongInt()) {
                #ifdef DEBUG_RFLINK_CONFIG
                sprintf(tmp, "added configitem '%s' with default_value=", item->json_name);
                Serial.print(tmp);
                Serial.println(item->getLongIntDefaultValue());
                #endif

            } else if (item->typeIsBool()) {
                #ifdef DEBUG_RFLINK_CONFIG
                sprintf(tmp, "added configitem '%s' with default_value=", item->json_name);
                Serial.print(tmp);
                Serial.println(item->getBoolDefaultValue());
                #endif
            }
            else {
                Serial.print("unsupported configitem type=");
                Serial.println(item->type);
            }

            item++;
        }
    }
    //sprintf(tmp, "Counted %i config items in total", countConfigItems);
    //Serial.println(tmp);

    sprintf(tmp, "Now opening JSON config file '%s'", configFileName);
    Serial.println(tmp);

    #ifdef ESP32
    File file = LITTLEFS.open(configFileName, "r");
    #else
    File file = LittleFS.open(configFileName, "r");
    #endif

    DeserializationError error = deserializeJson(doc, file);
    if (error)
        Serial.println(F("Failed to read file, using default configuration"));
    file.close();

    sprintf(tmp, "JSON file mem usage: %lu / %lu", doc.memoryUsage(), doc.memoryPool().capacity());
    Serial.println(tmp);

    bool fileHasChanged = false;

    for(int i=0; i<configItemListsSize; i++) {
        ConfigItem *item = configItemLists[i];

        while(!item->typeIsEOF()) {
            JsonVariant sectionVariant = doc[jsonSections[item->section]];
            JsonObject sectionObject;

            if(sectionVariant.isUndefined()) {
                fileHasChanged = true;
                sectionObject = doc.createNestedObject(jsonSections[item->section]);
            } else if(!sectionVariant.is<JsonObject>()) {
                fileHasChanged = true;
                doc.remove(sectionVariant);
                sectionObject = doc.createNestedObject(jsonSections[item->section]);
            } else {
                sectionObject = sectionVariant.as<JsonObject>();
            }

            if(item->checkOrCreateValueInJsonObject(sectionObject)) {
                fileHasChanged = true;
            }

            item++;
        }
    }

    if(fileHasChanged) {
        saveConfigToFlash();
    }

    printFile();

}

class CallbackManager {
    private:
        void (*callbacks[20])();
    public:
        CallbackManager() {
            for(int i=0; i<sizeof(callbacks)/sizeof(void (*)()); i++){
                callbacks[i] = nullptr;
            }
        }

        void add(void (*c)()) {
            if(c == nullptr)
                return;
            for(int i=0; i<sizeof(callbacks)/sizeof(void (*)()); i++){
                if(callbacks[i] ==  c)
                    return;
                if(callbacks[i] == nullptr) {
                    callbacks[i] = c;
                    return;
                }
            }
        }

        void execute() {
            for(int i=0; i<sizeof(callbacks)/sizeof(void (*)()); i++){
                if(callbacks[i] == nullptr)
                    return;
                callbacks[i]();
            }
        }
};

bool pushNewConfiguration(JsonObject &data, String &message, bool escapeNewLine ) {

    bool configHasChanged = false;
    CallbackManager callbackMgr;
    message.reserve(256);
    String new_line("\n");

    if(escapeNewLine)
        new_line = "\\n";

    // we iterate root level to find sections!
    for (JsonPair kv : data) {

        #ifdef DEBUG_RFLINK_CONFIG
        Serial.print("Remote has root object named: ");
        Serial.println(kv.key().c_str());
        #endif
        
        JsonVariant && section_variant = kv.value();
        if(!section_variant.is<JsonObject>()) {
            message += "root entry '";
            message += kv.key().c_str();
            message += "' is not an object, it will be ignored!";
            message += new_line;
            continue;
        }
        JsonObject && sectionObject = section_variant.as<JsonObject>();
        
        SectionId lookupSectionID = getSectionIdFromString(kv.key().c_str());
        if( lookupSectionID == SectionId::EOF_id ) {
            message += "root entry '";
            message += kv.key().c_str();
            message += "' is not a valid section name, it will be ignored";
            message += new_line;
            continue;
        }

        // from here we have a valid section, now we go down a level in the remote object
        for (JsonPair section_kv : sectionObject) {
            #ifdef RFLINK_CONFIG_DEBUG
            Serial.print("Remote section has item named: ");
            Serial.println(section_kv.key().c_str());
            #endif

            ConfigItem *item = findConfigItem(section_kv.key().c_str(), lookupSectionID);
            if(item == nullptr) {
                 message += "section '";
                 message += kv.key().c_str();
                 message += "' has extra configuration item named '";
                 message += section_kv.key().c_str();
                 message += "' it will be ignored";
                 message += new_line;
                 continue;
            }

            JsonVariant && remoteVariant = section_kv.value();

            if(item->typeIsChar()) {
                if( !remoteVariant.is<char *>() ) {
                    message += "section '";
                    message += kv.key().c_str();
                    message += "' has '";
                    message += section_kv.key().c_str();
                    message += "' with mismatched type (not string) so it will be ignored";
                    message += new_line;
                    continue;
                }
                const char *str = remoteVariant.as<const char *>();
                if(strcmp(str, item->getCharValue()) == 0) // no change!
                    continue; 
                
                configHasChanged = true;
                callbackMgr.add(item->update_callback);
                item->setCharValue(str);

            } else if(item->typeIsLongInt()) {

                long int remote_value;
                if(remoteVariant.is<signed long>())
                    remote_value = remoteVariant.as<signed long>();
                else {
                    message += F("section '");
                    message += kv.key().c_str();
                    message += F("' has item '");
                    message += section_kv.key().c_str();
                    message += "' with mismatched type (not long int) so it will be ignored";
                    message += new_line;
                    continue;
                }

                if(remote_value == item->getLongIntValue() ) // no change!
                    continue;
                
                configHasChanged = true;
                callbackMgr.add(item->update_callback);
                item->setLongIntValue(remote_value);

            } else if(item->typeIsBool()) {
                if( !remoteVariant.is<bool>() ) {
                    message += F("section '");
                    message += kv.key().c_str();
                    message += F("' has item '");
                    message += section_kv.key().c_str();
                    message += "' with mismatched type (not bool) so it will be ignored";
                    message += new_line;
                    continue;
                }
                auto remote_value = remoteVariant.as<bool>();
                if(remote_value == item->getBoolValue() ) // no change!
                    continue; 
                
                configHasChanged = true;
                callbackMgr.add(item->update_callback);
                item->setBoolValue(remote_value);
            }
        }
    }

    Serial.println(message.c_str());

    if(configHasChanged) {
        if(!saveConfigToFlash()) {
            message += F("Error! Failed to write JSON config to FLASH!");
            //Serial.println(F("Error! Failed to write JSON config to FLASH!"));
            return false;
        } else {
            Serial.println("Confile hfile saved to flash.");
        }
        callbackMgr.execute();
    } else {
        Serial.println("Config file has not changed.");
    }

    return true;
}

JsonVariant ConfigItem::createInJsonObject(JsonObject &obj) {
    if(this->typeIsChar()) {
        obj[this->json_name] = this->getCharDefaultValue();
        this->jsonRef = obj[this->json_name];
        return this->jsonRef;

    } else if(this->typeIsLongInt()) {
        obj[this->json_name] = this->getLongIntDefaultValue();
        this->jsonRef = obj[this->json_name];
        return this->jsonRef;

    } else if(this->typeIsBool()) {
        obj[this->json_name] = this->getBoolDefaultValue();
        this->jsonRef = obj[this->json_name];
        return this->jsonRef;
    }
    return JsonVariant();
}

bool ConfigItem::checkOrCreateValueInJsonObject(JsonObject &obj) {

    bool result = false;

    JsonVariant value = obj[this->json_name];

    if( value.isUndefined() ) {
        createInJsonObject(obj);
        return true;
    }

    this->jsonRef = value;

    if(this->typeIsChar()) {
        if(!value.is<char *>()) {
            value.set((char*)this->getCharDefaultValue());
            return true;
        }
    }

    if(this->typeIsLongInt()) {
        if(!value.is<signed long>()) {
            value.set(this->getLongIntDefaultValue());
            return true;
        }
    }

    if(this->typeIsBool()) {
        if(!value.is<bool>()) {
            value.set(this->getBoolDefaultValue());
            return true;
        }
    }

    return result;       
}


ConfigItem::ConfigItem(const char *name,
                        SectionId section,
                        const  char* default_value,
                        void (*update_callback)() )
{

    this->json_name = name;
    this->section = section;
    this->type = ConfigItemType::STRING_t;
    this->update_callback = update_callback;
    

    static_assert( sizeof(this->defaultValue) <= sizeof(char *), "variable size is too small");
    this->defaultValue = (void *) default_value;

}

ConfigItem::ConfigItem(const char *name,
                        SectionId section,
                        long int default_value,
                        void (*update_callback)())
{
    this->json_name = name;
    this->section = section;
    this->type = ConfigItemType::LONG_INT_t;
    this->update_callback = update_callback;

    static_assert( sizeof(this->defaultValue) <= sizeof(long int), "variable size is too small");
    this->defaultValue = (void *) default_value;
}


ConfigItem::ConfigItem(const char *name,
                        SectionId section,
                        bool default_value,
                        void (*update_callback)())
{
    this->json_name = name;
    this->section = section;
    this->type = ConfigItemType::BOOLEAN_t;
    this->update_callback = update_callback;

    this->boolDefaultValue = default_value;

}

ConfigItem::ConfigItem()
{
    this->json_name = nullptr;
    this->section = SectionId::EOF_id;
    this->type = ConfigItemType::EOF_t;
    this->update_callback = nullptr;
}


void dumpConfigToString(String &destination) {
    //serializeJson(doc, destination);
    serializeJsonPretty(doc, destination);
}

void dumpConfigToSerial() {
    serializeJson(doc, Serial);
    Serial.println();
}

SectionId getSectionIdFromString(const char *name) {

    for(int i=0; i<jsonSections_count; i++) {
        if(strcmp(name, jsonSections[i])== 0)
        return (SectionId) i;
    }

    return SectionId::EOF_id;

}

bool saveConfigToFlash(){

    Serial.print("Saving JSON config to FLASH.... ");
    Signal::AsyncSignalScanner::stopScanning();

#ifdef ESP32
    if( LITTLEFS.exists(F("/tmp.json")) )
        LITTLEFS.remove(F("/tmp.json"));
    File file = LITTLEFS.open(F("/tmp.json"), "w");
#else
    if( LittleFS.exists(F("/tmp.json")) )
        LittleFS.remove(F("/tmp.json"));
    File file = LittleFS.open(F("/tmp.json"), "w");
#endif

    auto bytes_written = serializeJson(doc, file);
    file.close();

    if (bytes_written == 0)
    {
        Serial.println(F("failed!"));
        Signal::AsyncSignalScanner::startScanning();
        return false;
    }
    else
    {
#ifdef ESP32
        if( LITTLEFS.exists(configFileName) )
            LITTLEFS.remove(configFileName);
        LITTLEFS.rename(F("/tmp.json"), configFileName);
#else
        if( LittleFS.exists(configFileName) )
            LittleFS.remove(configFileName);
        LittleFS.rename(F("/tmp.json"), configFileName);
#endif
        Serial.println(F("OK"));
    }


    Signal::AsyncSignalScanner::startScanning();


    return true;
}


void executeCliCommand(const char *cmd) {
    String strCmd(cmd);
    
    int commaIndex = strCmd.indexOf(';');

    if(commaIndex < 0) {
      Serial.println("Error : failed to find ending ';' for the command");
      return;
    }

    String command = strCmd.substring(0, commaIndex);

    if(command.equalsIgnoreCase(commands::reset)){
      resetConfig();
    } else if(command.equalsIgnoreCase(commands::dump)) {
        dumpConfigToSerial();
    }
    else if(command.equalsIgnoreCase(commands::set)) {
      //String argsStr = strCmd.substring(commaIndex+1);
      DynamicJsonDocument json(2500);

      if( deserializeJson(json, cmd + commaIndex +1 ) != DeserializationError::Ok) {
        Serial.print("An error occured while reading json");
        return;
      }

      auto root = json.as<JsonObject>();
      String msg;
      pushNewConfiguration(root, msg, false);
      if(msg.length() > 0) {
          Serial.printf("Some warning/errors occured while trying to SET config from CLI:\n");
          Serial.println(msg.c_str());
      }
    }
    else {
      Serial.printf("Error : unknown command '%s'\n", command.c_str());
    }

  }

} // end of Config namespace
} // end of RFLink namespace