#include <MIDIUSB.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
//ASW_MC-422s  micro.build.pid=0x8038

const int potPins[4] = {A0, A1, A2, A3}; // 팟 핀 (4개)
const int switchPins[] = {8, 4, 6};        // 스위치 핀: 프리셋 스위치, Red, Green
const int ledPins[] = {9, 5, 7};           // LED 핀: 팟 LED, Red LED, Green LED

// 각 팟별 개별 임계치(threshold)를 관리
int thresholds[4] = {0, 0, 0, 0};
const int screenAddress = 0x3C;

int prevPotValues[4] = {0, 0, 0, 0};
int smoothedAverage[4] = {0, 0, 0, 0};
// 프리셋 1과 프리셋 2의 MIDI 값을 저장 (총 8개)
byte receivedMidiValues[8] = {0, 0, 0, 0, 0, 0, 0, 0};
byte currentPreset = 1;
boolean presetSwitched = false;
bool toggleModes[] = {true, true};         // 토글 모드 (스위치 1: 토글, 스위치 2: 모멘터리)
bool ledStates[] = {false, false, false};     // LED 상태 배열
bool buttonStates[] = {false, false, false};
bool lastButtonStates[] = {false, false, false};

// 소프트웨어 디바운싱 변수
unsigned long lastDebounceTimes[] = {0, 0, 0};
unsigned long debounceDelay = 10;  // 디바운싱 시간 (필요시 20~50ms로 조정 가능)

// MIDI 설정
const byte ccNumbers[8] = {12, 84, 91, 88, 30, 31, 32, 33};
const byte channels[8] = {6, 6, 0, 12, 4, 4, 4, 4};
const char* potNames[8] = {"PICH", "GAIN", "CLIK", "+DRY", "CUT ", "ENV ", "OSC ", "AIR "};
const byte ccNumbersSwitch[] = {2, 108};    // 스위치 관련 MIDI CC 번호
const byte midiChannelsSwitch[] = {13, 0};    // 스위치 관련 MIDI 채널

Adafruit_SSD1306 display(128, 32, &Wire);

// 각 팟별 마지막 움직임 시간을 개별적으로 관리
unsigned long lastPotMoveTimes[4] = {0, 0, 0, 0};

unsigned long previousMillis = 0;
unsigned long ledOffMillis = 0;
unsigned long lastDisplayUpdate = 0;
const unsigned long displayInterval = 80;  // 디스플레이 업데이트 간격 (ms)
int ledState = LOW;
int blinkInterval = 1000;
boolean potValueChanged = false;
boolean displayUpdated = false;

float alpha = 0.1; // 지수 이동평균 평활화 상수

//────────────────────────────────────────
// 사용자 조작(팟/페이지) 비활성 감지 관련 변수  
unsigned long lastUserInteractionTime = 0;
const unsigned long userInactivityPeriod = 180000; // 5분 = 300,000ms
bool displayDimmed = false;
//────────────────────────────────────────

//────────────────────────────────────────
// 밝기 조정을 위한 contrast 값 설정 (0 ~ 255)
// activeContrast: 활동 시 밝기 (예: 255)
// dimContrast: 비활성 시(어둡게) 밝기 (예: 30)
const uint8_t activeContrast = 255;
const uint8_t dimContrast    = 20;

void setDisplayContrast(uint8_t contrast) {
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(contrast);
}
//────────────────────────────────────────

void setup() {
  // 스위치와 LED 핀 설정
  for (int i = 0; i < 3; i++) {
    pinMode(switchPins[i], INPUT_PULLUP); // 내부 풀업 사용
    pinMode(ledPins[i], OUTPUT);
  }
  
  Wire.begin();

  if (!display.begin(SSD1306_SWITCHCAPVCC, screenAddress)) {
    while (true);  // 디스플레이 초기화 실패 시 무한 대기
  }
    
  display.clearDisplay();

  // 초기 팟 값 설정 및 상태 초기화
  for (int i = 0; i < 4; i++) {
    int initialReading = analogRead(potPins[i]);
    smoothedAverage[i] = initialReading;
    int initialCC = map(initialReading, 0, 1023, 0, 129);
    initialCC = constrain(initialCC, 0, 127);
    prevPotValues[i] = initialCC;
    thresholds[i] = 0;              // 초기 민감도는 0
    lastPotMoveTimes[i] = millis(); // 각 팟의 초기 시간
  }

  lastUserInteractionTime = millis(); // 초기 사용자 인터랙션 시간 설정
  
  displayUpdated = true;
  updateDisplay();

  // 초기 밝기 설정 (활성 상태 contrast)
  setDisplayContrast(activeContrast);
}

void sendMidiCC(byte channel, byte cc, byte value) {
  // 팟 움직임에 따른 사용자 조작 발생 시각 업데이트
  lastUserInteractionTime = millis();
  
  midiEventPacket_t event = {0x0B, 0xB0 | channel, cc, value};
  MidiUSB.sendMIDI(event);
  MidiUSB.flush();
  
  // 스위치에 의한 메시지라면 LED 제어 생략
  if (cc == ccNumbersSwitch[0] || cc == ccNumbersSwitch[1]) {
    return;
  }

  // LED 깜빡임 처리 (팟 값에 따라)
  if (value >= 127) {
    digitalWrite(ledPins[0], HIGH);
    blinkInterval = 0;
  } else if (value > 0) {
    blinkInterval = map(value, 0, 126, 320, 20);
  } else {
    blinkInterval = 0;
    digitalWrite(ledPins[0], LOW);
  }

  potValueChanged = true;
  ledOffMillis = millis();
  displayUpdated = true;
}

void handleLED() {
  if (blinkInterval > 0) {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= blinkInterval) {
      previousMillis = currentMillis;
      ledState = !ledState;
      digitalWrite(ledPins[0], ledState);
    }
  }

  if (!potValueChanged && millis() - ledOffMillis >= 2000) {
    digitalWrite(ledPins[0], LOW);
  } else {
    potValueChanged = false;
  }
}

void drawBoldText(int x, int y, const char* text) {
  for (int i = 0; i < strlen(text); i++) {
    display.setCursor(x + i * 6, y);
    display.print(text[i]);

    display.setCursor(x + i * 6 + 1, y); // 살짝 오른쪽 오프셋
    display.print(text[i]);
  }
}

void updateDisplay() {
  if (!displayUpdated) return;

  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // 프리셋 번호 출력
  display.setCursor(0, 0);
  display.print(currentPreset + 1);
  display.print(" ");

  // 프리셋 팟 이름 중앙 정렬 출력
  int nameLineWidth = 0;
  for (int i = 0; i < 4; i++) {
    nameLineWidth += strlen(potNames[currentPreset * 4 + i]) * 6 + 4;
  }
  nameLineWidth -= 6;
  int nameStartX = 2 + (128 - nameLineWidth) / 2;
  display.setCursor(nameStartX, 0);
  for (int i = 0; i < 4; i++) {
    display.print(potNames[currentPreset * 4 + i]);
    if (i < 3) display.print(" ");
  }

  // 두 번째 줄: 지수 이동평균 값 (P:)
  display.setCursor(0, 14);
  display.print("P:");
  for (int i = 0; i < 4; i++) {
    int adjustedValue = map(smoothedAverage[i], 0, 1023, 0, 129);
    adjustedValue = constrain(adjustedValue, 0, 127);
    char paddedValue[4];
    sprintf(paddedValue, "%3d", adjustedValue);
    drawBoldText(display.getCursorX(), display.getCursorY(), paddedValue);
    display.print("  ");
  }

  // 세 번째 줄: 현재 프리셋 수신 MIDI 값 (M:)
  display.setCursor(0, 25);
  display.print("M:");
  for (int i = 0; i < 4; i++) {
    char paddedValue[4];
    sprintf(paddedValue, "%3d", receivedMidiValues[currentPreset * 4 + i]);
    drawBoldText(display.getCursorX(), display.getCursorY(), paddedValue);
    display.print("  ");
  }

  // 네 번째 줄: 다른 프리셋 수신 MIDI 값 (I:)
  display.setCursor(0, 38);
  display.print("I:");
  for (int i = 0; i < 4; i++) {
    char paddedValue[4];
    int otherPreset = (currentPreset == 0) ? 1 : 0;
    sprintf(paddedValue, "%3d", receivedMidiValues[otherPreset * 4 + i]);
    drawBoldText(display.getCursorX(), display.getCursorY(), paddedValue);
    display.print("  ");
  }

  display.display();
  displayUpdated = false;
}

void receiveMidiCC() {
  midiEventPacket_t rx = MidiUSB.read();

  if (rx.header != 0) {
    if (rx.header == 0x0B) {
      byte channel = rx.byte1 & 0x0F;
      byte cc = rx.byte2;
      byte value = rx.byte3;

      for (int i = 0; i < 8; i++) {
        if (channels[i] == channel && ccNumbers[i] == cc) {
          receivedMidiValues[i] = value;
          break;
        }
      }

      for (int i = 0; i < 2; i++) {
        if (channel == midiChannelsSwitch[i] && cc == ccNumbersSwitch[i]) {
          ledStates[i + 1] = (value > 0); // CC 값에 따른 LED 상태 동기화
          digitalWrite(ledPins[i + 1], ledStates[i + 1]);
        }
      }

      displayUpdated = true;
    }
  }
}

void handleSwitches() {
  for (int i = 0; i < 2; i++) {
    int reading = !digitalRead(switchPins[i + 1]);  // 스위치 논리 반전 (INPUT_PULLUP)

    if (reading != lastButtonStates[i + 1]) {
      lastDebounceTimes[i + 1] = millis();
    }

    if ((millis() - lastDebounceTimes[i + 1]) > debounceDelay) {
      if (reading != buttonStates[i + 1]) {
        buttonStates[i + 1] = reading;
        
        // 스위치 동작 시 사용자 조작 발생 시각 업데이트
        lastUserInteractionTime = millis();
        
        if (toggleModes[i]) {
          if (buttonStates[i + 1]) {
            ledStates[i + 1] = !ledStates[i + 1];
            sendMidiCC(midiChannelsSwitch[i], ccNumbersSwitch[i], ledStates[i + 1] ? 127 : 0);
          }
        } else {
          if (buttonStates[i + 1]) {
            ledStates[i + 1] = true;
            sendMidiCC(midiChannelsSwitch[i], ccNumbersSwitch[i], 127);
          } else {
            ledStates[i + 1] = false;
            sendMidiCC(midiChannelsSwitch[i], ccNumbersSwitch[i], 0);
          }
        }

        digitalWrite(ledPins[i + 1], ledStates[i + 1]);
      }
    }
    lastButtonStates[i + 1] = reading;
  }
}

void loop() {
  receiveMidiCC();
  handleSwitches();

  // 프리셋(페이지) 스위치 처리
  if (digitalRead(switchPins[0]) == LOW && currentPreset != 0) {
    currentPreset = 0;
    presetSwitched = true;
  } else if (digitalRead(switchPins[0]) == HIGH && currentPreset != 1) {
    currentPreset = 1;
    presetSwitched = true;
  }

  // 프리셋 전환 시 각 팟의 기준값, threshold, 마지막 움직임 시간을 초기화
  if (presetSwitched) {
    for (int i = 0; i < 4; i++) {
      int initialReading = analogRead(potPins[i]);
      smoothedAverage[i] = initialReading;
      int initialCC = map(initialReading, 0, 1023, 0, 129);
      initialCC = constrain(initialCC, 0, 127);
      prevPotValues[i] = initialCC;
      thresholds[i] = 5;           // 페이지 전환 후 해당 팟은 최소 5 이상의 변화가 있어야 전송
      lastPotMoveTimes[i] = millis();
    }
    presetSwitched = false;
    displayUpdated = true;
    updateDisplay();
    // 프리셋 변경 시 사용자 조작 발생 시각 업데이트
    lastUserInteractionTime = millis();
  }

  // 각 팟별 아날로그 값을 읽고 변화량(diff)을 계산하여 MIDI 전송 조건 판단
  for (int i = 0; i < 4; i++) {
    int potValue = analogRead(potPins[i]);
    smoothedAverage[i] = alpha * potValue + (1 - alpha) * smoothedAverage[i];
    int adjustedValue = map(smoothedAverage[i], 0, 1023, 0, 129);
    adjustedValue = constrain(adjustedValue, 0, 127);
    int diff = abs(adjustedValue - prevPotValues[i]);

    // 만약 페이지 전환 후 threshold가 5이면, 5 이상의 변화여야 전송함
    if (thresholds[i] == 5) {
      if (diff >= 5) { // 5 이상의 변화가 있어야 CC 전송
        sendMidiCC(channels[currentPreset * 4 + i], ccNumbers[currentPreset * 4 + i], adjustedValue);
        prevPotValues[i] = adjustedValue;
        lastPotMoveTimes[i] = millis();
        displayUpdated = true;
        thresholds[i] = 0; // 전송 후 다시 민감도 0으로 복귀
      }
    } else {
      if (diff > thresholds[i]) {
        sendMidiCC(channels[currentPreset * 4 + i], ccNumbers[currentPreset * 4 + i], adjustedValue);
        prevPotValues[i] = adjustedValue;
        lastPotMoveTimes[i] = millis();
        displayUpdated = true;
        thresholds[i] = 0;
      } else if (millis() - lastPotMoveTimes[i] > 2000) {
        thresholds[i] = 1; // 일정 시간 동안 변화 없으면 민감도를 낮춤 (노이즈 필터링)
      }
    }
  }

  handleLED();

  //────────────────────────────
  // 사용자 활동 비활성(5분 이상) 시 밝기(contrast) 조절
  unsigned long currentTime = millis();
  if (currentTime - lastUserInteractionTime >= userInactivityPeriod) {
    if (!displayDimmed) {
      setDisplayContrast(dimContrast);
      displayDimmed = true;
      displayUpdated = true;
    }
  } else {
    if (displayDimmed) {
      setDisplayContrast(activeContrast);
      displayDimmed = false;
      displayUpdated = true;
    }
  }
  //────────────────────────────

  // 주기적으로 디스플레이 업데이트
  if (currentTime - lastDisplayUpdate >= displayInterval) {
    lastDisplayUpdate = currentTime;
    if (displayUpdated) {
      updateDisplay();
    }
  }
}