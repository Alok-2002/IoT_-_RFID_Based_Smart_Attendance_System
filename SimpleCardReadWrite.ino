/* RFID authorize with add/remove via Serial + EEPROM persistence
   - MFRC522 (SS_PIN=10, RST_PIN=9)
   - LED_PIN lights continuously for authorized card
   - BUZZER_PIN produces continuous tone while authorized card present
   - Serial commands (while card is on reader): 'a' add, 'r' remove, 'p' print, 'c' clear
*/

#include <SPI.h>
#include <MFRC522.h>
#include <EEPROM.h>

#define SS_PIN 10
#define RST_PIN 9
MFRC522 mfrc522(SS_PIN, RST_PIN);

// hardware
const int LED_PIN = 8;
const int BUZZER_PIN = 7;

// EEPROM layout
const int EEPROM_ADDR_COUNT = 0;      // 1 byte: number of stored cards
const int EEPROM_ADDR_START = 10;     // start storing entries after some offset
const int MAX_CARDS = 40;             // max entries (ensure EEPROM fits)
const int MAX_UID_LEN = 10;           // support UID up to 10 bytes
const int ENTRY_SIZE = 1 + MAX_UID_LEN; // 1 byte length + uid bytes

// debounce / removal detection
const int REMOVAL_CHECKS = 6;     // number of consecutive "no-card" checks to decide removed
const int REMOVAL_DELAY_MS = 120; // delay between checks

// runtime buffers
byte currentUID[MAX_UID_LEN];
byte currentUIDLen = 0;

void setup() {
  Serial.begin(115200);
  while (!Serial) { }
  SPI.begin();
  mfrc522.PCD_Init();

  pinMode(LED_PIN, OUTPUT);
  pinMode(BUZZER_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  noTone(BUZZER_PIN);

  // initialize count if invalid
  if (!countValid()) writeCount(0);

  Serial.println(F("RFID Authorize System"));
  Serial.println(F("Commands (while card on reader): a=add, r=remove, p=print, c=clear"));
  Serial.println(F("Present card..."));
}

// --------------------- main loop ---------------------
void loop() {
  // check for new card
  if (!mfrc522.PICC_IsNewCardPresent()) {
    // still allow serial command to clear/print even if no card
    if (Serial.available()) handleSerialNoCard();
    return;
  }
  if (!mfrc522.PICC_ReadCardSerial()) return;

  // read uid
  currentUIDLen = mfrc522.uid.size;
  if (currentUIDLen > MAX_UID_LEN) currentUIDLen = MAX_UID_LEN;
  for (byte i = 0; i < currentUIDLen; i++) currentUID[i] = mfrc522.uid.uidByte[i];

  Serial.print(F("Card UID: "));
  printUID(currentUID, currentUIDLen);

  // Check if authorized
  if (isAuthorized(currentUID, currentUIDLen)) {
    Serial.println(F("Access Granted ✔ (AUTHORIZED)"));
    // turn LED and buzzer ON continuously while card present
    digitalWrite(LED_PIN, HIGH);
    tone(BUZZER_PIN, 2000); // continuous tone (no duration)
    // while card remains present allow serial commands (add/remove/print/clear)
    waitWhileCardPresentAllowSerial();
    // once removed, turn off
    digitalWrite(LED_PIN, LOW);
    noTone(BUZZER_PIN);
  } else {
    Serial.println(F("Access Denied ✖ (UNAUTHORIZED)"));
    // do nothing (explicitly requested). still allow user to add/remove while card present
    waitWhileCardPresentAllowSerial();
  }

  // halt PICC and small delay
  mfrc522.PICC_HaltA();
  delay(150);
}

// --------------------- Serial handling without card ---------------------
void handleSerialNoCard() {
  char c = Serial.read();
  if (c == 'p' || c == 'P') {
    printAuthorizedList();
  } else if (c == 'c' || c == 'C') {
    clearAuthorizedList();
    Serial.println(F("Authorized list cleared."));
  } // 'a' or 'r' require a card present
}

// Wait while card is present. Inside this loop allow serial commands:
// 'a' = add current card to list
// 'r' = remove current card from list
// 'p' = print list; 'c' = clear list
void waitWhileCardPresentAllowSerial() {
  int consecutiveNoCard = 0;
  // keep checking: as long as reader detects card we reset consecutiveNoCard
  while (true) {
    // service serial commands if any
    if (Serial.available()) {
      char ch = Serial.read();
      if ((ch == 'a' || ch == 'A')) {
        if (addCard(currentUID, currentUIDLen)) Serial.println(F("Card added to authorized list."));
        else Serial.println(F("Add failed (already present or list full)."));
      } else if ((ch == 'r' || ch == 'R')) {
        if (removeCard(currentUID, currentUIDLen)) Serial.println(F("Card removed from authorized list."));
        else Serial.println(F("Remove failed (not found)."));
      } else if (ch == 'p' || ch == 'P') {
        printAuthorizedList();
      } else if (ch == 'c' || ch == 'C') {
        clearAuthorizedList();
        Serial.println(F("Authorized list cleared."));
      }
    }

    // now check presence
    // PICC_IsNewCardPresent returns true when a new card is presented.
    // To detect removal, attempt a short read; if it fails repeatedly we assume removed.
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      // card present (or newly presented) => reset counter
      consecutiveNoCard = 0;
    } else {
      // possibly still present OR removed; increase counter
      consecutiveNoCard++;
      if (consecutiveNoCard > REMOVAL_CHECKS) break; // consider removed
    }
    delay(REMOVAL_DELAY_MS);
  }
}

// --------------------- EEPROM helpers ---------------------
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

// add card (length + uid). returns true if added, false if duplicate or full
bool addCard(byte *uid, byte len) {
  if (len == 0) return false;
  byte count = readCount();
  if (count >= MAX_CARDS) return false;

  // check duplicate
  if (findIndex(uid, len) >= 0) return false;

  int addr = entryAddress(count);
  EEPROM.update(addr, len); // store length
  for (int i = 0; i < MAX_UID_LEN; i++) {
    byte b = (i < len) ? uid[i] : 0;
    EEPROM.update(addr + 1 + i, b);
  }
  writeCount(count + 1);
  return true;
}

// find index of uid in stored list. returns -1 if not found
int findIndex(byte *uid, byte len) {
  byte count = readCount();
  for (int idx = 0; idx < count; idx++) {
    int addr = entryAddress(idx);
    byte stLen = EEPROM.read(addr);
    if (stLen != len) continue;
    bool match = true;
    for (int i = 0; i < stLen; i++) {
      byte b = EEPROM.read(addr + 1 + i);
      if (b != uid[i]) { match = false; break; }
    }
    if (match) return idx;
  }
  return -1;
}

// remove card by shifting subsequent entries left. returns true if removed.
bool removeCard(byte *uid, byte len) {
  int idx = findIndex(uid, len);
  if (idx < 0) return false;
  byte count = readCount();
  // shift
  for (int i = idx; i < count - 1; i++) {
    int from = entryAddress(i + 1);
    int to = entryAddress(i);
    // copy ENTRY_SIZE bytes
    for (int b = 0; b < ENTRY_SIZE; b++) {
      byte val = EEPROM.read(from + b);
      EEPROM.update(to + b, val);
    }
  }
  // clear last entry (optional)
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

// --------------------- utility / printing ---------------------
bool isAuthorized(byte *uid, byte len) {
  return (findIndex(uid, len) >= 0);
}

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
    byte l = EEPROM.read(addr);
    byte buf[MAX_UID_LEN];
    for (int j = 0; j < l; j++) buf[j] = EEPROM.read(addr + 1 + j);
    Serial.print(F(" [")); Serial.print(i); Serial.print(F("] "));
    printUID(buf, l);
  }
}
