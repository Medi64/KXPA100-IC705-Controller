#pragma once
#include <WiFi.h>

class CatWifiClient {
public:
  CatWifiClient(const char* ssid, const char* password,
                const char* serverIP, uint16_t port, uint16_t timeout)
      : _ssid(ssid), _password(password),
        _serverIP(serverIP), _port(port), _timeout(timeout),
        _socketState(DISCONNECTED), _lastConnectAttempt(0),
        _retryCount(0) {}

  void begin() {
    Serial.println("Starting WiFi...");
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(true);
    delay(100);

    WiFi.begin(_ssid, _password);

    // Event Callback - NON-BLOCKING
    WiFi.onEvent([this](arduino_event_id_t event, arduino_event_info_t info) {
      switch (event) {
        case ARDUINO_EVENT_WIFI_STA_GOT_IP:
          Serial.print("WiFi connected, IP: ");
          Serial.println(WiFi.localIP());
          _socketState = READY_TO_CONNECT;
          break;

        case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
          Serial.println("WiFi disconnected, trying to reconnect...");
          _socketState = DISCONNECTED;
          WiFi.begin(_ssid, _password);
          break;
      }
    });
  }

  // Call this regularly from your loop/task
  void update() {
    unsigned long now = millis();
    
    switch (_socketState) {
      case READY_TO_CONNECT:
        // Attempt connection with backoff
        if (now - _lastConnectAttempt >= _getBackoffDelay()) {
          _attemptSocketConnect();
        }
        break;
        
      case CONNECTING:
        // Check if connection succeeded
        if (_socket.connected()) {
          Serial.println("CAT-Server connected");
          _socketState = CONNECTED;
          _retryCount = 0;
        } else if (now - _lastConnectAttempt >= CONNECT_TIMEOUT_MS) {
          Serial.println("Socket connect timeout");
          _socket.stop();
          _socketState = READY_TO_CONNECT;
          _retryCount++;
          
          if (_retryCount >= MAX_RETRIES) {
            Serial.println("Max retries reached, waiting longer...");
            _retryCount = MAX_RETRIES; // Cap it
          }
        }
        break;
        
      case CONNECTED:
        // Monitor connection health
        if (!_socket.connected()) {
          Serial.println("Socket disconnected");
          _socketState = READY_TO_CONNECT;
          _retryCount = 0;
        }
        break;
        
      case DISCONNECTED:
        // Waiting for WiFi
        break;
    }
  }

  bool isConnected() {
    return WiFi.status() == WL_CONNECTED && _socket.connected();
  }

  String sendCommand(const char* command) {
    if (!isConnected()) {
      return "";
    }

    _socket.print(command);

    unsigned long startTime = millis();
    // Wait for data availability
    while (!_socket.available() && millis() - startTime < _timeout) {
      delay(10);
    }

    if (!_socket.available()) {
      Serial.println("CAT command timeout");
      return "";
    }

    String response;
    response.reserve(64); // Pre-allocate to avoid fragmentation
    while (_socket.available()) {
      response += static_cast<char>(_socket.read());
    }
    return response;
  }

private:
  enum SocketState {
    DISCONNECTED,
    READY_TO_CONNECT,
    CONNECTING,
    CONNECTED
  };

  static const uint16_t CONNECT_TIMEOUT_MS = 2000;
  static const uint16_t INITIAL_BACKOFF_MS = 500;
  static const uint16_t MAX_BACKOFF_MS = 30000;
  static const uint8_t MAX_RETRIES = 10;

  const char* _ssid;
  const char* _password;
  const char* _serverIP;
  uint16_t _port;
  uint16_t _timeout;
  WiFiClient _socket;
  
  SocketState _socketState;
  unsigned long _lastConnectAttempt;
  uint8_t _retryCount;

  void _attemptSocketConnect() {
    Serial.println("Attempting CAT-Server connection...");
    _socket.connect(_serverIP, _port);
    _socketState = CONNECTING;
    _lastConnectAttempt = millis();
  }

  unsigned long _getBackoffDelay() {
    // Exponential backoff: 500ms, 1s, 2s, 4s, 8s, ... max 30s
    unsigned long backoff = INITIAL_BACKOFF_MS * (1 << min(_retryCount, (uint8_t)6));
    return min(backoff, (unsigned long)MAX_BACKOFF_MS);
  }
};
