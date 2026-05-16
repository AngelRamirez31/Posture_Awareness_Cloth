#include <Wire.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

// UUID(canales de comuncacion, service identifica el dispositivo,characteristic envia los datos, commando envia la peticion de calibrar)
#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_UUID        "12345678-1234-5678-1234-56789abcdef0"

Preferences preferences;

// variables globales(puntero que referencia al canal de comunicacion, banderas para saber si esta conectado o si se pide calibrar)
BLECharacteristic* pCharacteristic = NULL;
bool deviceConnected = false;
bool peticionCalibrar = false;

// variables que se utilizan para el filtro complementario
float anguloCuello = 0, anguloEspalda = 0;
float offsetCuello = 0, offsetEspalda = 0;
unsigned long tiempoPrevio = 0;
const float alpha = 0.98;

// --- MOTORES --- pines de los motores de vibracion
const int motorCuelloPin = 4; 
const int motorEspaldaPin = 16; 

// tiempos para la vibracion
const int TIEMPO_ENCENDIDO = 300; // vibra por 300 ms 
const int TIEMPO_APAGADO = 800;   // descansa 800 ms
const unsigned long TIEMPO_TOLERANCIA = 10000; // 10000 ms = 10 segundos
const float ANGULO_MAXIMO = 15.0;              // los grados limite antes de considerar mala postura

// variables para controlar los 10 segundos de tolerancia (independientes)
unsigned long tiempoInicioMalaPosturaCuello = 0; 
bool cronometroActivoCuello = false;
bool alertaCuello = false; // Se vuelve true cuando pasan los 10s

unsigned long tiempoInicioMalaPosturaEspalda = 0; 
bool cronometroActivoEspalda = false;
bool alertaEspalda = false; // Se vuelve true cuando pasan los 10s

// variable para el GESTOR CENTRAL de motores (evita que enciendan al mismo tiempo)
unsigned long tiempoUltimoCicloMotor = 0;

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

//esta funcion tiene un filtro complementario
float obtenerAnguloFiltrado(int direccion, float &anguloActual, float dt) {
  Wire.beginTransmission(direccion);
  Wire.write(0x3B); 
  Wire.endTransmission(false);
  Wire.requestFrom(direccion, 14, true);

  if (Wire.available() == 14) {
    int16_t ax = Wire.read()<<8|Wire.read();
    int16_t ay = Wire.read()<<8|Wire.read();
    int16_t az = Wire.read()<<8|Wire.read();
    Wire.read()<<8|Wire.read(); // ignorar temperatura
    int16_t gx = Wire.read()<<8|Wire.read();

    float accAngulo = atan2(az, ay) * 180.0 / PI;
    float gyroRate = gx / 131.0; 
    anguloActual = alpha * (anguloActual + gyroRate * dt) + (1.0 - alpha) * accAngulo;
  }
  
  return anguloActual;
}

void setup() {
  Serial.begin(115200);

  // configurar los pines de los motores
  pinMode(motorCuelloPin, OUTPUT);
  digitalWrite(motorCuelloPin, LOW); 
  pinMode(motorEspaldaPin, OUTPUT);
  digitalWrite(motorEspaldaPin, LOW); 
  
  preferences.begin("postura", false);
  offsetCuello = preferences.getFloat("offCuello", 0.0);
  offsetEspalda = preferences.getFloat("offEspalda", 0.0);

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
  
  // Zumbido inicial secuencial (Turnamos los motores para no forzar el encendido)
  digitalWrite(motorCuelloPin, HIGH);
  delay(200);
  digitalWrite(motorCuelloPin, LOW);
  delay(100);
  digitalWrite(motorEspaldaPin, HIGH);
  delay(200);
  digitalWrite(motorEspaldaPin, LOW);
}

void loop() {
  // calculo del tiempo para la formula
  unsigned long tiempoActual = millis();
  float dt = (tiempoActual - tiempoPrevio) / 1000.0;
  tiempoPrevio = tiempoActual;
  
  if (dt > 0.1) dt = 0.02; 

  float cuello = obtenerAnguloFiltrado(0x68, anguloCuello, dt);
  float espalda = obtenerAnguloFiltrado(0x69, anguloEspalda, dt);

  // calibracion 
  if (peticionCalibrar) {
    offsetCuello = cuello;
    offsetEspalda = espalda;
    preferences.putFloat("offCuello", offsetCuello);
    preferences.putFloat("offEspalda", offsetEspalda);
    peticionCalibrar = false;
  }

  // calculo final
  float cuelloFinal = cuello - offsetCuello;
  float espaldaFinal = espalda - offsetEspalda;

  // evalua cuello (Cronometro de 10 segundos)
  if (abs(cuelloFinal) > ANGULO_MAXIMO) {
    if (!cronometroActivoCuello) {
      tiempoInicioMalaPosturaCuello = millis(); 
      cronometroActivoCuello = true;
    } else if (millis() - tiempoInicioMalaPosturaCuello >= TIEMPO_TOLERANCIA) {
      alertaCuello = true; // Ya pasaron los 10s, necesita vibrar
    }
  } else {
    cronometroActivoCuello = false;    
    alertaCuello = false;         
  }

  // espalda (Cronometro de 10 segundos)
  if (abs(espaldaFinal) > ANGULO_MAXIMO) {
    if (!cronometroActivoEspalda) {
      tiempoInicioMalaPosturaEspalda = millis(); 
      cronometroActivoEspalda = true;
    } else if (millis() - tiempoInicioMalaPosturaEspalda >= TIEMPO_TOLERANCIA) {
      alertaEspalda = true; // Ya pasaron los 10s, necesita vibrar
    }
  } else {
    cronometroActivoEspalda = false;    
    alertaEspalda = false;         
  }

  // gestor motores para que no se solapen
  unsigned long tiempoTranscurridoVibracion = millis() - tiempoUltimoCicloMotor;

  if (alertaCuello && alertaEspalda) {
    // Si ambos estan mal:
    // 0 - 300ms: Vibra Cuello
    // 300 - 600ms: Silencio (pausa)
    // 600 - 900ms: Vibra Espalda
    // 900 - 1200ms: Silencio (pausa y reinicio)
    
    if (tiempoTranscurridoVibracion < TIEMPO_ENCENDIDO) {
      digitalWrite(motorCuelloPin, HIGH);
      digitalWrite(motorEspaldaPin, LOW);
    } 
    else if (tiempoTranscurridoVibracion < TIEMPO_ENCENDIDO + 300) {
      digitalWrite(motorCuelloPin, LOW);
      digitalWrite(motorEspaldaPin, LOW);
    } 
    else if (tiempoTranscurridoVibracion < (TIEMPO_ENCENDIDO * 2) + 300) {
      digitalWrite(motorCuelloPin, LOW);
      digitalWrite(motorEspaldaPin, HIGH);
    } 
    else if (tiempoTranscurridoVibracion < (TIEMPO_ENCENDIDO * 2) + 600) {
      digitalWrite(motorCuelloPin, LOW);
      digitalWrite(motorEspaldaPin, LOW);
    } 
    else {
      tiempoUltimoCicloMotor = millis(); // Termina la rotacion, vuelve a empezar
    }

  } else if (alertaCuello) {
    // Si solo el cuello esta mal:
    if (tiempoTranscurridoVibracion < TIEMPO_ENCENDIDO) {
      digitalWrite(motorCuelloPin, HIGH);
      digitalWrite(motorEspaldaPin, LOW);
    } else if (tiempoTranscurridoVibracion < TIEMPO_ENCENDIDO + TIEMPO_APAGADO) {
      digitalWrite(motorCuelloPin, LOW);
      digitalWrite(motorEspaldaPin, LOW);
    } else {
      tiempoUltimoCicloMotor = millis();
    }

  } else if (alertaEspalda) {
    // Si la espalda esta mal:
    if (tiempoTranscurridoVibracion < TIEMPO_ENCENDIDO) {
      digitalWrite(motorCuelloPin, LOW);
      digitalWrite(motorEspaldaPin, HIGH);
    } else if (tiempoTranscurridoVibracion < TIEMPO_ENCENDIDO + TIEMPO_APAGADO) {
      digitalWrite(motorCuelloPin, LOW);
      digitalWrite(motorEspaldaPin, LOW);
    } else {
      tiempoUltimoCicloMotor = millis();
    }

  } else {
    // no hay corvatura
    digitalWrite(motorCuelloPin, LOW);
    digitalWrite(motorEspaldaPin, LOW);
    tiempoUltimoCicloMotor = millis(); // Mantener el cronometro listo en 0
  }

  //envio de datos al cel
  if (deviceConnected) {
    String msg = "CUELLO: " + String(cuelloFinal, 1) + "°\nESPALDA: " + String(espaldaFinal, 1) + "°";
    Serial.println(msg);
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
  }

  delay(50); 
}
