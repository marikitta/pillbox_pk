#include <Wire.h>
#include <RTClib.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>


// ------------------- Пины (MEGA) -------------------
const byte LED_PINS[7]   = {22, 23, 24, 25, 26, 27, 28};

// Герконы NO: один вывод на GND, второй на пин (INPUT_PULLUP)
// закрыт -> LOW, открыт -> HIGH
const byte REED_PINS[7]  = {30, 31, 32, 33, 34, 35, 36};

const byte BUZZER_PIN    = 44;

// 3 кнопки (влево, ОК, вправо), INPUT_PULLUP:
const byte BTN_LEFT      = 41;
const byte BTN_OK        = 40;   // долгий hold = настройки
const byte BTN_RIGHT     = 42;
// ---------------------------------------------------

RTC_DS3231 rtc;
LiquidCrystal_I2C lcd(0x27, 16, 2); // если не показывает — попробуйте 0x3F

// ---------- Логика времени ----------
struct CellTime {
  byte hour;
  byte minute;  // только 0,15,30,45
};

byte cellsCount = 4;                  // по умолчанию 4 ячейки
CellTime cells[7];                    // времена ячеек
bool cellDoneToday[7];                // уже приняли сегодня?

// Дефолтные пробные времена (для 4 ячеек)
const CellTime defaultTimes[4] = {
  {15, 8},
  {15, 9},
  {15, 10},
  {15, 34}
};

// ---------- Состояния ----------
enum Mode {
  MODE_WORK,
  MODE_SETTINGS_CELLS,
  MODE_SETTINGS_TIME_H,
  MODE_SETTINGS_TIME_M,
  MODE_SETTINGS_CONFIRM,
  MODE_ALERT,
  MODE_DEMO,
  MODE_ALERT_DEMO
};

Mode mode = MODE_WORK;
int settingIndex = 0;          // номер настраиваемой ячейки (0..cellsCount-1)
byte tempHour = 8;
byte tempMinute = 0;

// ---------- Для кнопок ----------
unsigned long okPressStart = 0;
unsigned long sidePressStart = 0;
unsigned long start_time_demo = 0;
unsigned long current_time_demo=0;
bool okHolding = false;

unsigned long lrftPressStart = 0;
bool leftHolding = false;

unsigned long rightPressStart = 0;
bool rightHolding = false;



// ---------- Для сигнала ----------
int activeCell = -1;
bool demoActive = false;
byte demoIndex = 0;
unsigned long lastBlink = 0;
bool blinkOn = false;
unsigned long lastBeep = 0;
bool beepOn = false;

// ---------- EEPROM адреса ----------
const int EEPROM_MAGIC_ADDR = 0;
const byte EEPROM_MAGIC = 0x42;
const int EEPROM_CELLS_ADDR = 1;   // 1 байт
const int EEPROM_TIMES_ADDR = 2;   // далее пары {hour, minute} * 7

// ----------------- Сервис -----------------
void loadSettings() {
  if (EEPROM.read(EEPROM_MAGIC_ADDR) != EEPROM_MAGIC) {
    // первое включение — загрузим дефолт
    cellsCount = 4;
    for (int i=0;i<4;i++) cells[i]=defaultTimes[i];
    for (int i=4;i<7;i++) cells[i]={0,0};
    return;
  }
  cellsCount = EEPROM.read(EEPROM_CELLS_ADDR);
  if (cellsCount < 1 || cellsCount > 7) cellsCount = 4;
  int addr = EEPROM_TIMES_ADDR;
  for (int i=0;i<7;i++) {
    cells[i].hour = EEPROM.read(addr++);
    cells[i].minute = EEPROM.read(addr++);
  }
}

void saveSettings() {
  EEPROM.write(EEPROM_MAGIC_ADDR, EEPROM_MAGIC);
  EEPROM.write(EEPROM_CELLS_ADDR, cellsCount);
  int addr = EEPROM_TIMES_ADDR;
  for (int i=0;i<7;i++) {
    EEPROM.write(addr++, cells[i].hour);
    EEPROM.write(addr++, cells[i].minute);
  }
}

void resetDailyFlags() {
  for (int i=0;i<7;i++) cellDoneToday[i]=false;
}

void print2(byte v) {
  if (v<10) lcd.print('0');
  lcd.print(v);
}

void showTime(DateTime now) {
  lcd.setCursor(0,0);
  print2(now.hour()); lcd.print(':'); print2(now.minute()); lcd.print(':'); print2(now.second());
  lcd.print("   ");
  lcd.setCursor(0,1);
  lcd.print("Date ");
  print2(now.day()); lcd.print('.'); print2(now.month()); lcd.print('.'); lcd.print(now.year());
}

// Массив разрешённых минут
const byte minuteOptions[4] = {0,15,30,45};
int minuteIndex(byte m) {
  for (int i=0;i<4;i++) if (minuteOptions[i]==m) return i;
  return 0;
}

bool readBtn(byte pin) {
  return digitalRead(pin) == LOW; // т.к. INPUT_PULLUP
}

// ---------------- Setup ----------------
void setup() {
  Wire.begin();
  rtc.begin();
  Serial.begin(9600);

  lcd.init();
  lcd.backlight();

  for (int i=0;i<7;i++) pinMode(LED_PINS[i], OUTPUT);
  for (int i=0;i<7;i++) pinMode(REED_PINS[i], INPUT_PULLUP);
  //Serial.println(REED_PINS[3]);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(BTN_LEFT, INPUT_PULLUP);
  pinMode(BTN_OK, INPUT_PULLUP);
  pinMode(BTN_RIGHT, INPUT_PULLUP);

  //loadSettings();
  resetDailyFlags();

  // если RTC сброшены — выставим время прошивки
  if (rtc.lostPower()) {
    rtc.adjust(DateTime(F(__DATE__), F(__TIME__)));
  }

  lcd.clear();
}

// ---------------- Loop ----------------
void loop() {
  DateTime now = rtc.now();
// сброс флагов в полночь
  static int lastDay = -1;
  if (now.day() != lastDay) {
    lastDay = now.day();
    resetDailyFlags();
  }

  handleOkHold();
  hendeltwoHold(now);
  switch(mode) {
    case MODE_WORK:
      workMode(now);
      break;
    case MODE_DEMO:
    case MODE_SETTINGS_CELLS:
    case MODE_SETTINGS_TIME_H:
    case MODE_SETTINGS_TIME_M:
    case MODE_SETTINGS_CONFIRM:
      settingsMode();
      break;
    case MODE_ALERT:
      alertMode(now);
      break;
    case MODE_ALERT_DEMO:
      alertModeDemo(now);
      break;
    
  }
}

// ------------ Долгое удержание OK ------------
void handleOkHold() {
  bool ok = readBtn(BTN_OK);

  if (ok && !okHolding) {
    okHolding = true;
    okPressStart = millis();
  }
  if (!ok && okHolding) {
    okHolding = false;
  }

  if (okHolding && millis() - okPressStart > 2000 && mode == MODE_WORK) {
    mode = MODE_SETTINGS_CELLS;
    //Serial.println("Режим настройки");
    lcd.clear();
    delay(200);
    okHolding = false;
    while (readBtn(BTN_OK)){  }
  }
}

void hendeltwoHold(DateTime now){
  bool left = readBtn(BTN_LEFT);

  if(left  && !leftHolding ) {
    leftHolding = true;
    sidePressStart = millis();
  }
  if (!left && leftHolding ){
      leftHolding = false;
  }
  if (leftHolding && millis() - sidePressStart > 2000 && mode == MODE_WORK) {
    demoActive = true;
    demoIndex = 0;
    for (int i=0; i<5; i++) digitalWrite(LED_PINS[i], LOW);
    blinkOn = false; 
    beepOn = false; 
    lastBlink = millis();
    lastBeep = millis();
    activeCell = demoIndex;
    mode = MODE_ALERT_DEMO;
    lcd.clear();
    lcd.setCursor(0,0);
    lcd.print("DEMO_MODE");
    leftHolding = false;
    while (readBtn(BTN_LEFT)){  }

    }
  }
  

// ------------ Рабочий режим ------------
void workMode(DateTime now) {
  showTime(now);

  for (int i=0;i<cellsCount;i++) {
    if (!cellDoneToday[i] &&
        now.hour() == cells[i].hour &&
        now.minute() == cells[i].minute &&
        now.second() == 0)
    {
      activeCell = i;
      mode = MODE_ALERT;
      lcd.clear();
      break;
    }
  }
}

// ------------ Режим тревоги ------------
void alertMode(DateTime now) {
  // мигание нужного светодиода
  if (millis() - lastBlink > 400) {
    lastBlink = millis();
    blinkOn = !blinkOn;
    digitalWrite(LED_PINS[activeCell], blinkOn);
  }

  // прерывистый звук
  if (millis() - lastBeep > 500) {
    lastBeep = millis();
    beepOn = !beepOn;
    if (beepOn) tone(BUZZER_PIN, 2000);
    else noTone(BUZZER_PIN);
  }

  lcd.setCursor(0,0);
  lcd.print("Take pill cell ");
  lcd.print(activeCell+1);
  lcd.print(" ");

  lcd.setCursor(0,1);
  print2(now.hour()); lcd.print(':'); print2(now.minute());
  lcd.print(" wait open.. ");

  // геркон нужного ящика
  bool opened = digitalRead(REED_PINS[activeCell]) == HIGH; // открыт -> HIGH
  if (opened) {
    stopAlertAndBack();
  }
}


void alertModeDemo(DateTime now) {
  // мигание нужного светодиода
  if (millis() - lastBlink > 400) {
    lastBlink = millis();
    blinkOn = !blinkOn;
    digitalWrite(LED_PINS[activeCell], blinkOn);
  }

  // прерывистый звук
  if (millis() - lastBeep > 500) {
    lastBeep = millis();
    beepOn = !beepOn;
    if (beepOn) tone(BUZZER_PIN, 2000);
    else noTone(BUZZER_PIN);
  }

  lcd.setCursor(0,0);
  lcd.print("DEMO Take pill cell ");
  Serial.println(activeCell);
  lcd.print(activeCell+1);
  lcd.print(" ");

  lcd.setCursor(0,1);
  print2(now.hour()); lcd.print(':'); print2(now.minute());
  lcd.print(" wait open.. ");

  // геркон нужного ящика
  bool opened = digitalRead(REED_PINS[activeCell]) == HIGH; // открыт -> HIGH
  if (opened) {
    stopAlertAndBackDemo();
  }
}


void stopAlertAndBack() {
  noTone(BUZZER_PIN);
  for (int i=0;i<7;i++) digitalWrite(LED_PINS[i], LOW);
  cellDoneToday[activeCell] = true;
  activeCell = -1;
  mode = MODE_WORK;
  lcd.clear();
}

void stopAlertAndBackDemo() {
  noTone(BUZZER_PIN);
  for (int i=0;i<4;i++) digitalWrite(LED_PINS[i], LOW);
  demoIndex++;
  if (demoIndex >= cellsCount){
    demoActive = false;
    activeCell = -1;
    mode = MODE_WORK;
    lcd.clear();
    return;
  }
  activeCell = demoIndex;
  blinkOn = false;
  beepOn = false;
  lastBlink = millis();
  lastBeep = millis();
  mode = MODE_ALERT_DEMO;
  lcd.clear();
}

// ------------ Настройки ------------
void settingsMode() {
  bool left  = readBtn(BTN_LEFT);
  bool ok    = readBtn(BTN_OK);
  bool right = readBtn(BTN_RIGHT);

  static bool lastLeft=0, lastOk=0, lastRight=0;
  bool leftClick  = left  && !lastLeft;
  bool okClick    = ok    && !lastOk;
  bool rightClick = right && !lastRight;
  lastLeft = left; lastOk = ok; lastRight = right;

  if (mode == MODE_SETTINGS_CELLS) {
    lcd.setCursor(0,0);
    lcd.print("How many cells?");
    lcd.setCursor(0,1);
    lcd.print("Cells: ");
    lcd.print(cellsCount);
    lcd.print("   ");

    if (leftClick && cellsCount>1) cellsCount--;
    if (rightClick && cellsCount<7) cellsCount++;

    if (okClick) {
      settingIndex = 0;
      tempHour = cells[0].hour;
      tempMinute = cells[0].minute;
      mode = MODE_SETTINGS_TIME_H;
      lcd.clear();
    }
  }

  else if (mode == MODE_SETTINGS_TIME_H) {
    lcd.setCursor(0,0);
    lcd.print("Cell ");
    lcd.print(settingIndex+1);
    lcd.print(" hour:");
    lcd.setCursor(0,1);
    lcd.print("Hour: ");
    lcd.print(tempHour);
    lcd.print("   ");

    if (leftClick)  tempHour = (tempHour==0)?23:tempHour-1;
    if (rightClick) tempHour = (tempHour==23)?0:tempHour+1;

    if (okClick) {
      mode = MODE_SETTINGS_TIME_M;
      lcd.clear();
    }
  }

  else if (mode == MODE_SETTINGS_TIME_M) {
    int mi = minuteIndex(tempMinute);
    lcd.setCursor(0,0);
    lcd.print("Cell ");
    lcd.print(settingIndex+1);
    lcd.print(" minute:");
    lcd.setCursor(0,1);
    lcd.print("Min: ");
    lcd.print(minuteOptions[mi]);
    lcd.print("   ");

    if (leftClick)  mi = (mi==0)?3:mi-1;
    if (rightClick) mi = (mi==3)?0:mi+1;
    tempMinute = minuteOptions[mi];

    if (okClick) {
      cells[settingIndex].hour = tempHour;
      cells[settingIndex].minute = tempMinute;
settingIndex++;
      if (settingIndex >= cellsCount) {
        mode = MODE_SETTINGS_CONFIRM;
        lcd.clear();
      } else {
        tempHour = cells[settingIndex].hour;
        tempMinute = cells[settingIndex].minute;
        mode = MODE_SETTINGS_TIME_H;
        lcd.clear();
      }
    }
  }

  else if (mode == MODE_SETTINGS_CONFIRM) {
    lcd.setCursor(0,0);
    lcd.print("Confirm settings?");
    lcd.setCursor(0,1);
    lcd.print("<Yes   No>");

    if (leftClick) {
      mode = MODE_SETTINGS_CELLS;
      lcd.clear();
    }
    if (rightClick || okClick) {
      saveSettings();
      resetDailyFlags();
      mode = MODE_WORK;
      lcd.clear();
    }
  }
}
