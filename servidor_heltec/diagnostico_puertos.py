import serial
import serial.tools.list_ports
import time

print("\n════════════════════════════════════════════════")
print("  🔍 DIAGNÓSTICO DE PUERTOS COM")
print("════════════════════════════════════════════════\n")

# 1. Listar puertos disponibles
print("📋 Puertos disponibles:")
ports = serial.tools.list_ports.comports()
if not ports:
    print("   ❌ No se encontraron puertos COM")
else:
    for port, desc, hwid in ports:
        print(f"   ✅ {port}: {desc}")
        print(f"      Hardware: {hwid}\n")

# 2. Intentar conectar a cada puerto
print("\n🔌 Intentando conectar a cada puerto...\n")
for port, desc, hwid in ports:
    print(f"   Probando {port}...", end=" ")
    try:
        ser = serial.Serial(port, 115200, timeout=2)
        time.sleep(0.5)
        
        # Enviar PING
        ser.write(b'PING\n')
        time.sleep(0.5)
        
        if ser.in_waiting > 0:
            response = ser.readline().decode('utf-8', errors='ignore').strip()
            if "PONG" in response:
                print(f"✅ EXITO - Respuesta: {response}")
            else:
                print(f"⚠️  Respuesta inesperada: {response}")
        else:
            print("⚠️  Sin respuesta")
        
        ser.close()
        
    except PermissionError:
        print("❌ PERMISO DENEGADO")
    except Exception as e:
        print(f"❌ Error: {e}")

print("\n════════════════════════════════════════════════\n")