#include <Wire.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>


// UUID(canales de comuncacion, service identifica el dispositivo,characteristic envia los datos, commando envia la peticion de calibrar)
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_UUID        "12345678-1234-5678-1234-56789abcdef0"


// variables globales(puntero que referencia al canal de comunicacion, banderas para saber si esta conectado o si se pide calibrar)
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool peticionCalibrar = false;

// variables que se utilizan para el filtro complementario
float anguloCuello = 0, anguloEspalda = 0;
float offsetCuello = 0, offsetEspalda = 0;
unsigned long tiempoPrevio = 0;
const float alpha = 0.98;

//clase para reconocer si la chamarra esta conectada 
class MyServerCallbacks: public BLEServerCallbacks {
    void onConnect(BLEServer* pServer) { deviceConnected = true; }
    void onDisconnect(BLEServer* pServer) { deviceConnected = false; pServer->startAdvertising(); }
};

//clase para cuando se manda la peticion de calibrar desde el cel
class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
    void onWrite(BLECharacteristic *pCharacteristic) {
      String comando = pCharacteristic->getValue();
      if (comando == "CALIBRAR") peticionCalibrar = true;
    }
};

//funcion que despierta ambos sensores
void despertarMPU(int direccion) {
  Wire.beginTransmission(direccion);
  Wire.write(0x6B); Wire.write(0);
  Wire.endTransmission(true);
}

//esta funcion tiene un filtro complementario, primero se piden los 14 bytes del giroscopio y el acelerometro, despues los datos se dividen en 2partes(8 bits por parte)
// asi el sensor reconoce valores entre -32000 y 32000, despues apllica una formula para obtener los grados por segundo del gyro y despues se usa atan2 para los datos 
//del accele y se convierte de radianes a grados, al final se apica el filtro para obtener el angulo actual
float obtenerAnguloFiltrado(int direccion, float &anguloActual, float dt) {
  Wire.beginTransmission(direccion);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom(direccion, 14, true);

  int16_t ax = Wire.read()<<8|Wire.read();
  int16_t ay = Wire.read()<<8|Wire.read();
  int16_t az = Wire.read()<<8|Wire.read();
  Wire.read()<<8|Wire.read(); // Ignorar temperatura
  int16_t gx = Wire.read()<<8|Wire.read();

  float accAngulo = atan2(ay, az) * 180.0 / PI;
  float gyroRate = gx / 131.0; 
  
  //gyroscopio(drift) y accel(rudio), por eso 98 gyroscopio,2 accel
  anguloActual = alpha * (anguloActual + gyroRate * dt) + (1.0 - alpha) * accAngulo;
  
  return anguloActual;
}

//funcion para iniciar el protocolo I2C, los protocolos ble y el advertising para que sea visible en el cel
void setup() {
  Serial.begin(115200);
  Wire.begin();
  despertarMPU(0x68); despertarMPU(0x69);

  BLEDevice::init("SudaderaBLE");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());
  BLEService *pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(CHARACTERISTIC_UUID, BLECharacteristic::PROPERTY_NOTIFY);
  pCharacteristic->addDescriptor(new BLE2902());
  BLECharacteristic* pCmd = pService->createCharacteristic(COMMAND_UUID, BLECharacteristic::PROPERTY_WRITE);
  pCmd->setCallbacks(new MyCharacteristicCallbacks());
  pService->start();
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  BLEDevice::getAdvertising()->start();
  
  tiempoPrevio = millis();
}

void loop() {
  // calculo del tiempo para la formula
  unsigned long tiempoActual = millis();
  float dt = (tiempoActual - tiempoPrevio) / 1000.0;
  tiempoPrevio = tiempoActual;
  
  // evita saltos de tiempo locos al iniciar
  if (dt > 0.1) dt = 0.02; 

  // dt ya calculado a ambos sensores
  float cuello = obtenerAnguloFiltrado(0x68, anguloCuello, dt);
  float espalda = obtenerAnguloFiltrado(0x69, anguloEspalda, dt);

  // calibracion simple y eficiente (
  if (peticionCalibrar) {
    offsetCuello = cuello;
    offsetEspalda = espalda;
    peticionCalibrar = false;
  }

// calculo final
  float cuelloFinal = cuello - offsetCuello;
  float espaldaFinal = espalda - offsetEspalda;

//envio de datos al cel
  if (deviceConnected) {
    String msg = "CUELLO: " + String(cuelloFinal, 1) + "°\nESPALDA: " + String(espaldaFinal, 1) + "°";
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
  }

  delay(50); 
}