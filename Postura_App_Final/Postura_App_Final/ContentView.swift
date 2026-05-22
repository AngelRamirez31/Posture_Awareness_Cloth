import SwiftUI
import CoreBluetooth
import CoreML
internal import Combine

// Identificadores únicos UUID
let esp32ServiceCBUUID = CBUUID(string: "4fafc201-1fb5-459e-8fcc-c5c9c331914b")
let postureCharacteristicCBUUID = CBUUID(string: "beb5483e-36e1-4688-b7f5-ea07361b26a8") // para recibir datos
let commandCharacteristicCBUUID = CBUUID(string: "12345678-1234-5678-1234-56789abcdef0") // para enviar comandos

// Administrador de Bluetooth y Procesamiento de Datos
class BluetoothManager: NSObject, ObservableObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    var centralManager: CBCentralManager!
    var esp32Peripheral: CBPeripheral?
    var commandCharacteristic: CBCharacteristic? // nuestro buzón de salida
    
    // Instancia del modelo de IA cargada UNA sola vez para evitar colapsar la memoria
    var aiModel: PostureCoachScore2?
    
    @Published var isConnected = false
    @Published var posturaTexto = "Esperando datos..."
    @Published var estadoPosturaTexto = "Esperando datos..."
    @Published var esMalaPostura = false
    @Published var scoreIA: Double = 100.0
    
    override init() {
        super.init()
        centralManager = CBCentralManager(delegate: self, queue: nil)
        
        // Inicializamos el modelo de CoreML aquí para no sobrecargar la interfaz gráfica
        do {
            let config = MLModelConfiguration()
            self.aiModel = try PostureCoachScore2(configuration: config)
            print("¡Modelo de IA cargado exitosamente en memoria!")
        } catch {
            print("Error crítico al inicializar el modelo de IA: \(error)")
        }
    }
    
    // Validación de seguridad, que el bluetooth esté encendido y se inicia el escaneo del UUID
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            print("Bluetooth encendido. Buscando sudadera...")
            centralManager.scanForPeripherals(withServices: [esp32ServiceCBUUID])
        } else {
            print("Por favor, enciende el Bluetooth.")
        }
    }
    
    // Esta función almacena la referencia del esp32,
    // después se delega así mismo para recibir datos y al final se apaga para que ya no siga buscando
    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        print("¡Sudadera encontrada!")
        esp32Peripheral = peripheral
        esp32Peripheral?.delegate = self
        centralManager.stopScan()
        centralManager.connect(esp32Peripheral!)
    }
    
    // Función que actualiza el estado a conectado y pide los datos(UUID)
    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        print("¡Conectado!")
        DispatchQueue.main.async { self.isConnected = true }
        esp32Peripheral?.discoverServices([esp32ServiceCBUUID])
    }
    
    // Función que busca específicamente las direcciones de los datos y el comando y si no encuentra nada regresa un error
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let services = peripheral.services else { return }
        for service in services {
            // busca ambas direcciones
            peripheral.discoverCharacteristics([postureCharacteristicCBUUID, commandCharacteristicCBUUID], for: service)
        }
    }
    
    // A esta función se le asignan las notificaciones para que lea en tiempo real y guarda el buzón de comandos
    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        guard let characteristics = service.characteristics else { return }
        for characteristic in characteristics {
            if characteristic.uuid == postureCharacteristicCBUUID {
                peripheral.setNotifyValue(true, for: characteristic)
            } else if characteristic.uuid == commandCharacteristicCBUUID {
                // si encuentra el buzón lo guardamos para después
                self.commandCharacteristic = characteristic
                print("¡Buzón de comandos encontrado!")
            }
        }
    }
    
    // Función central: lee lo que manda el ESP32.
    // Importante: ya NO usamos abs(cuello - espalda), porque eso genera falsos positivos.
    // El ESP32 manda ESTADO: GOOD o ESTADO: SLOUCHING usando histéresis y curva relativa.
    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        if characteristic.uuid == postureCharacteristicCBUUID,
           let data = characteristic.value,
           let mensaje = String(data: data, encoding: .utf8) {
            
            let datos = extraerDatosPosturaDesdeTexto(mensaje)
            
            var nuevoEstadoTexto = "Analizando..."
            var malaPosturaDetectada = false
            var nuevoScoreIA = self.scoreIA
            
            if mensaje.contains("CALIBRANDO") {
                nuevoEstadoTexto = "Calibrando..."
                malaPosturaDetectada = false
            } else if mensaje.contains("CALIBRACION LISTA") {
                nuevoEstadoTexto = "Calibración lista"
                malaPosturaDetectada = false
            } else if let estadoESP32 = datos.estado {
                if estadoESP32.uppercased().contains("SLOUCHING") {
                    nuevoEstadoTexto = "Slouching"
                    malaPosturaDetectada = true
                } else {
                    nuevoEstadoTexto = "Active - Good posture"
                    malaPosturaDetectada = false
                }
            } else if datos.encontroCuello && datos.encontroEspalda {
                // Respaldo por si algún día el ESP32 no manda ESTADO.
                // Se usa curva firmada, no abs().
                let curva = datos.curva ?? (datos.cuello - datos.espalda)
                if curva > 22.0 {
                    nuevoEstadoTexto = "Slouching"
                    malaPosturaDetectada = true
                } else {
                    nuevoEstadoTexto = "Active - Good posture"
                    malaPosturaDetectada = false
                }
            } else if mensaje.contains("Esperando") {
                nuevoEstadoTexto = "Esperando datos..."
            }
            
            if datos.encontroCuello && datos.encontroEspalda, let model = self.aiModel {
                do {
                    let input = PostureCoachScore2Input(
                        AnguloCuello: Int64(datos.cuello),
                        AnguloEspalda: Int64(datos.espalda)
                    )
                    let prediction = try model.prediction(input: input)
                    nuevoScoreIA = prediction.Score
                } catch {
                    print("Error al realizar la predicción: \(error)")
                }
            }
            
            DispatchQueue.main.async {
                self.posturaTexto = mensaje
                self.estadoPosturaTexto = nuevoEstadoTexto
                self.esMalaPostura = malaPosturaDetectada
                self.scoreIA = nuevoScoreIA
            }
        }
    }
    
    // Helper para leer CUELLO, ESPALDA, CURVA y ESTADO desde el texto del ESP32.
    private func extraerDatosPosturaDesdeTexto(_ texto: String) -> (cuello: Double, espalda: Double, curva: Double?, estado: String?, encontroCuello: Bool, encontroEspalda: Bool) {
        var cuello = 0.0
        var espalda = 0.0
        var curva: Double? = nil
        var estado: String? = nil
        var encontroCuello = false
        var encontroEspalda = false
        
        let lineas = texto.split(separator: "\n")
        for lineaSub in lineas {
            let linea = String(lineaSub)
            let partes = linea.split(separator: ":", maxSplits: 1)
            guard partes.count == 2 else { continue }
            
            let etiqueta = partes[0].trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            let valor = partes[1]
                .replacingOccurrences(of: "°", with: "")
                .trimmingCharacters(in: .whitespacesAndNewlines)
            
            if etiqueta.contains("cuello"), let angulo = Double(valor) {
                cuello = angulo
                encontroCuello = true
            } else if etiqueta.contains("espalda"), let angulo = Double(valor) {
                espalda = angulo
                encontroEspalda = true
            } else if etiqueta.contains("curva"), let angulo = Double(valor) {
                curva = angulo
            } else if etiqueta.contains("estado") {
                estado = valor
            }
        }
        
        return (cuello, espalda, curva, estado, encontroCuello, encontroEspalda)
    }
    
    // función que borra todo si se desconecta y vuelve a buscar
    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        print("Desconectado. Buscando de nuevo...")
        DispatchQueue.main.async {
            self.isConnected = false
            self.estadoPosturaTexto = "Buscando sudadera..."
        }
        commandCharacteristic = nil // se borra la referencia al desconectar
        centralManager.scanForPeripherals(withServices: [esp32ServiceCBUUID])
    }
    
    // comando calibración
    func enviarComandoCalibrar() {
        guard let peripheral = esp32Peripheral,
              let characteristic = commandCharacteristic else {
            print("Error: No se puede enviar comando. No conectado o característica no encontrada.")
            return
        }
        
        let comando = "CALIBRAR"
        if let data = comando.data(using: .utf8) {
            // Se envía el dato al esp32
            peripheral.writeValue(data, for: characteristic, type: .withResponse)
            print("Comando de calibración enviado.")
        }
    }
}

// Vista principal con las pestañas
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
            RecomendacionesView(bleManager: bleManager)
                .tabItem {
                    Label("IA Coach", systemImage: "brain.head.profile")
                }
        }
        .accentColor(.blue)
    }
}

// --- pestaña 1: calibración y monitor ---
struct MonitorView: View {
    @ObservedObject var bleManager: BluetoothManager
    @State private var mostrarGrados = false // Controla el menú desplegable opcional
    
    // Define el color del texto basado en el estado actual de la postura
    func obtenerColorEstado() -> Color {
        if bleManager.estadoPosturaTexto == "Esperando datos..." || bleManager.estadoPosturaTexto == "Analizando..." {
            return .gray
        }
        return bleManager.esMalaPostura ? .red : .green
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
            
            // Sección Principal Límpia: Solo muestra el Estado de la Postura solicitado
            VStack {
                Text("Estado de Postura:")
                    .font(.headline)
                    .foregroundColor(.secondary)
                
                Text(bleManager.estadoPosturaTexto)
                    .font(.title).bold()
                    .foregroundColor(obtenerColorEstado())
                    .padding(.vertical, 10)
                    .multilineTextAlignment(.center)
            }
            .padding()
            .frame(maxWidth: .infinity)
            .background(Color.blue.opacity(0.1))
            .cornerRadius(15)
            
            // Sección Opcional: Desplegable por si quiere saber los grados de los sensores
            DisclosureGroup("Ver grados detallados", isExpanded: $mostrarGrados) {
                VStack {
                    Text(bleManager.posturaTexto)
                        .font(.body)
                        .fontWeight(.medium)
                        .multilineTextAlignment(.center)
                        .padding(.top, 10)
                        .foregroundColor(.primary)
                }
            }
            .padding()
            .background(Color.blue.opacity(0.05))
            .cornerRadius(15)
            .accentColor(.blue)
            
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
    @ObservedObject var bleManager: BluetoothManager
    
    // Determina el color del círculo del Score de forma reactiva
    func obtenerColorParaScore(_ score: Double) -> Color {
        if score >= 80 { return .green }
        else if score >= 50 { return .orange }
        else { return .red }
    }

    // Cambia los textos de las recomendaciones de la IA basándose en los datos del manager
    func obtenerRecomendacionIA() -> (titulo: String, consejo: String, icono: String) {
        if bleManager.posturaTexto.contains("Esperando") {
            return ("Analizando...", "Ponte la sudadera y conéctate para que la IA evalúe tus hábitos.", "magnifyingglass")
        }
        
        if bleManager.esMalaPostura {
            return ("Tensión en la Columna", "Se detectó una curva relativa de cuello/espalda fuera del rango calibrado. Intenta alinear tu torso y relajar los hombros.", "figure.flexibility")
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
                
                // Círculo dinámico del Score de IA
                if !bleManager.posturaTexto.contains("Esperando") {
                    let colorScore = obtenerColorParaScore(bleManager.scoreIA)
                    
                    VStack {
                        ZStack {
                            Circle()
                                .stroke(lineWidth: 15)
                                .opacity(0.2)
                                .foregroundColor(Color.gray)
                            
                            Circle()
                                .trim(from: 0.0, to: CGFloat(min(max(bleManager.scoreIA / 100.0, 0.0), 1.0)))
                                .stroke(style: StrokeStyle(lineWidth: 15, lineCap: .round, lineJoin: .round))
                                .foregroundColor(colorScore)
                                .rotationEffect(Angle(degrees: 270.0))
                                .animation(.easeInOut(duration: 0.5), value: bleManager.scoreIA)
                            
                            VStack {
                                Text("\(Int(bleManager.scoreIA))")
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
                
                // Tarjeta inferior con el consejo dinámico
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
