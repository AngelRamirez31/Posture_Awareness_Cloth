#include <Wire.h>
#include <math.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>
#include <Preferences.h>

#define SERVICE_UUID        "4fafc201-1fb5-459e-8fcc-c5c9c331914b"
#define CHARACTERISTIC_UUID "beb5483e-36e1-4688-b7f5-ea07361b26a8"
#define COMMAND_UUID        "12345678-1234-5678-1234-56789abcdef0"

BLECharacteristic* pCharacteristic = NULL;
Preferences preferences;

bool deviceConnected = false;
bool peticionCalibrar = false;
bool peticionMotorTest = false;
bool peticionBorrarCal = false;

const uint8_t MPU_CUELLO  = 0x69;
const uint8_t MPU_ESPALDA = 0x68;

const int motorCuelloPin = 4;
const int motorEspaldaPin = 16;

const bool MOTORES_ACTIVOS_EN_HIGH = true;

const bool PROBAR_MOTORES_AL_INICIO = false;

const unsigned long TIEMPO_ENCENDIDO = 110;
const unsigned long TIEMPO_APAGADO   = 950;
const int MAX_PULSOS_POR_ALERTA = 3;
const unsigned long COOLDOWN_MOTORES_MS = 6000;

const uint8_t INTENSIDAD_MOTOR_CUELLO = 95;
const uint8_t INTENSIDAD_MOTOR_ESPALDA = 95;
const int PWM_FREQ_MOTOR = 180;
const int PWM_RESOLUCION_MOTOR = 8;

const unsigned long TIEMPO_TOLERANCIA = 3000;

const bool VIBRAR_AL_CALIBRAR = false;

const int SIGNO_CUELLO  = 1;
const int SIGNO_ESPALDA = 1;

const bool USAR_ABS_CURVA = true;
const bool USAR_ABS_ESPALDA = true;

const float ZONA_MUERTA_GRADOS = 3.0;

const float UMBRAL_CUELLO_MALO  = 17.0;
const float UMBRAL_CUELLO_BUENO = 9.0;

const float UMBRAL_ESPALDA_MALO  = 20.0;
const float UMBRAL_ESPALDA_BUENO = 11.0;

const unsigned long INTERVALO_LECTURA_MS = 50;
const float TAU_FILTRO = 0.70;
const float BETA_SUAVIZADO = 0.88;

const int MUESTRAS_CALIBRACION = 150;
const int DELAY_CALIBRACION_MS = 20;

const bool PAUSAR_ALERTAS_EN_MOVIMIENTO = true;

const float UMBRAL_ACC_MOVIMIENTO = 0.28;
const float UMBRAL_GYRO_MOVIMIENTO = 90.0;
const unsigned long TIEMPO_REPOSO_TRAS_MOVIMIENTO = 900;

struct DatosMPU {
  int16_t ax, ay, az;
  int16_t gx, gy, gz;
  bool ok;
};

struct SensorPostura {
  uint8_t direccion;
  float anguloFiltrado;
  float offset;
  float gyroBias;
  float anguloFinal;
  float anguloSuavizado;
  bool ok;
  bool movimiento;
};

struct EstadoZona {
  bool mala;
  bool alerta;
  unsigned long tiempoInicio;
};

SensorPostura sensorCuello  = {MPU_CUELLO, 0, 0, 0, 0, 0, false, false};
SensorPostura sensorEspalda = {MPU_ESPALDA, 0, 0, 0, 0, 0, false, false};

EstadoZona estadoCuello  = {false, false, 0};
EstadoZona estadoEspalda = {false, false, 0};

unsigned long tiempoPrevio = 0;
unsigned long tiempoUltimaLectura = 0;
unsigned long tiempoUltimoCicloMotor = 0;
unsigned long tiempoUltimoMovimiento = 0;

float curvaCuello = 0;
float valorDeteccionCuello = 0;
float valorDeteccionEspalda = 0;
bool movimientoFuerte = false;

void configurarPWMmotores() {

  ledcAttach(motorCuelloPin, PWM_FREQ_MOTOR, PWM_RESOLUCION_MOTOR);
  ledcAttach(motorEspaldaPin, PWM_FREQ_MOTOR, PWM_RESOLUCION_MOTOR);
}

void escribirMotorPWM(int pin, bool encendido, uint8_t intensidad) {
  uint8_t duty;

  if (MOTORES_ACTIVOS_EN_HIGH) {
    duty = encendido ? intensidad : 0;
  } else {

    duty = encendido ? (255 - intensidad) : 255;
  }

  ledcWrite(pin, duty);
}

void escribirMotor(int pin, bool encendido) {
  if (pin == motorCuelloPin) {
    escribirMotorPWM(pin, encendido, INTENSIDAD_MOTOR_CUELLO);
  } else if (pin == motorEspaldaPin) {
    escribirMotorPWM(pin, encendido, INTENSIDAD_MOTOR_ESPALDA);
  }
}

void apagarMotores() {
  escribirMotor(motorCuelloPin, false);
  escribirMotor(motorEspaldaPin, false);
}

void vibrarMotor(int pin, int tiempoMs) {

  escribirMotor(pin, true);
  delay(tiempoMs);
  escribirMotor(pin, false);
}

void pruebaMotores() {
  apagarMotores();
  delay(200);

  Serial.println("PRUEBA MOTOR CUELLO - pulso corto");
  vibrarMotor(motorCuelloPin, 180);
  delay(700);

  Serial.println("PRUEBA MOTOR ESPALDA - pulso corto");
  vibrarMotor(motorEspaldaPin, 180);
  delay(700);

  Serial.println("PRUEBA AMBOS MOTORES - pulso corto");
  escribirMotor(motorCuelloPin, true);
  escribirMotor(motorEspaldaPin, true);
  delay(180);
  apagarMotores();
}

void reiniciarControlMotores() {
  apagarMotores();
  tiempoUltimoCicloMotor = millis();
}

class MyServerCallbacks: public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    deviceConnected = true;
  }

  void onDisconnect(BLEServer* pServer) {
    deviceConnected = false;
    pServer->startAdvertising();
  }
};

class MyCharacteristicCallbacks: public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pCharacteristic) {
    String comando = pCharacteristic->getValue();
    comando.trim();
    comando.toUpperCase();

    if (comando == "CALIBRAR") {
      peticionCalibrar = true;
    } else if (comando == "TEST" || comando == "MOTORTEST" || comando == "MOTORES") {
      peticionMotorTest = true;
    } else if (comando == "BORRAR_CAL") {
      peticionBorrarCal = true;
    }
  }
};

void notificarBLE(const String &msg) {
  Serial.println(msg);

  if (deviceConnected && pCharacteristic != NULL) {
    pCharacteristic->setValue(msg.c_str());
    pCharacteristic->notify();
  }
}

float normalizar180(float angulo) {
  while (angulo > 180.0) angulo -= 360.0;
  while (angulo < -180.0) angulo += 360.0;
  return angulo;
}

float aplicarZonaMuerta(float valor) {
  if (fabs(valor) < ZONA_MUERTA_GRADOS) return 0.0;
  return valor;
}

void escribirRegistroMPU(uint8_t direccion, uint8_t registro, uint8_t valor) {
  Wire.beginTransmission(direccion);
  Wire.write(registro);
  Wire.write(valor);
  Wire.endTransmission(true);
}

bool configurarMPU(uint8_t direccion) {
  escribirRegistroMPU(direccion, 0x6B, 0x00);
  delay(50);

  escribirRegistroMPU(direccion, 0x1A, 0x04);
  escribirRegistroMPU(direccion, 0x1B, 0x00);
  escribirRegistroMPU(direccion, 0x1C, 0x00);

  Wire.beginTransmission(direccion);
  Wire.write(0x75);
  if (Wire.endTransmission(false) != 0) return false;
  Wire.requestFrom(direccion, (uint8_t)1, (uint8_t)true);

  if (Wire.available()) {
    uint8_t whoami = Wire.read();
    return (whoami == 0x68 || whoami == 0x69 || whoami == 0x70);
  }
  return false;
}

DatosMPU leerMPU(uint8_t direccion) {
  DatosMPU d;
  d.ok = false;

  Wire.beginTransmission(direccion);
  Wire.write(0x3B);

  if (Wire.endTransmission(false) != 0) return d;

  uint8_t leidos = Wire.requestFrom(direccion, (uint8_t)14, (uint8_t)true);
  if (leidos != 14 || Wire.available() < 14) return d;

  d.ax = (Wire.read() << 8) | Wire.read();
  d.ay = (Wire.read() << 8) | Wire.read();
  d.az = (Wire.read() << 8) | Wire.read();

  Wire.read(); Wire.read();

  d.gx = (Wire.read() << 8) | Wire.read();
  d.gy = (Wire.read() << 8) | Wire.read();
  d.gz = (Wire.read() << 8) | Wire.read();

  d.ok = true;
  return d;
}

float calcularAnguloAcelerometro(const DatosMPU &d) {
  return atan2((float)d.az, (float)d.ay) * 180.0 / PI;
}

float calcularVelocidadGiro(const DatosMPU &d) {
  return ((float)d.gx) / 131.0;
}

bool detectarMovimiento(const DatosMPU &d) {
  if (!d.ok) return false;

  float axg = ((float)d.ax) / 16384.0;
  float ayg = ((float)d.ay) / 16384.0;
  float azg = ((float)d.az) / 16384.0;
  float accTotal = sqrt(axg * axg + ayg * ayg + azg * azg);
  float accExtra = fabs(accTotal - 1.0);

  float gx = fabs(((float)d.gx) / 131.0);
  float gy = fabs(((float)d.gy) / 131.0);
  float gz = fabs(((float)d.gz) / 131.0);
  float gyroTotal = gx + gy + gz;

  return (accExtra > UMBRAL_ACC_MOVIMIENTO || gyroTotal > UMBRAL_GYRO_MOVIMIENTO);
}

bool actualizarFiltro(SensorPostura &sensor, float dt) {
  DatosMPU d = leerMPU(sensor.direccion);
  sensor.ok = d.ok;

  if (!d.ok) return false;

  sensor.movimiento = detectarMovimiento(d);

  float accAngulo = calcularAnguloAcelerometro(d);
  float gyroRate = calcularVelocidadGiro(d) - sensor.gyroBias;

  if (dt <= 0.0 || dt > 0.2) dt = 0.02;

  float tau = sensor.movimiento ? 1.20 : TAU_FILTRO;
  float alpha = tau / (tau + dt);

  float prediccion = normalizar180(sensor.anguloFiltrado + gyroRate * dt);
  float errorAcc = normalizar180(accAngulo - prediccion);

  sensor.anguloFiltrado = normalizar180(prediccion + (1.0 - alpha) * errorAcc);
  return true;
}

float promedioAngulos(float sumaSin, float sumaCos) {
  return atan2(sumaSin, sumaCos) * 180.0 / PI;
}

void calibrarPostura() {
  apagarMotores();
  notificarBLE("CALIBRANDO\nMantente recto y quieto 3s");

  float sumaSinCuello = 0, sumaCosCuello = 0;
  float sumaSinEspalda = 0, sumaCosEspalda = 0;
  float sumaGyroCuello = 0, sumaGyroEspalda = 0;
  int muestrasValidasCuello = 0, muestrasValidasEspalda = 0;

  for (int i = 0; i < MUESTRAS_CALIBRACION; i++) {
    DatosMPU dCuello = leerMPU(MPU_CUELLO);
    DatosMPU dEspalda = leerMPU(MPU_ESPALDA);

    if (dCuello.ok) {
      float ang = calcularAnguloAcelerometro(dCuello) * PI / 180.0;
      sumaSinCuello += sin(ang);
      sumaCosCuello += cos(ang);
      sumaGyroCuello += calcularVelocidadGiro(dCuello);
      muestrasValidasCuello++;
    }

    if (dEspalda.ok) {
      float ang = calcularAnguloAcelerometro(dEspalda) * PI / 180.0;
      sumaSinEspalda += sin(ang);
      sumaCosEspalda += cos(ang);
      sumaGyroEspalda += calcularVelocidadGiro(dEspalda);
      muestrasValidasEspalda++;
    }

    delay(DELAY_CALIBRACION_MS);
  }

  if (muestrasValidasCuello > 20) {
    sensorCuello.offset = promedioAngulos(sumaSinCuello, sumaCosCuello);
    sensorCuello.gyroBias = sumaGyroCuello / muestrasValidasCuello;
    sensorCuello.anguloFiltrado = sensorCuello.offset;
    sensorCuello.anguloFinal = 0;
    sensorCuello.anguloSuavizado = 0;
    preferences.putFloat("offCuello", sensorCuello.offset);
    preferences.putFloat("biasCuello", sensorCuello.gyroBias);
  }

  if (muestrasValidasEspalda > 20) {
    sensorEspalda.offset = promedioAngulos(sumaSinEspalda, sumaCosEspalda);
    sensorEspalda.gyroBias = sumaGyroEspalda / muestrasValidasEspalda;
    sensorEspalda.anguloFiltrado = sensorEspalda.offset;
    sensorEspalda.anguloFinal = 0;
    sensorEspalda.anguloSuavizado = 0;
    preferences.putFloat("offEspalda", sensorEspalda.offset);
    preferences.putFloat("biasEspalda", sensorEspalda.gyroBias);
  }

  estadoCuello = {false, false, 0};
  estadoEspalda = {false, false, 0};
  curvaCuello = 0;
  valorDeteccionCuello = 0;
  valorDeteccionEspalda = 0;
  movimientoFuerte = false;
  tiempoUltimoCicloMotor = millis();

  notificarBLE("CALIBRACION LISTA");

  if (VIBRAR_AL_CALIBRAR) {
    vibrarMotor(motorCuelloPin, 100);
    delay(150);
    vibrarMotor(motorEspaldaPin, 100);
  }
}

void borrarCalibracion() {
  preferences.remove("offCuello");
  preferences.remove("offEspalda");
  preferences.remove("biasCuello");
  preferences.remove("biasEspalda");

  sensorCuello.offset = 0.0;
  sensorEspalda.offset = 0.0;
  sensorCuello.gyroBias = 0.0;
  sensorEspalda.gyroBias = 0.0;

  estadoCuello = {false, false, 0};
  estadoEspalda = {false, false, 0};
  apagarMotores();
  notificarBLE("CALIBRACION BORRADA\nUsa CALIBRAR estando recto.");
}

void actualizarAngulosFinales() {
  float cuelloCrudo = normalizar180(sensorCuello.anguloFiltrado - sensorCuello.offset) * SIGNO_CUELLO;
  float espaldaCruda = normalizar180(sensorEspalda.anguloFiltrado - sensorEspalda.offset) * SIGNO_ESPALDA;

  cuelloCrudo = aplicarZonaMuerta(cuelloCrudo);
  espaldaCruda = aplicarZonaMuerta(espaldaCruda);

  sensorCuello.anguloSuavizado = BETA_SUAVIZADO * sensorCuello.anguloSuavizado + (1.0 - BETA_SUAVIZADO) * cuelloCrudo;
  sensorEspalda.anguloSuavizado = BETA_SUAVIZADO * sensorEspalda.anguloSuavizado + (1.0 - BETA_SUAVIZADO) * espaldaCruda;

  sensorCuello.anguloFinal = aplicarZonaMuerta(sensorCuello.anguloSuavizado);
  sensorEspalda.anguloFinal = aplicarZonaMuerta(sensorEspalda.anguloSuavizado);

  curvaCuello = aplicarZonaMuerta(sensorCuello.anguloFinal - sensorEspalda.anguloFinal);

  valorDeteccionCuello = USAR_ABS_CURVA ? fabs(curvaCuello) : curvaCuello;
  valorDeteccionEspalda = USAR_ABS_ESPALDA ? fabs(sensorEspalda.anguloFinal) : sensorEspalda.anguloFinal;
}

void actualizarZona(EstadoZona &zona, float valor, float umbralMalo, float umbralBueno, unsigned long ahora) {
  if (!zona.mala) {
    if (valor > umbralMalo) {
      zona.mala = true;
      zona.alerta = false;
      zona.tiempoInicio = ahora;
    }
  } else {
    if (valor < umbralBueno) {
      zona.mala = false;
      zona.alerta = false;
      zona.tiempoInicio = 0;
    } else if (ahora - zona.tiempoInicio >= TIEMPO_TOLERANCIA) {
      zona.alerta = true;
    }
  }
}

void pausarAlertasPorMovimiento(unsigned long ahora) {
  estadoCuello = {false, false, 0};
  estadoEspalda = {false, false, 0};
  apagarMotores();
  tiempoUltimoCicloMotor = ahora;
}

void controlarMotores() {
  static bool pulsoEncendido = false;
  static int pulsosHechos = 0;
  static unsigned long marcaMotor = 0;
  static unsigned long cooldownHasta = 0;
  static bool alertaPreviaCuello = false;
  static bool alertaPreviaEspalda = false;

  bool alertaCuello = estadoCuello.alerta;
  bool alertaEspalda = estadoEspalda.alerta;
  bool hayAlerta = alertaCuello || alertaEspalda;
  unsigned long ahora = millis();

  if (!hayAlerta) {
    apagarMotores();
    pulsoEncendido = false;
    pulsosHechos = 0;
    cooldownHasta = 0;
    marcaMotor = ahora;
    alertaPreviaCuello = false;
    alertaPreviaEspalda = false;
    tiempoUltimoCicloMotor = ahora;
    return;
  }

  if (alertaCuello != alertaPreviaCuello || alertaEspalda != alertaPreviaEspalda) {
    apagarMotores();
    pulsoEncendido = false;
    pulsosHechos = 0;
    cooldownHasta = 0;
    marcaMotor = ahora;
    alertaPreviaCuello = alertaCuello;
    alertaPreviaEspalda = alertaEspalda;
  }

  if (ahora < cooldownHasta) {
    apagarMotores();
    return;
  }

  if (pulsosHechos >= MAX_PULSOS_POR_ALERTA) {
    apagarMotores();
    pulsoEncendido = false;
    cooldownHasta = ahora + COOLDOWN_MOTORES_MS;
    pulsosHechos = 0;
    marcaMotor = ahora;
    return;
  }

  if (pulsoEncendido) {
    if (ahora - marcaMotor >= TIEMPO_ENCENDIDO) {
      apagarMotores();
      pulsoEncendido = false;
      pulsosHechos++;
      marcaMotor = ahora;
    }
    return;
  }

  if (ahora - marcaMotor < TIEMPO_APAGADO) {
    apagarMotores();
    return;
  }

  apagarMotores();

  if (alertaCuello && alertaEspalda) {

    if (pulsosHechos % 2 == 0) {
      escribirMotor(motorCuelloPin, true);
    } else {
      escribirMotor(motorEspaldaPin, true);
    }
  } else if (alertaCuello) {
    escribirMotor(motorCuelloPin, true);
  } else if (alertaEspalda) {
    escribirMotor(motorEspaldaPin, true);
  }

  pulsoEncendido = true;
  marcaMotor = ahora;
  tiempoUltimoCicloMotor = ahora;
}

String construirMensajeBLE() {
  String estado;
  if (movimientoFuerte && PAUSAR_ALERTAS_EN_MOVIMIENTO) {
    estado = "MOVING";
  } else if (estadoCuello.mala || estadoEspalda.mala) {
    estado = "SLOUCHING";
  } else {
    estado = "GOOD";
  }

  String msg = "CUELLO: " + String(sensorCuello.anguloFinal, 1) + "°";
  msg += "\nESPALDA: " + String(sensorEspalda.anguloFinal, 1) + "°";
  msg += "\nCURVA: " + String(curvaCuello, 1) + "°";
  msg += "\nDET_CUELLO: " + String(valorDeteccionCuello, 1) + "°";
  msg += "\nDET_ESPALDA: " + String(valorDeteccionEspalda, 1) + "°";
  msg += "\nMOV: " + String(movimientoFuerte ? "SI" : "NO");
  msg += "\nALERTA_CUELLO: " + String(estadoCuello.alerta ? "SI" : "NO");
  msg += "\nALERTA_ESPALDA: " + String(estadoEspalda.alerta ? "SI" : "NO");
  msg += "\nMOTOR_PWM: " + String(INTENSIDAD_MOTOR_CUELLO);
  msg += "\nESTADO: " + estado;

  return msg;
}

void revisarComandosSerial() {
  if (!Serial.available()) return;

  String comando = Serial.readStringUntil('\n');
  comando.trim();
  comando.toUpperCase();

  if (comando == "CALIBRAR") {
    peticionCalibrar = true;
  } else if (comando == "TEST" || comando == "MOTORTEST" || comando == "MOTORES") {
    peticionMotorTest = true;
  } else if (comando == "BORRAR_CAL") {
    peticionBorrarCal = true;
  }
}

void setup() {
  Serial.begin(115200);

  pinMode(motorCuelloPin, OUTPUT);
  pinMode(motorEspaldaPin, OUTPUT);
  configurarPWMmotores();
  apagarMotores();

  if (PROBAR_MOTORES_AL_INICIO) {
    delay(800);
    pruebaMotores();
  }

  Wire.begin();
  Wire.setClock(400000);

  bool okCuello = configurarMPU(MPU_CUELLO);
  bool okEspalda = configurarMPU(MPU_ESPALDA);

  preferences.begin("postura", false);
  sensorCuello.offset = preferences.getFloat("offCuello", 0.0);
  sensorEspalda.offset = preferences.getFloat("offEspalda", 0.0);
  sensorCuello.gyroBias = preferences.getFloat("biasCuello", 0.0);
  sensorEspalda.gyroBias = preferences.getFloat("biasEspalda", 0.0);

  DatosMPU dCuello = leerMPU(MPU_CUELLO);
  DatosMPU dEspalda = leerMPU(MPU_ESPALDA);

  if (dCuello.ok) sensorCuello.anguloFiltrado = calcularAnguloAcelerometro(dCuello);
  if (dEspalda.ok) sensorEspalda.anguloFiltrado = calcularAnguloAcelerometro(dEspalda);

  BLEDevice::init("SudaderaBLE");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *pService = pServer->createService(SERVICE_UUID);

  pCharacteristic = pService->createCharacteristic(
    CHARACTERISTIC_UUID,
    BLECharacteristic::PROPERTY_NOTIFY
  );
  pCharacteristic->addDescriptor(new BLE2902());

  BLECharacteristic* pCmd = pService->createCharacteristic(
    COMMAND_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pCmd->setCallbacks(new MyCharacteristicCallbacks());

  pService->start();
  BLEDevice::getAdvertising()->addServiceUUID(SERVICE_UUID);
  BLEDevice::getAdvertising()->start();

  tiempoPrevio = millis();
  tiempoUltimaLectura = millis();
  tiempoUltimoCicloMotor = millis();
  tiempoUltimoMovimiento = 0;

  Serial.println("Sistema iniciado.");
  Serial.print("MPU cuello 0x69: "); Serial.println(okCuello ? "OK" : "NO DETECTADO");
  Serial.print("MPU espalda 0x68: "); Serial.println(okEspalda ? "OK" : "NO DETECTADO");
  Serial.println("Comandos por Serial/BLE: CALIBRAR, TEST, BORRAR_CAL");
  Serial.println("Motores en modo suave: pulsos cortos + PWM + cooldown.");
  Serial.println("Si se reinicia al vibrar, revisa transistor/MOSFET, diodo, capacitor y alimentacion.");
}

void loop() {
  revisarComandosSerial();

  unsigned long ahora = millis();

  if (peticionBorrarCal) {
    peticionBorrarCal = false;
    borrarCalibracion();
    tiempoPrevio = millis();
    tiempoUltimaLectura = millis();
    return;
  }

  if (peticionMotorTest) {
    peticionMotorTest = false;
    notificarBLE("MOTOR TEST SUAVE");
    pruebaMotores();
    tiempoUltimoCicloMotor = millis();
    return;
  }

  if (peticionCalibrar) {
    peticionCalibrar = false;
    calibrarPostura();
    tiempoPrevio = millis();
    tiempoUltimaLectura = millis();
    return;
  }

  if (ahora - tiempoUltimaLectura < INTERVALO_LECTURA_MS) {
    controlarMotores();
    return;
  }

  tiempoUltimaLectura = ahora;

  float dt = (ahora - tiempoPrevio) / 1000.0;
  tiempoPrevio = ahora;

  actualizarFiltro(sensorCuello, dt);
  actualizarFiltro(sensorEspalda, dt);
  actualizarAngulosFinales();

  movimientoFuerte = sensorCuello.movimiento || sensorEspalda.movimiento;
  if (movimientoFuerte) {
    tiempoUltimoMovimiento = ahora;
  }

  bool pausaPorMovimiento = PAUSAR_ALERTAS_EN_MOVIMIENTO &&
                            (ahora - tiempoUltimoMovimiento < TIEMPO_REPOSO_TRAS_MOVIMIENTO);

  if (pausaPorMovimiento) {
    movimientoFuerte = true;
    pausarAlertasPorMovimiento(ahora);
  } else {
    movimientoFuerte = false;

    actualizarZona(
      estadoCuello,
      valorDeteccionCuello,
      UMBRAL_CUELLO_MALO,
      UMBRAL_CUELLO_BUENO,
      ahora
    );

    actualizarZona(
      estadoEspalda,
      valorDeteccionEspalda,
      UMBRAL_ESPALDA_MALO,
      UMBRAL_ESPALDA_BUENO,
      ahora
    );

    controlarMotores();
  }

  String msg = construirMensajeBLE();
  notificarBLE(msg);
}
