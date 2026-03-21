import SwiftUI
import CoreBluetooth
internal import Combine

// UUIDs
let esp32ServiceCBUUID = CBUUID(string: "4fafc201-1fb5-459e-8fcc-c5c9c331914b")
let postureCharacteristicCBUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8") // Para recibir datos
let commandCharacteristicCBUUID = CBUUID(string: "12345678-1234-5678-1234-56789abcdef0") // NUEVO: Para enviar comandos

// administrador de Bluetooth
class BluetoothManager: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var centralManager: CBCentralManager!
    var esp32Peripheral: CBPeripheral?
    var commandCharacteristic: CBCharacteristic? // buzón de salida
    
    @Published var isConnected = false
    @Published var posturaTexto = "Esperando datos..."
    
    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }
    
    //validacion de seguridad, que el bluetooth este encendido y se inicia el escaneo del UUID
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            print("Bluetooth encendido. Buscando sudadera...")
            centralManager.scanForPeripherals(withServices: [esp32ServiceCBUUID])
        } else {
            print("Por favor, enciende el Bluetooth.")
        }
    }
    
    //Esta funcion almacena la referencia del esp32,
    //despues se delega asi mismo para recibir datos y al final se apaga para que ya no siga buscando
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        print("¡Sudadera encontrada!")
        esp32Peripheral = peripheral
        esp32Peripheral?.delegate = self
        centralManager.stopScan()
        centralManager.connect(esp32Peripheral!)
    }
    //funcion que actualiza el estado a conectado y pide los datos(UUID)
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("¡Conectado!")
        DispatchQueue.main.async { self.isConnected = true }
        esp32Peripheral?.discoverServices([esp32ServiceCBUUID])
    }
    
    //funcion que busca especificamente las direciones de los datos y el comando y si no encuentra nada regresa un error
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for service in services {
            // Buscamos ambas características ahora
            peripheral.discoverCharacteristics([postureCharacteristicCBUUID, commandCharacteristicCBUUID], for: service)
        }
    }
    
    // a esta funcion se le asignan
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == postureCharacteristicCBUUID {
                peripheral.setNotifyValue(true, for: characteristic)
            } else if characteristic.uuid == commandCharacteristicCBUUID {
                // Si encontramos el buzón de salida, lo guardamos para usarlo luego
                self.commandCharacteristic = characteristic
                print("¡Buzón de comandos encontrado!")
            }
        }
    }
    
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == postureCharacteristicCBUUID,
           let data = characteristic.value,
           let mensaje = String(data: data, encoding: .utf8) {
            DispatchQueue.main.async {
                self.posturaTexto = mensaje
            }
        }
    }
    
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("Desconectado. Buscando de nuevo...")
        DispatchQueue.main.async { self.isConnected = false }
        commandCharacteristic = nil // se borra la referencia al desconectar
        centralManager.scanForPeripherals(withServices: [esp32ServiceCBUUID])
    }
    
    // comando calibracion
    func enviarComandoCalibrar() {
        guard let peripheral = esp32Peripheral,
              let characteristic = commandCharacteristic else {
            print("Error: No se puede enviar comando. No conectado o característica no encontrada.")
            return
        }
        
        let comando = "CALIBRAR"
        if let data = comando.data(using: .utf8) {
            // Se envia el dato al esp32
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
            print("Comando de calibración enviado.")
        }
    }
}

// 3. La Interfaz Visual
struct ContentView: View {
    @StateObject var bleManager = BluetoothManager()
    
    var body: some View {
        VStack(spacing: 30) {
            Text("BackRight")
                .font(.largeTitle)
                .bold()
            
            HStack {
                Circle()
                    .fill(bleManager.isConnected ? Color.green : Color.red)
                    .frame(width: 20, height: 20)
                Text(bleManager.isConnected ? "Conectado al ESP32" : "Buscando sudadera...")
                    .foregroundColor(.gray)
            }
            
            // TARJETA 1: Solo muestra los números exactos
            VStack {
                Text("Postura Actual:")
                    .font(.headline)
                    .foregroundColor(.secondary)
                
                Text(bleManager.posturaTexto)
                    .font(.title2)
                    .bold()
                    .padding()
                    .multilineTextAlignment(.center)
            }
            .padding()
            .background(Color.blue.opacity(0.1))
            .cornerRadius(15)
            
            // TARJETA 2: Solo muestra el semáforo (Diagnóstico)
            VStack {
                Text("Estado de Postura:")
                    .font(.headline)
                    .foregroundColor(.secondary)
                
                // Lógica para determinar si la postura es buena o mala
                let esMalaPostura = bleManager.posturaTexto.contains("-") ||
                                   (Double(bleManager.posturaTexto.filter("0123456789.".contains)) ?? 0 > 20)

                Text(esMalaPostura ? "Slouching" : "Good posture")
                    .font(.largeTitle) // Lo hice un poco más grande para que resalte
                    .bold()
                    .foregroundColor(esMalaPostura ? .red : .green)
                    .padding(.vertical, 10)
            }
            .padding()
            .background(Color.blue.opacity(0.1))
            .cornerRadius(15)
            
            // Boton Calibrar
            Button(action: {
                bleManager.enviarComandoCalibrar()
                let generadorImpacto = UIImpactFeedbackGenerator(style: .medium)
                generadorImpacto.impactOccurred()
            }) {
                Text("Calibrar Postura")
                    .font(.headline)
                    .padding()
                    .frame(maxWidth: .infinity)
                    .background(bleManager.isConnected ? Color.orange : Color.gray)
                    .foregroundColor(.white)
                    .cornerRadius(15)
            }
            .disabled(!bleManager.isConnected)
            .padding(.horizontal, 40)
        }
        .padding()
    }
}

#Preview {
    ContentView()
}
