import SwiftUI
import CoreBluetooth
import CoreML
internal import Combine

// identificadores unicos UUID
let esp32ServiceCBUUID = CBUUID(string: "4fafc201-1fb5-459e-8fcc-c5c9c331914b")
let postureCharacteristicCBUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8") // para recibir datos
let commandCharacteristicCBUUID = CBUUID(string: "12345678-1234-5678-1234-56789abcdef0") // para enviar comandos

// administrador de Bluetooth
class BluetoothManager: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var centralManager: CBCentralManager!
    var esp32Peripheral: CBPeripheral?
    var commandCharacteristic: CBCharacteristic? // nuestro buzon de salida
    
    @Published var isConnected = false
    @Published var posturaTexto = "Esperando datos..."
    
    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
    }
    
    // validacion de seguridad, que el bluetooth este encendido y se inicia el escaneo del UUID
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
            // busca ambas direcciones
            peripheral.discoverCharacteristics([postureCharacteristicCBUUID, commandCharacteristicCBUUID], for: service)
        }
    }
    
    // a esta funcion se le asignan las notificaciones para que lea en tiempo real y guarda el buzon de comandos
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == postureCharacteristicCBUUID {
                peripheral.setNotifyValue(true, for: characteristic)
            } else if characteristic.uuid == commandCharacteristicCBUUID {
                // si encuentra el buzon lo guardamos para despues
                self.commandCharacteristic = characteristic
                print("¡Buzón de comandos encontrado!")
            }
        }
    }
    
    // funcion para leer lo que llega del sensor y actualizar el texto de la pantalla
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == postureCharacteristicCBUUID,
           let data = characteristic.value,
           let mensaje = String(data: data, encoding: .utf8) {
            DispatchQueue.main.async {
                self.posturaTexto = mensaje
            }
        }
    }
    
    // funcion que borra todo si se desconecta y vuelve a buscar
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

// vista principal con las pestañas
struct ContentView: View {
    @StateObject var bleManager = BluetoothManager()
    
    var body: some View {
        TabView {
            // pestaña 1: el monitor
            MonitorView(bleManager: bleManager)
                .tabItem {
                    Label("Monitor", systemImage: "figure.stand")
                }
            
            // pestaña 2: el coach de inteligencia artificial
            RecomendacionesView(posturaTexto: bleManager.posturaTexto)
                .tabItem {
                    Label("IA Coach", systemImage: "brain.head.profile")
                }
        }
        .accentColor(.blue)
    }
}

// --- pestaña 1: calibracion y monitor ---
struct MonitorView: View {
    @ObservedObject var bleManager: BluetoothManager
    
    // funcion para revisar si el angulo pasa de 15 grados y marcar mala postura
    func evaluarPostura(texto: String) -> Bool {
        if texto == "Esperando datos..." { return false }
        let lineas = texto.split(separator: "\n")
        for linea in lineas {
            let partes = linea.split(separator: ":")
            if partes.count == 2 {
                let numeroLimpio = partes[1].replacingOccurrences(of: "°", with: "").trimmingCharacters(in: .whitespaces)
                if let angulo = Double(numeroLimpio) {
                    if abs(angulo) > 15.0 { return true }
                }
            }
        }
        return false
    }
    
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
            
            VStack {
                Text("Postura Actual:")
                    .font(.headline).foregroundColor(.secondary)
                Text(bleManager.posturaTexto)
                    .font(.title2).bold().padding().multilineTextAlignment(.center)
            }
            .padding().background(Color.blue.opacity(0.1)).cornerRadius(15)
            
            VStack {
                Text("Estado de Postura:")
                    .font(.headline).foregroundColor(.secondary)
                
                let esMalaPostura = evaluarPostura(texto: bleManager.posturaTexto)
                Text(esMalaPostura ? "Slouching" : "Good posture")
                    .font(.largeTitle).bold()
                    .foregroundColor(esMalaPostura ? .red : .green)
                    .padding(.vertical, 10)
            }
            .padding().background(Color.blue.opacity(0.1)).cornerRadius(15)
            
            Spacer()
            
            Button(action: {
                bleManager.enviarComandoCalibrar()
                let generadorImpacto = UIImpactFeedbackGenerator(style: .medium)
                generadorImpacto.impactOccurred()
            }) {
                Text("Calibrar Postura")
                    .font(.headline).padding().frame(maxWidth: .infinity)
                    .background(bleManager.isConnected ? Color.orange : Color.gray)
                    .foregroundColor(.white).cornerRadius(15)
            }
            .disabled(!bleManager.isConnected)
            .padding(.horizontal, 40)
            .padding(.bottom, 20)
        }
        .padding()
    }
}

// --- pestaña 2: modelo de IA ---
struct RecomendacionesView: View {
    var posturaTexto: String
    
    
    // funcion que saca los puros numeros de cuello y espalda del texto del bluetooth
    func extraerAngulos(texto: String) -> (cuello: Double, espalda: Double) {
        var cuello = 0.0
        var espalda = 0.0
        let lineas = texto.split(separator: "\n")
        for linea in lineas {
            let partes = linea.split(separator: ":")
            if partes.count == 2 {
                let numero = Double(partes[1].replacingOccurrences(of: "°", with: "").trimmingCharacters(in: .whitespaces)) ?? 0.0
                if linea.lowercased().contains("cuello") { cuello = numero }
                if linea.lowercased().contains("espalda") { espalda = numero }
            }
        }
        return (cuello, espalda)
    }
    
    // funcion que corre el modelo de core ml y saca el score
    func calcularScoreConIA(anguloCuello: Double, anguloEspalda: Double) -> Double {
        do {
            let config = MLModelConfiguration()
            let model = try PostureCoachScore2(configuration: config)
            
            let input = PostureCoachScore2Input(
                AnguloCuello: Int64(anguloCuello),
                AnguloEspalda: Int64(anguloEspalda)
            )
            
            let prediction = try model.prediction(input: input)
            return prediction.Score // nos regresa de 0 a 100
        } catch {
            print("Error al usar el modelo de IA: \(error)")
            return 0.0
        }
    }
    
    // funcion para pintar el circulo dependiendo de la calificacion
    func obtenerColorParaScore(_ score: Double) -> Color {
        if score >= 80 { return .green }
        else if score >= 50 { return .orange }
        else { return .red }
    }

    // funcion para cambiar los textos y el icono del consejo
    func obtenerRecomendacionIA() -> (titulo: String, consejo: String, icono: String) {
        if posturaTexto.contains("Esperando") {
            return ("Analizando...", "Ponte la sudadera y conéctate para que la IA evalúe tus hábitos.", "magnifyingglass")
        }
        
        // logica rapida para dar el consejo
        if posturaTexto.contains("-") || posturaTexto.contains("20") {
            return ("Tensión en el Cuello", "Se detecto que inclinas mucho la cabeza. Recomendación: Haz estiramientos de barbilla al pecho por 30 segundos.", "figure.flexibility")
        } else {
            return ("Postura Óptima", "Tu alineación espinal es correcta. Mantén tu monitor a la altura de los ojos para seguir así.", "star.fill")
        }
    }
    
    var body: some View {
        NavigationView {
            VStack(spacing: 20) {
                
                Image(systemName: "brain.head.profile")
                    .resizable()
                    .scaledToFit()
                    .frame(width: 80, height: 80)
                    .foregroundColor(.purple)
                    .padding(.top, 20)
                
                Text("Coach Inteligente")
                    .font(.title)
                    .bold()
                
                Text("Recomendaciones generadas basadas en tu historial de postura.")
                    .font(.subheadline)
                    .foregroundColor(.gray)
                    .multilineTextAlignment(.center)
                    .padding(.horizontal)
                
                // el circulo grandote con la calificacion
                if !posturaTexto.contains("Esperando") {
                    let angulos = extraerAngulos(texto: posturaTexto)
                    let scoreIA = calcularScoreConIA(anguloCuello: angulos.cuello, anguloEspalda: angulos.espalda)
                    let colorScore = obtenerColorParaScore(scoreIA)
                    
                    VStack {
                        ZStack {
                            Circle()
                                .stroke(lineWidth: 15)
                                .opacity(0.2)
                                .foregroundColor(Color.gray)
                            
                            Circle()
                                .trim(from: 0.0, to: CGFloat(min(max(scoreIA / 100.0, 0.0), 1.0)))
                                .stroke(style: StrokeStyle(lineWidth: 15, lineCap: .round, lineJoin: .round))
                                .foregroundColor(colorScore)
                                .rotationEffect(Angle(degrees: 270.0))
                                .animation(.easeInOut(duration: 0.5), value: scoreIA)
                            
                            VStack {
                                Text("\(Int(scoreIA))")
                                    .font(.system(size: 50, weight: .bold, design: .rounded))
                                    .foregroundColor(colorScore)
                                Text("Score")
                                    .font(.caption)
                                    .foregroundColor(.gray)
                            }
                        }
                        .frame(width: 140, height: 140)
                        .padding(.vertical, 10)
                    }
                }
                
                let ia = obtenerRecomendacionIA()
                
                // la tarjetita de abajo con la recomendacion
                VStack(alignment: .leading, spacing: 15) {
                    HStack {
                        Image(systemName: ia.icono)
                            .font(.title)
                            .foregroundColor(.purple)
                        Text(ia.titulo)
                            .font(.title3)
                            .bold()
                    }
                    
                    Text(ia.consejo)
                        .font(.body)
                        .fixedSize(horizontal: false, vertical: true)
                }
                .padding()
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.purple.opacity(0.1))
                .cornerRadius(15)
                .padding(.horizontal)
                
                Spacer()
            }
            .navigationBarHidden(true)
        }
    }
}

#Preview {
    ContentView()
}

