#include "KXPA100Controller.h"

//-----------------------------------------------------------------------------
// Constants
//-----------------------------------------------------------------------------
static const uint16_t SERIAL_TIMEOUT_MS = 100;
static const uint8_t MAX_RETRIES = 3;

//-----------------------------------------------------------------------------
// Initialize Static Members
//-----------------------------------------------------------------------------

const char* const KXPA100Controller::_modeCmd[] = { "^MDB", "^MDM", "^MDA" };
const char* const KXPA100Controller::_modeStr[] = { "Bypass", "Manual", "Automatic" };

const KXPA100Controller::BandInfo KXPA100Controller::_bandTable[] = {
  {1800000L,   2000000L,   "160m", "^BN00;", "^AN1;"},
  {3500000L,   3800000L,   "80m",  "^BN01;", "^AN1;"},
  {5351500L,   5366500L,   "60m",  "^BN02;", "^AN1;"},
  {7000000L,   7200000L,   "40m",  "^BN03;", "^AN1;"},
  {10100000L,  10150000L,  "30m",  "^BN04;", "^AN1;"},
  {14000000L,  14350000L,  "20m",  "^BN05;", "^AN1;"},
  {18068000L,  18168000L,  "17m",  "^BN06;", "^AN1;"},
  {21000000L,  21450000L,  "15m",  "^BN07;", "^AN1;"},
  {24890000L,  24990000L,  "12m",  "^BN08;", "^AN1;"},
  {28000000L,  29700000L,  "10m",  "^BN09;", "^AN1;"},
  {50000000L,  52000000L,  "6m",   "^BN10;", "^AN2;"}
};

const size_t KXPA100Controller::_bandCount =
  sizeof(KXPA100Controller::_bandTable) /
  sizeof(KXPA100Controller::_bandTable[0]);

//-----------------------------------------------------------------------------
// Constructor
//-----------------------------------------------------------------------------

KXPA100Controller::KXPA100Controller(HardwareSerial& port,
                                     int rxPin,
                                     int txPin,
                                     uint32_t baud,
                                     uint16_t delayComm,
                                     bool inverted)
  : _port(port)
  , _rxPin(rxPin)
  , _txPin(txPin)
  , _baud(baud)
  , _delayComm(delayComm)
  , _inverted(inverted)
{ }

//-----------------------------------------------------------------------------
// Public Methods
//-----------------------------------------------------------------------------

void KXPA100Controller::begin() {
  // Initialize Serial2 with total inverted RX/TX if required
  _port.begin(_baud, SERIAL_8N1, _rxPin, _txPin, _inverted);
  _port.setTimeout(SERIAL_TIMEOUT_MS);
  
  // Flush any garbage from buffer
  while (_port.available()) {
    _port.read();
  }
}

bool KXPA100Controller::checkConnection() {
  String v = txRx("^I;");
  bool ok = v == ("^IKXPA100");
  
  if (!ok) {
    Serial.println("KXPA100 connection check failed");
  }
  
  return ok;
}

String KXPA100Controller::getSWR() {
  String s = txRx("^SW;");
  if (s.length() == 0) return "0.0";
  
  s.replace("^SW", "");
  float swr = s.toFloat() / 10.0f;
  
  // Sanity check
  if (swr < 1.0 || swr > 99.9) {
    Serial.println("Invalid SWR value received");
    return "ERR";
  }
  
  return String(swr, 1);
}

String KXPA100Controller::getPower() {
  String p = txRx("^PF;");
  if (p.length() == 0) return "0";
  
  p.replace("^PF", "");
  float pp = p.toFloat() / 10.0f;
  
  // Sanity check (KXPA100 max ~100W)
  if (pp < 0 || pp > 150) {
    Serial.println("Invalid power value received");
    return "ERR";
  }
  
  return String(pp, 0);
}

String KXPA100Controller::getTemperature() {
  String t = txRx("^TM;");
  if (t.length() == 0) return "0";
  
  t.replace("^TM", "");
  t.replace(";", "");
  float tt = t.toFloat() / 10.0f;
  
  // Sanity check (-40°C to +100°C)
  if (tt < -40 || tt > 100) {
    Serial.println("Invalid temperature value received");
    return "ERR";
  }
  
  return String(tt, 0);
}

String KXPA100Controller::getAntenna() {
  String a = txRx("^AN;");
  if (a.length() == 0) return "?";
  return a;
}

String KXPA100Controller::getMode() {
  String m = txRx("^MD;");
  if (m.length() == 0) return "Unknown";
  
  for (int i = 0; i < 3; ++i) {
    if (m == _modeCmd[i]) return _modeStr[i];
  }
  
  Serial.print("Unknown mode received: ");
  Serial.println(m);
  return m;
}

String KXPA100Controller::setMode(const char* mode) {
  String c = txRx(mode);
  return c;
}

String KXPA100Controller::getVoltage() {
  String v = txRx("^SV;");
  if (v.length() == 0) return "0.0";
  
  v.replace("^SV","");
  float vv = v.toFloat() / 1000.0f;
  
  // Sanity check (typical 12-15V)
  if (vv < 0 || vv > 20) {
    Serial.println("Invalid voltage value received");
    return "ERR";
  }
  
  return String(vv, 1);
}

String KXPA100Controller::getFaultCodes() {
  String f = txRx("^FL;");
  if (f.length() == 0) return "?";
  
  f.replace("^FL", "");
  return f;
}

int KXPA100Controller::getBand() {
  String b = txRx("^BN;");
  if (b.length() == 0) return -1;
  
  b.replace("^BN", "");
  int band = b.toInt();
  
  // Validate band index
  if (band < 0 || band >= (int)_bandCount) {
    Serial.print("Invalid band index received: ");
    Serial.println(band);
    return -1;
  }
  
  return band;
}

const char* KXPA100Controller::getBandName(int index) const {
  if (index < 0 || index >= (int)_bandCount) return "Invalid";
  return _bandTable[index].name;
}

const char* KXPA100Controller::getAntennaCmd(int index) const {
  if (index < 0 || index >= (int)_bandCount) return "";
  return _bandTable[index].antennaCmd;
}

int KXPA100Controller::getBandIndexByFrequency(uint32_t freq) {
  for (size_t i = 0; i < _bandCount; ++i) {
    if (freq >= _bandTable[i].lowerFreq && freq <= _bandTable[i].upperFreq) {
      return i;
    }
  }
  // Frequency not found
  return -1;
}

void KXPA100Controller::setBand(int idx) {
  if (idx < 0 || idx >= (int)_bandCount) {
    Serial.print("setBand: Invalid index ");
    Serial.println(idx);
    return;
  }
  
  // Retry logic for critical commands
  for (uint8_t attempt = 0; attempt < MAX_RETRIES; attempt++) {
    _port.write(_bandTable[idx].bandCmd);           
    delay(_delayComm); 
    _port.write(_bandTable[idx].antennaCmd);
    delay(_delayComm);
    
    // Verify the band was set (optional but recommended)
    int actualBand = getBand();
    if (actualBand == idx) {
      return; // Success
    }
    
    Serial.print("setBand retry ");
    Serial.print(attempt + 1);
    Serial.print("/");
    Serial.println(MAX_RETRIES);
    delay(50); // Small delay before retry
  }
  
  Serial.println("setBand: Failed after retries");
}

//-----------------------------------------------------------------------------
// Private TX/RX with Timeout Handling
//-----------------------------------------------------------------------------

String KXPA100Controller::txRx(const char* cmd) {
  // Check if port is available
  if (!_port) {
    Serial.println("Serial port not available");
    return "";
  }
  
  // Clear any stale data in RX buffer
  while (_port.available()) {
    _port.read();
  }
  
  // Send command
  size_t written = _port.write(cmd);
  if (written != strlen(cmd)) {
    Serial.println("Incomplete command write");
    return "";
  }
  
  delay(_delayComm);
  
  // Read response with timeout (handled by setTimeout)
  String resp = _port.readStringUntil(';');
  
  if (resp.length() == 0) {
    Serial.print("No response for command: ");
    Serial.println(cmd);
    return "";
  }
  
  return resp;
}
