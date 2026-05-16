# Posture_Awareness_Cloth

Un sistema *wearable* basado en ESP32 que monitorea la postura del usuario en tiempo real utilizando sensores inerciales (MPU6050) y proporciona retroalimentación háptica mediante motores de vibración. 

El sistema evalúa el ángulo del cuello y la espalda, si detecta una mala postura prolongada (más de 10 segundos), activa una secuencia de vibración para alertar al usuario. Además, se envía los datos en tiempo real a una aplicación móvil vía Bluetooth Low Energy (BLE).

## Hardware Utilizado

* **1x** ESP32 (Microcontrolador principal)
* **2x** MPU6050 (Acelerómetro y Giroscopio I2C) - *Uno para el cuello, otro para la espalda.*
* **2x** Motores de vibración tipo moneda (Coin motors).
* **1x** Batería / PowerBank portátil.
* Cables, bases (headers), chamarra.

## Características Principales

- **Filtro Complementario Integrado:** Combina datos del acelerómetro y giroscopio para obtener ángulos estables y sin ruido.
- **Calibración BLE:** Permite establecer el "punto cero" (offset) de la postura ideal directamente desde el celular.
- **Gestor de Motores Inteligente (Multiplexación):** Si ambos sensores detectan mala postura, los motores se turnan para vibrar. Esto evita picos de corriente que puedan calentar el ESP32 o drenr la bateria, y pueda ser peligroso para el usuario.
- **Tolerancia de Tiempo:** Evita falsas alarmas (ej. al agacharse a recoger algo) esperando 10 segundos continuos de mala postura antes de alertar.

## Esquema de Conexión Básico

| Componente | Pin ESP32 / Conexión |
| :--- | :--- |
| **MPU6050 (Cuello)** | SDA, SCL, VCC, GND. **AD0 -> GND** (Dirección 0x68) |
| **MPU6050 (Espalda)** | SDA, SCL, VCC, GND. **AD0 -> VCC** (Dirección 0x69) |
| **Motor Cuello** | Pin 4 |
| **Motor Espalda** | Pin 16 |



## Instalación y Uso

1. Clona este repositorio o descarga el código fuente.
2. Abre el archivo en el **Arduino IDE**.
3. Asegúrate de tener instaladas las bibliotecas necesarias en el IDLE de Arduino (normalmente incluidas en el core del ESP32):
   - `Wire.h`
   - `BLEDevice.h` (y dependencias BLE)
   - `Preferences.h`
4. Conecta tu ESP32, selecciona la placa adecuada y el puerto COM.
5. Sube el código.
6. Enciende el sistema. Se escuchara una secuencia de vibración que confirma el inicio correcto.
7. Conecta tu aplicación móvil mediante Bluetooth al dispositivo llamado **"SudaderaBLE"**.

## Comandos BLE Soportados

El dispositivo expone una característica de escritura (`UUID: "Copia el UUID DE TU DISP"`). 
* Enviar el texto `CALIBRAR` guardará la postura actual del usuario en la memoria no volátil (NVS) del ESP32 para usarla como punto de referencia.


## Licencia

Es un proyecto escolar. Siéntete libre de usarlo, modificarlo y adaptarlo a tus necesidades.
