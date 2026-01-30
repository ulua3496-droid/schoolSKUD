#include <Wire.h>
#include <SPI.h>
#include <MFRC522.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include "RTClib.h" 

#define SS_PIN 5
#define RST_PIN 0 
#define RED 13
#define GRN 22
MFRC522 rfid(SS_PIN, RST_PIN);

String lastScannedID = ""; //для перекидывания нрвых айди на сайт(при нажатии кнопки)(глобальная переменная, поэтому до всех функций)

AsyncWebServer server(80);
RTC_DS3231 rtc;

const char* ap_ssid = "School_SKUD";
const char* ap_password = "voskr2121";

String getTimestamp() {
  if (rtc.begin()) { 
    DateTime now = rtc.now();
    char buf[] = "YYYY-MM-DD hh:mm:ss";
    return String(now.toString(buf));
  } else {
    return "20000-00-00 00:00:00"; 
  }
}
bool emergencyOpen = false;
bool emergencyClose = false;

int banStartMin = 1320; //22-00*60
int banEndMin = 360; //06-00*60

//функция смены статуса активности
bool updateStudentStatus(String cardId, bool newStatus) {
    File file = LittleFS.open("/students.json", "r");
    if(!file) return false;

    DynamicJsonDocument doc(4096);
    DeserializationError error = deserializeJson(doc, file);
    file.close();

    JsonArray students = doc["students"];
    bool found = false;
    
    for(JsonObject student : students) {
        if(student["id"].as<String>() == cardId) {
            found = true;
          
            bool oldStatus = student["active"].as<bool>();
            
            student["active"] = newStatus;
    
            JsonArray history = student["history"];
            if(history.size() >= 50) {
                history.remove(0);
            }
            
            JsonObject record = history.createNestedObject();
            record["timestamp"] = getTimestamp();
            record["action"] = "status_change";
            record["old_status"] = oldStatus;
            record["new_status"] = newStatus;
            
            Serial.println("Статус изменён: " + cardId + " - " + 
                          String(oldStatus ? "активен" : "не активен") + 
                          " → " + String(newStatus ? "активен" : "не активен"));
            break;
        }
    }
    if(!found) {
        Serial.println("Карта " + cardId + " не найдена");
        return false;
    }
    
    // 4. Сохраняем изменения
    file = LittleFS.open("/students.json", "w");
    if(!file) {
        Serial.println("Ошибка сохранения файла");
        return false;
    }
    
    serializeJsonPretty(doc, file);
    file.close();
    
    Serial.println("Статус карты " + cardId + " успешно обновлён");
    return true;
}

void setup() {
  Serial.begin(115200);
  SPI.begin();
  rfid.PCD_Init();
  pinMode(RED, OUTPUT);
  pinMode(GRN, OUTPUT);
  // 1. Запуск файловой системы
  if (!LittleFS.begin(true)) {
    Serial.println("Ошибка LittleFS!");
    return;
  }

  // 2. Попытка запустить часы 
  if (!rtc.begin()) {
    Serial.println("Модуль RTC не найден (пока работаем без него)");
  }

  // 3. Запуск Wi-Fi в режиме точки доступа
  WiFi.softAP(ap_ssid, ap_password);
  Serial.print("Wi-Fi запущен. IP адрес: ");
  Serial.println(WiFi.softAPIP());


  if (LittleFS.exists("/config.json")) {
      File configFile = LittleFS.open("/config.json", "r");
      DynamicJsonDocument doc(256);
      deserializeJson(doc, configFile);
      banStartMin = doc["banStartMin"] | 1320; // 1320 - значение по умолчанию
      banEndMin = doc["banEndMin"] | 360;
      configFile.close();
      Serial.println("Настройки загружены из памяти");
    }

  // 4. Настройка веб-сервера
 
  // Главная страница админки
  server.on("/dashboard", HTTP_GET, [](AsyncWebServerRequest *request){
  request->send(LittleFS, "/dashboard.html", "text/html"); //отправили данные из внутр. хранилища
  });

  // Отдача данных о студентах на сайт
  server.on("/get_students", HTTP_GET, [](AsyncWebServerRequest *request){
  request->send(LittleFS, "/students.json", "application/json");  //указали, что это структурированные данные(модель) 
  });

  //Для кнопки добавления новых айди
  server.on("/get_last_id", HTTP_GET,[](AsyncWebServerRequest *request){ 
  request->send(200, "text/plain", lastScannedID);   //отправляем текст из переменной
  });

//добавляем новых студентов
  server.on("/add_student", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
    [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
    // 2. Парсим в переменную incomingData
    DynamicJsonDocument incomingData(512);
    DeserializationError error = deserializeJson(incomingData, data, len);
    
    if(error) {
        request->send(400, "text/plain", "Ошибка JSON");
        return;
    }
    
    // 3. Проверяем обязательные поля
    if(!incomingData.containsKey("id") || 
       !incomingData.containsKey("first_name") || 
       !incomingData.containsKey("last_name") || 
       !incomingData.containsKey("class") ||
       !incomingData.containsKey("active")) {
        request->send(400, "text/plain", "Нет всех полей");
        return;
    }
    
    // 4. Извлекаем айди
    String cardId = incomingData["id"].as<String>();
    Serial.println("Добавляю карту: " + cardId);
    
    // 5. Читаем файл
    File file = LittleFS.open("/students.json", "r");
    if(!file) {
        request->send(500, "text/plain", "Файл не найден");
        return;
    }

    DynamicJsonDocument studentsFile(4096);
    error = deserializeJson(studentsFile, file);
    file.close();
    
    if(error) {
        request->send(500, "text/plain", "Ошибка файла");
        return;
    }
    // Проверяем ID
    JsonArray students = studentsFile["students"];
    for(JsonObject student : students) {
        if(student["id"].as<String>() == cardId) {
            request->send(400, "text/plain", "Карта уже есть");
            return;
        }
    }

    students.add(incomingData.as<JsonObject>());
    
    file = LittleFS.open("/students.json", "w");
    if(!file) {
        request->send(500, "text/plain", "Ошибка сохранения");
        return;
    }
    serializeJsonPretty(studentsFile, file);
    file.close();
    Serial.println("Ученик добавлен: " + cardId);
    request->send(200, "text/plain", "Ученик добавлен");
});

    //эндпоинт для сметы статуса
server.on("/update_status", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL,
  [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
   
    DynamicJsonDocument doc(512); // Переименовали из data в doc
    DeserializationError error = deserializeJson(doc, data, len); // Теперь передаем data (массив байт)
   
    if(error) {
        request->send(400, "application/json", "{\"error\":\"Ошибка JSON\"}");
        return;
    }

    if(!doc.containsKey("id") || !doc.containsKey("active")) {
        request->send(400, "application/json", "{\"error\":\"Нет полей id или active\"}");
        return;
    }

    String cardId = doc["id"].as<String>();
    bool newStatus = doc["active"].as<bool>();
   
    bool success = updateStudentStatus(cardId, newStatus);
    if(success) {
        request->send(200, "application/json", "{\"success\":true}");
    } else {
        request->send(404, "application/json", "{\"error\":\"Not found\"}");
    }
});
// Экстренные команды
server.on("/command", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasArg("action")) {
    String action = request->arg("action");
    if (action == "EMERGENCY_OPEN") {
      emergencyOpen = true;
      emergencyClose = false;
      digitalWrite(GRN, 1);
      digitalWrite(RED, 0);
    } else if (action == "EMERGENCY_CLOSE") {
      emergencyOpen = false;
      emergencyClose = true;
      digitalWrite(RED, 1);
      digitalWrite(GRN, 0);
    }
  }
  request->send(200, "text/plain", "OK");
});

// Установка времени
server.on("/set_ban", HTTP_GET, [](AsyncWebServerRequest *request) {
  if (request->hasArg("start") && request->hasArg("end")) {
    String start = request->arg("start");
    String end = request->arg("end");
   
    banStartMin = start.substring(0, 2).toInt() * 60 + start.substring(3, 5).toInt();
    banEndMin = end.substring(0, 2).toInt() * 60 + end.substring(3, 5).toInt();

    DynamicJsonDocument configDoc(256);
    configDoc["banStartMin"] = banStartMin;
    configDoc["banEndMin"] = banEndMin;
   
    File configFile = LittleFS.open("/config.json", "w");
    serializeJson(configDoc, configFile);
    configFile.close();

    request->send(200, "text/plain", "Saved");
  } else {
    request->send(400, "text/plain", "Missing args");
  }
});


  server.begin();
  Serial.println("Сервер запущен!");
}

// Основная логика проверки карты(функция)
bool checkStudent(String uid) {
  File file = LittleFS.open("/students.json", "r");
  if (!file) {
    Serial.println("Файл students.json не найден!");
    return false;
  }
 
  DynamicJsonDocument doc(4096);           //Создаем документ
  DeserializationError error = deserializeJson(doc, file);
  file.close();

  if (error) {
    Serial.println("Ошибка чтения JSON!");
    return false;
  }

  JsonArray students = doc["students"];
  bool found = false;

  for (JsonObject s : students) {
    if (s["id"] == uid) {
      found = true;
     
      // Проверка активности
      if (s["active"] == false) {
        Serial.println("ДОСТУП ЗАПРЕЩЕН: Карта заблокирована.");
        digitalWrite(RED, 1);
        delay(2000);
        digitalWrite(RED, 0);
        return false;
      }

      // Добавляем запись в историю
      JsonArray history = s["history"];
      if (history.size() >= 10) history.remove(0); // Ограничение в 10 записей
      history.add(getTimestamp());

      // Сохраняем обновленный JSON обратно в файл
      File outFile = LittleFS.open("/students.json", "w");
      serializeJson(doc, outFile);
      outFile.close();

      Serial.print("Доступ открыт: ");
      Serial.print(s["last_name"].as<String>());
      Serial.print(" ");
      Serial.println(s["first_name"].as<String>());
      digitalWrite(GRN, 1);
      delay(2000);
      digitalWrite(GRN, 0);
      return true;
    }
  }

  if (!found) {
    Serial.println("ДОСТУП ЗАПРЕЩЕН: Неизвестная карта.");
    digitalWrite(RED, 1);
    delay(2000);  
    digitalWrite(RED, 0);
  }
  return false;
}
//Функция проверки всех временных условий 
bool isAccessAllowed() { 
  if (emergencyOpen) return true; 
  if (emergencyClose) return false; 
  DateTime now = rtc.now(); 
  int currentMin = now.hour() * 60 + now.minute(); 
  if (banStartMin > banEndMin) { // Если интервал через полночь 
    if (currentMin >= banStartMin || currentMin < banEndMin) return false; 
  } else {
    if (currentMin >= banStartMin && currentMin < banEndMin) return false; 
  } 
  return true; 
}  

void loop() {

  if (!rfid.PICC_IsNewCardPresent() || !rfid.PICC_ReadCardSerial()) return;

  String uid = "";
  for (byte i = 0; i < rfid.uid.size; i++) {
    uid += String(rfid.uid.uidByte[i] < 0x10 ? "0" : "");
    uid += String(rfid.uid.uidByte[i], HEX);
  }
  uid.toUpperCase();
  lastScannedID = uid;
  Serial.print("Считан ID: ");
  Serial.println(uid);

  if (isAccessAllowed()) {
    checkStudent(uid);
  } else {

    Serial.println("Доступ запрещен настройками времени.");
    digitalWrite(RED, 1);
    delay(2000);
    digitalWrite(RED, 0);
  }

//сброс РФИДа
  rfid.PICC_HaltA();
  rfid.PCD_StopCrypto1();
  delay(500); 
}