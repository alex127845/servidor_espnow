# ════════════════════════════════════════════════════════════════
# 📡 SERVIDOR TX HELTEC - FLASK CON GUI WEB
# ════════════════════════════════════════════════════════════════
# Instalación:
# pip install flask pyserial
# 
# Uso: python servidor_flask.py
# Abrir: http://localhost:5000
# ════════════════════════════════════════════════════════════════

from flask import Flask, render_template, request, jsonify, send_file
import serial
import threading
import time
import os
from pathlib import Path
import json

app = Flask(__name__)

# ════════════════════════════════════════════════════════════════
# 🔧 CONFIGURACIÓN
# ════════════════════════════════════════════════════════════════

UPLOAD_FOLDER = 'archivos'
ALLOWED_EXTENSIONS = {'bin', 'pdf', 'txt', 'jpg', 'png', 'zip'}

if not os.path.exists(UPLOAD_FOLDER):
    os.makedirs(UPLOAD_FOLDER)

app.config['UPLOAD_FOLDER'] = UPLOAD_FOLDER
app.config['MAX_CONTENT_LENGTH'] = 100 * 1024 * 1024  # 100 MB máximo

# ════════════════════════════════════════════════════════════════
# 📊 ESTADO GLOBAL
# ════════════════════════════════════════════════════════════════

class TransmissionState:
    def __init__(self):
        self.connected = False
        self.transmitting = False
        self.progress = 0
        self.status = "Desconectado"
        self.current_file = ""
        self.file_size = 0
        self.bytes_sent = 0
        self.total_packets = 0
        self.total_retries = 0
        self.transmission_time = 0.0
        self.error_message = ""
        self.ser = None
        self.port = ""
        self.baudrate = 115200
        self.power = 20
        self.channel = 1
        self.rate = 1

state = TransmissionState()
lock = threading.Lock()

# ════════════════════════════════════════════════════════════════
# 🔌 FUNCIONES SERIAL
# ════════════════════════════════════════════════════════════════

def connect_heltec(port):
    """Conectar con el Heltec - versión mejorada"""
    global state
    try:
        # Cerrar conexión anterior si existe
        if state.ser and state.ser.is_open:
            state.ser.close()
            time.sleep(0.5)
        
        with lock:
            state.ser = serial.Serial(
                port, 
                state.baudrate, 
                timeout=5,
                write_timeout=5
            )
            state.port = port
            state.connected = True
            state.status = f"Conectado en {port}"
        
        time.sleep(1)
        
        # LIMPIAR BUFFER: leer todo lo que haya en el puerto
        with lock:
            if state.ser and state.ser.in_waiting > 0:
                state.ser.reset_input_buffer()
        
        time.sleep(0.5)
        
        # Test conexión
        if ping_heltec():
            state.status = f"✅ Conectado en {port}"
            return True
        else:
            disconnect_heltec()
            state.error_message = "El Heltec no responde (PING fallido)"
            state.status = "❌ El Heltec no responde"
            return False
            
    except PermissionError as e:
        state.error_message = f"Acceso denegado en {port}. Intenta:\n1. Ejecutar como ADMINISTRADOR\n2. Cerrar Arduino IDE\n3. Reconectar el USB"
        state.status = f"❌ Permiso denegado"
        return False
        
    except serial.SerialException as e:
        state.error_message = f"Puerto no disponible: {e}"
        state.status = f"❌ Puerto no disponible"
        return False
        
    except Exception as e:
        state.error_message = str(e)
        state.status = f"❌ Error: {e}"
        return False

def disconnect_heltec():
    """Desconectar"""
    global state
    with lock:
        if state.ser:
            state.ser.close()
        state.connected = False
        state.status = "Desconectado"

def ping_heltec():
    """Test de conexión - mejorado"""
    try:
        with lock:
            if not state.ser or not state.ser.is_open:
                return False
            state.ser.write(b'PING\n')
        
        time.sleep(1)  # Aumentar a 1 segundo
        
        # Leer todas las líneas disponibles
        response = ""
        timeout_counter = 0
        while timeout_counter < 20:  # 2 segundos máximo
            with lock:
                if state.ser and state.ser.in_waiting > 0:
                    line = state.ser.readline().decode('utf-8', errors='ignore').strip()
                    response += line + "\n"
                    if "PONG" in line:
                        print(f"✅ PING OK: {response}")
                        return True
            time.sleep(0.1)
            timeout_counter += 1
        
        print(f"⚠️ PING sin respuesta clara: {response}")
        return False
        
    except Exception as e:
        print(f"❌ Error en PING: {e}")
        return False

def send_command(cmd):
    """Enviar comando"""
    try:
        with lock:
            if not state.ser or not state.ser.is_open:
                return False
            state.ser.write(cmd.encode() + b'\n')
        return True
    except:
        return False

def read_response(timeout=2):
    """Leer respuesta - mejorado"""
    try:
        with lock:
            if not state.ser or not state.ser.is_open:
                return None
            
            start = time.time()
            response = ""
            
            while time.time() - start < timeout:
                if state.ser.in_waiting > 0:
                    char = state.ser.read(1).decode('utf-8', errors='ignore')
                    response += char
                    
                    # Si encontramos una línea completa, retornarla
                    if char == '\n':
                        line = response.strip()
                        if line and not line.startswith('📩'):  # Ignorar logs del Heltec
                            return line
                
                time.sleep(0.01)
        
        return None if not response else response.strip()
        
    except Exception as e:
        print(f"❌ Error leyendo respuesta: {e}")
        return None

# Tamaño del chunk a enviar (1 KB)
CHUNK_SIZE = 1024

def send_file_thread(filepath):
    """Enviar archivo por chunks - MEJORADO"""
    global state
    
    try:
        file_path = Path(filepath)
        
        if not file_path.exists():
            state.error_message = "Archivo no existe"
            state.transmitting = False
            return
        
        state.current_file = file_path.name
        state.file_size = file_path.stat().st_size
        state.bytes_sent = 0
        state.progress = 0
        state.total_packets = 0
        state.error_message = ""
        state.status = "Iniciando transmisión..."
        
        print(f"\n╔════════════════════════════════════════╗")
        print(f"║  📡 TRANSMISIÓN POR CHUNKS            ║")
        print(f"║  Archivo: {file_path.name:<27}║")
        print(f"║  Tamaño: {state.file_size} bytes{' '*(30-len(str(state.file_size)))}║")
        print(f"║  Chunk size: {CHUNK_SIZE} bytes{' '*(26-len(str(CHUNK_SIZE)))}║")
        print(f"╚════════════════════════════════════════╝\n")
        
        # 1. Enviar comando START
        cmd = f"START:{file_path.name}:{state.file_size}"
        print(f"📤 [1/3] Enviando START...")
        send_command(cmd)
        
        # Esperar OK:READY_TO_RX
        print(f"⏳ Esperando ACK...")
        response = read_response(timeout=5)
        
        if not response or "OK:READY_TO_RX" not in response:
            state.error_message = f"Heltec no respondió: {response}"
            state.status = "❌ Sin respuesta"
            state.transmitting = False
            print(f"❌ Error: {state.error_message}")
            return
        
        print(f"✅ Recibido: {response}\n")
        state.status = "Enviando datos..."
        
        # 2. Enviar archivo por chunks
        print(f"📤 [2/3] Enviando {(state.file_size + CHUNK_SIZE - 1) // CHUNK_SIZE} chunks...\n")
        
        chunk_num = 0
        with open(file_path, 'rb') as f:
            while True:
                chunk = f.read(CHUNK_SIZE)
                if not chunk:
                    break
                
                # Enviar chunk como binario
                with lock:
                    if not state.ser or not state.ser.is_open:
                        state.error_message = "Conexión perdida"
                        state.transmitting = False
                        return
                    state.ser.write(chunk)
                
                state.bytes_sent += len(chunk)
                state.progress = int((state.bytes_sent / state.file_size) * 100)
                chunk_num += 1
                
                # Mostrar progreso cada 10 chunks o último chunk
                if chunk_num % 10 == 0 or state.bytes_sent >= state.file_size:
                    bar_length = 40
                    filled = int(bar_length * state.progress / 100)
                    bar = '█' * filled + '░' * (bar_length - filled)
                    print(f"  [{bar}] {state.progress}% ({state.bytes_sent}/{state.file_size} bytes) - {chunk_num} chunks")
                
                # Esperar un poco entre chunks (para permitir lectura en Heltec)
                time.sleep(0.01)
        
        print(f"\n✅ Todos los datos enviados\n")
        state.status = "Finalizando..."
        time.sleep(0.5)
        
        # 3. Enviar END
        print(f"📤 [3/3] Finalizando...")
        send_command("END")
        
        # Esperar confirmación TX_COMPLETE
        print(f"⏳ Esperando transmisión por ESP-NOW...\n")
        
        timeout_counter = 0
        max_timeout = 180  # 3 minutos máximo
        
        while timeout_counter < max_timeout:
            response = read_response(timeout=1)
            if response:
                print(f"📩 {response}")
                if "TX_COMPLETE" in response:
                    try:
                        parts = response.split(':')
                        state.total_packets = int(parts[1]) if len(parts) > 1 else 0
                        state.transmission_time = float(parts[2]) if len(parts) > 2 else 0.0
                    except:
                        pass
                    
                    state.status = "✅ Transmisión completada"
                    state.progress = 100
                    state.transmitting = False
                    
                    print(f"\n╔════════════════════════════════════════╗")
                    print(f"║  ✅ TRANSMISIÓN EXITOSA              ║")
                    print(f"║  Paquetes: {state.total_packets:<30}║")
                    print(f"║  Tiempo: {state.transmission_time:.2f}s{' '*(31-len(f'{state.transmission_time:.2f}'))}║")
                    print(f"╚════════════════════════════════════════╝\n")
                    return
            
            timeout_counter += 1
        
        state.error_message = "Timeout esperando TX_COMPLETE"
        state.status = "❌ Timeout"
        state.transmitting = False
        print(f"❌ {state.error_message}")
        
    except Exception as e:
        state.error_message = str(e)
        state.status = f"❌ Error: {e}"
        state.transmitting = False
        print(f"❌ Excepción: {e}")
        import traceback
        traceback.print_exc()


# ════════════════════════════════════════════════════════════════
# 🌐 RUTAS FLASK
# ════════════════════════════════════════════════════════════════

@app.route('/')
def index():
    """Página principal"""
    return render_template('index.html')

@app.route('/api/ports', methods=['GET'])
def get_ports():
    """Obtener puertos COM disponibles"""
    try:
        import serial.tools.list_ports
        ports = []
        for port, desc, hwid in serial.tools.list_ports.comports():
            ports.append({
                'port': port,
                'description': desc,
                'hwid': hwid
            })
        return jsonify(ports)
    except Exception as e:
        return jsonify({'error': str(e)}), 500

@app.route('/api/connect', methods=['POST'])
def connect():
    """Conectar con Heltec"""
    data = request.json
    port = data.get('port')
    
    if connect_heltec(port):
        return jsonify({'success': True, 'message': 'Conectado'})
    else:
        return jsonify({'success': False, 'message': state.error_message}), 400

@app.route('/api/disconnect', methods=['POST'])
def disconnect():
    """Desconectar"""
    disconnect_heltec()
    return jsonify({'success': True})

@app.route('/api/status', methods=['GET'])
def get_status():
    """Obtener estado actual"""
    with lock:
        return jsonify({
            'connected': state.connected,
            'transmitting': state.transmitting,
            'progress': state.progress,
            'status': state.status,
            'current_file': state.current_file,
            'file_size': state.file_size,
            'bytes_sent': state.bytes_sent,
            'total_packets': state.total_packets,
            'error_message': state.error_message,
            'port': state.port,
            'power': state.power,
            'channel': state.channel,
            'rate': state.rate,
            'transmission_time': state.transmission_time
        })

@app.route('/api/upload', methods=['POST'])
def upload_file():
    """Subir archivo"""
    if 'file' not in request.files:
        return jsonify({'error': 'No file'}), 400
    
    file = request.files['file']
    if file.filename == '':
        return jsonify({'error': 'Empty filename'}), 400
    
    if not state.connected:
        return jsonify({'error': 'Not connected'}), 400
    
    if state.transmitting:
        return jsonify({'error': 'Already transmitting'}), 400
    
    # Guardar archivo
    filename = file.filename
    filepath = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    file.save(filepath)
    
    # Iniciar transmisión en hilo
    state.transmitting = True
    thread = threading.Thread(target=send_file_thread, args=(filepath,))
    thread.daemon = True
    thread.start()
    
    return jsonify({'success': True, 'message': 'Transmisión iniciada'})

@app.route('/api/config', methods=['POST'])
def set_config():
    """Configurar ESP-NOW"""
    data = request.json
    power = data.get('power', 20)
    channel = data.get('channel', 1)
    rate = data.get('rate', 1)
    
    state.power = power
    state.channel = channel
    state.rate = rate
    
    cmd = f"CONFIG:{power}:{channel}:{rate}"
    send_command(cmd)
    
    response = read_response(timeout=3)
    
    return jsonify({'success': True, 'response': response})

@app.route('/api/files', methods=['GET'])
def list_files():
    """Listar archivos disponibles"""
    files = []
    if os.path.exists(app.config['UPLOAD_FOLDER']):
        for f in os.listdir(app.config['UPLOAD_FOLDER']):
            filepath = os.path.join(app.config['UPLOAD_FOLDER'], f)
            if os.path.isfile(filepath):
                size = os.path.getsize(filepath)
                files.append({
                    'name': f,
                    'size': size,
                    'size_formatted': format_size(size)
                })
    return jsonify(files)

@app.route('/api/delete-file', methods=['POST'])
def delete_file():
    """Eliminar archivo"""
    data = request.json
    filename = data.get('filename')
    
    filepath = os.path.join(app.config['UPLOAD_FOLDER'], filename)
    
    if os.path.exists(filepath):
        os.remove(filepath)
        return jsonify({'success': True})
    
    return jsonify({'success': False, 'error': 'File not found'}), 404

# ════════════════════════════════════════════════════════════════
# 🛠️ FUNCIONES AUXILIARES
# ════════════════════════════════════════════════════════════════

def format_size(bytes):
    """Formatear tamaño de archivo"""
    for unit in ['B', 'KB', 'MB', 'GB']:
        if bytes < 1024:
            return f"{bytes:.2f} {unit}"
        bytes /= 1024
    return f"{bytes:.2f} TB"

# ════════════════════════════════════════════════════════════════
# 🚀 MAIN
# ════════════════════════════════════════════════════════════════

if __name__ == '__main__':
    print("\n════════════════════════════════════════════════")
    print("  📡 SERVIDOR TX HELTEC - FLASK GUI")
    print("════════════════════════════════════════════════")
    print("\n🌐 Abre en tu navegador: http://localhost:5000")
    print("📁 Carpeta de archivos: ./archivos\n")
    
    app.run(debug=True, host='0.0.0.0', port=5000)