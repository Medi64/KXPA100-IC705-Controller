#ifndef KXPA100CONTROLLER_H
#define KXPA100CONTROLLER_H

#include <Arduino.h>

class KXPA100Controller {
public:
  struct BandInfo {
    uint32_t lowerFreq;
    uint32_t upperFreq;
    const char* name;
    const char* bandCmd;
    const char* antennaCmd;
  };

  KXPA100Controller(HardwareSerial& port,
                    int rxPin,
                    int txPin,
                    uint32_t baud,
                    uint16_t delayComm,
                    bool inverted);

  void begin();
  bool checkConnection();
  String getSWR();
  String getPower();
  String getTemperature();
  String getAntenna();
  String getMode();
  String setMode(const char* mode);
  String getVoltage();
  String getFaultCodes();
  int getBand();
  const char* getBandName(int index) const;
  const char* getAntennaCmd(int index) const;
  int getBandIndexByFrequency(uint32_t freq);
  void setBand(int idx);
 
private:
  String txRx(const char* cmd);
  HardwareSerial& _port;
  int _rxPin;
  int _txPin;
  uint32_t _baud;
  uint16_t _delayComm;
  bool _inverted;

  static const char* const _modeCmd[];
  static const char* const _modeStr[];
  static const BandInfo _bandTable[];
  static const size_t _bandCount;
};

#endif // KXPA100CONTROLLER_H