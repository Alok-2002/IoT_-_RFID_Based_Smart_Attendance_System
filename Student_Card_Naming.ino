/* RFID named-authorize system
   - MFRC522 (SS_PIN=10, RST_PIN=9)
   - LED_PIN lights continuously for authorized card
   - BUZZER_PIN produces continuous tone while authorized card present
   - Serial commands (while card on reader): 
       'a' add (prompts for name), 
       'r' remove, 
       'p' print list, 
       'c' clear all
*/

#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>

#define SS_PIN 10
#define RST_PIN 9
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Hardware
const int LED_PIN = 8;
const int BUZZER_PIN = 7;

// EEPROM/Storage configuration
const int EEPROM_ADDR_COUNT = 0;    // 1 byte = number of stored cards
const int EEPROM_ADDR_START = 10;   // starting address for entries (leave small header space)
const int MAX_CARDS = 25;           // safe for Uno EEPROM (25 * ENTRY_SIZE fits into 1024B)
const int MAX_UID_LEN = 10;         // support up to 10-byte UIDs
const int MAX_NAME_LEN = 20;        // up to 20 characters names
const int ENTRY_SIZE = 1 + MAX_UID_LEN + 1 + MAX_NAME_LEN; // uidLen + uid bytes + nameLen + name bytes

// Card presence removal detection
const int REMOVAL_CHECKS = 6;
const int REMOVAL_DELAY_MS = 120;

// Runtime buffers
byte currentUID[MAX_UID_LEN];
byte currentUIDLen = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) { } // wait for Serial (helpful when using native USB boards)
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);

  // initialize count if invalid
  if (!countValid()) writeCount(0);

  Serial.println(F("RFID Named-Authorize System"));
  Serial.println(F("Commands (while card on reader): a=add, r=remove, p=print, c=clear"));
  Serial.println(F("Open Serial Monitor at 115200 baud."));
  Serial.println(F("Present card..."));
}

// --------------------- Main loop ---------------------
void loop() {
  // allow basic commands when no card present
  if (!mfrc522.PICC_IsNewCardPresent()) {
    if (Serial.available()) handleSerialNoCard();
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) return;

  // Read and store current UID
  currentUIDLen = mfrc522.uid.size;
  if (currentUIDLen > MAX_UID_LEN) currentUIDLen = MAX_UID_LEN;
  for (byte i = 0; i < currentUIDLen; i++) currentUID[i] = mfrc522.uid.uidByte[i];

  Serial.print(F("Card UID: "));
  printUID(currentUID, currentUIDLen);

  // If authorized -> continuously ON until removed
  int idx = findIndex(currentUID, currentUIDLen);
  if (idx >= 0) {
    // read name for display
    String name = readNameFromEEPROM(idx);
    Serial.print(F("Card belongs to: "));
    Serial.println(name);
    Serial.println(F("Access Granted ✔ (AUTHORIZED)"));

    digitalWrite(LED_PIN, HIGH);
    tone(BUZZER_PIN, 2000); // continuous tone

    // allow serial commands while card present (including remove)
    waitWhileCardPresentAllowSerial();
    // once removed turn off
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
  } else {
    Serial.println(F("Access Denied ✖ (UNAUTHORIZED)"));
    // allow add/remove/print while card present (user may press 'a' to add)
    waitWhileCardPresentAllowSerial();
  }

  mfrc522.PICC_HaltA();
  delay(150);
}

// --------------------- Serial handling when no card ---------------------
void handleSerialNoCard() {
  char c = Serial.read();
  if (c == 'p' || c == 'P') {
    printAuthorizedList();
  } else if (c == 'c' || c == 'C') {
    clearAuthorizedList();
    Serial.println(F("Authorized list cleared."));
  } // 'a' and 'r' require a card present
}

// Wait while card present. Accept Serial commands inside.
void waitWhileCardPresentAllowSerial() {
  int consecutiveNoCard = 0;
  while (true) {
    // Serial commands
    if (Serial.available()) {
      char ch = Serial.read();
      if (ch == 'a' || ch == 'A') {
        Serial.println(F("Add: Enter name (max 20 chars), then press ENTER:"));
        String name = readLineFromSerial(30000); // wait up to 30s for name
        name.trim();
        if (name.length() == 0) {
          Serial.println(F("No name entered. Add cancelled."));
        } else {
          if (addCardWithName(currentUID, currentUIDLen, name)) {
            Serial.print(F("Added: "));
            Serial.println(name);
          } else {
            Serial.println(F("Add failed (duplicate or list full)."));
          }
        }
      } else if (ch == 'r' || ch == 'R') {
        if (removeCard(currentUID, currentUIDLen)) {
          Serial.println(F("Card removed from authorized list."));
        } else {
          Serial.println(F("Remove failed (not found)."));
        }
      } else if (ch == 'p' || ch == 'P') {
        printAuthorizedList();
      } else if (ch == 'c' || ch == 'C') {
        clearAuthorizedList();
        Serial.println(F("Authorized list cleared."));
      }
    }

    // detect presence: try reading a card; if read fails repeatedly assume removed
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      consecutiveNoCard = 0;
    } else {
      consecutiveNoCard++;
      if (consecutiveNoCard > REMOVAL_CHECKS) break;
    }
    delay(REMOVAL_DELAY_MS);
  }
}

// --------------------- EEPROM helpers & storage ---------------------
bool countValid() {
  byte c = EEPROM.read(EEPROM_ADDR_COUNT);
  if (c <= MAX_CARDS) return true;
  return false;
}
void writeCount(byte v) {
  if (v > MAX_CARDS) v = MAX_CARDS;
  EEPROM.update(EEPROM_ADDR_COUNT, v);
}
byte readCount() {
  byte v = EEPROM.read(EEPROM_ADDR_COUNT);
  if (v > MAX_CARDS) return 0;
  return v;
}
int entryAddress(int index) {
  return EEPROM_ADDR_START + index * ENTRY_SIZE;
}

// Add card with name. returns true if added.
bool addCardWithName(byte *uid, byte uidLen, String name) {
  if (uidLen == 0) return false;
  if (name.length() > MAX_NAME_LEN) name = name.substring(0, MAX_NAME_LEN);
  byte count = readCount();
  if (count >= MAX_CARDS) return false;
  if (findIndex(uid, uidLen) >= 0) return false; // duplicate

  int addr = entryAddress(count);
  EEPROM.update(addr, uidLen); // store uidLen (1)
  // store UID padded to MAX_UID_LEN
  for (int i = 0; i < MAX_UID_LEN; i++) {
    byte b = (i < uidLen) ? uid[i] : 0;
    EEPROM.update(addr + 1 + i, b);
  }
  // store name length and name bytes (padded)
  byte nlen = name.length();
  EEPROM.update(addr + 1 + MAX_UID_LEN, nlen);
  for (int i = 0; i < MAX_NAME_LEN; i++) {
    byte b = (i < nlen) ? name[i] : 0;
    EEPROM.update(addr + 1 + MAX_UID_LEN + 1 + i, b);
  }
  writeCount(count + 1);
  return true;
}

// Find index of uid in storage. returns -1 if not found
int findIndex(byte *uid, byte uidLen) {
  byte count = readCount();
  for (int idx = 0; idx < count; idx++) {
    int addr = entryAddress(idx);
    byte storedUidLen = EEPROM.read(addr);
    if (storedUidLen != uidLen) continue;
    bool match = true;
    for (int i = 0; i < storedUidLen; i++) {
      byte b = EEPROM.read(addr + 1 + i);
      if (b != uid[i]) { match = false; break; }
    }
    if (match) return idx;
  }
  return -1;
}

// Remove card. Returns true if removed.
bool removeCard(byte *uid, byte uidLen) {
  int idx = findIndex(uid, uidLen);
  if (idx < 0) return false;
  byte count = readCount();
  // shift subsequent entries left
  for (int i = idx; i < count - 1; i++) {
    int from = entryAddress(i + 1);
    int to = entryAddress(i);
    for (int b = 0; b < ENTRY_SIZE; b++) {
      byte val = EEPROM.read(from + b);
      EEPROM.update(to + b, val);
    }
  }
  // clear last entry
  int lastAddr = entryAddress(count - 1);
  for (int b = 0; b < ENTRY_SIZE; b++) EEPROM.update(lastAddr + b, 0);
  writeCount(count - 1);
  return true;
}

void clearAuthorizedList() {
  byte count = readCount();
  for (int i = 0; i < count; i++) {
    int addr = entryAddress(i);
    for (int b = 0; b < ENTRY_SIZE; b++) EEPROM.update(addr + b, 0);
  }
  writeCount(0);
}

// Read name stored for index
String readNameFromEEPROM(int index) {
  int addr = entryAddress(index);
  byte storedUidLen = EEPROM.read(addr);
  byte nlen = EEPROM.read(addr + 1 + MAX_UID_LEN);
  if (nlen == 0) return String("NoName");
  char buf[MAX_NAME_LEN + 1];
  for (int i = 0; i < nlen; i++) {
    buf[i] = (char)EEPROM.read(addr + 1 + MAX_UID_LEN + 1 + i);
  }
  buf[nlen] = '\0';
  return String(buf);
}

// --------------------- Utility / Printing ---------------------
void printUID(byte *uid, byte len) {
  for (int i = 0; i < len; i++) {
    if (uid[i] < 0x10) Serial.print('0');
    Serial.print(uid[i], HEX);
    if (i < len - 1) Serial.print(':');
  }
  Serial.println();
}

void printAuthorizedList() {
  byte count = readCount();
  Serial.print(F("Authorized count: "));
  Serial.println(count);
  for (int i = 0; i < count; i++) {
    int addr = entryAddress(i);
    byte ul = EEPROM.read(addr);
    byte buf[MAX_UID_LEN];
    for (int j = 0; j < ul; j++) buf[j] = EEPROM.read(addr + 1 + j);
    Serial.print(F(" [")); Serial.print(i); Serial.print(F("] UID="));
    printUID(buf, ul);
    Serial.print(F("     Name: "));
    Serial.println(readNameFromEEPROM(i));
  }
}

// Read a line from Serial (blocking up to timeoutMs). Returns empty string on timeout.
String readLineFromSerial(unsigned long timeoutMs) {
  unsigned long start = millis();
  String s = "";
  while (true) {
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\r') continue; // ignore CR
      if (c == '\n') return s;
      s += c;
      // limit length
      if (s.length() >= MAX_NAME_LEN) {
        // consume rest of line until newline (non-blocking)
        while (Serial.available()) {
          char cc = Serial.read();
          if (cc == '\n') break;
        }
        return s;
      }
    }
    if (timeoutMs > 0 && millis() - start > timeoutMs) return String("");
    delay(10);
  }
}
