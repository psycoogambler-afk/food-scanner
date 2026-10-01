import os
import sys
import glob
import subprocess

def find_mkspiffs():
    base = os.path.expandvars(r"%LOCALAPPDATA%\Arduino15\packages\esp32\tools\mkspiffs")
    matches = glob.glob(os.path.join(base, "*", "mkspiffs.exe"))
    return matches[-1] if matches else None

def find_esptool():
    base = os.path.expandvars(r"%LOCALAPPDATA%\Arduino15\packages\esp32\tools\esptool_py")
    matches = glob.glob(os.path.join(base, "*", "esptool.exe"))
    return matches[-1] if matches else None

def main():
    print("=================================================")
    print(" ESP32 Web Dashboard (SPIFFS) Auto-Flasher")
    print("=================================================")
    
    mkspiffs = find_mkspiffs()
    esptool = find_esptool()
    
    if not mkspiffs or not esptool:
        print("ERROR: Could not find 'mkspiffs' or 'esptool' on your system.")
        print("Please ensure you have installed the ESP32 boards in the Arduino IDE.")
        sys.exit(1)
        
    print("Please check your Arduino IDE to see which port your ESP32 is connected to.")
    port = input("Enter your ESP32's COM port (e.g. COM3): ").strip().upper()
    
    if not port.startswith("COM"):
        print("Invalid COM port. It should look like 'COM3'.")
        sys.exit(1)
        
    data_dir = os.path.join("smart_food_scanner", "data")
    if not os.path.exists(data_dir):
        print(f"ERROR: Cannot find data directory at {data_dir}")
        sys.exit(1)
        
    print("\n[1/2] Compiling the web files into a SPIFFS image...")
    # Default partition scheme config: offset 0x290000, size 0x170000
    spiffs_bin = "spiffs_image.bin"
    build_cmd = [mkspiffs, "-c", data_dir, "-b", "4096", "-p", "256", "-s", "0x170000", spiffs_bin]
    if subprocess.run(build_cmd).returncode != 0:
        print("ERROR: Failed to build SPIFFS image.")
        sys.exit(1)
        
    print(f"\n[2/2] Flashing to {port}... (You might need to hold the 'BOOT' button on your ESP32 if it fails to connect)")
    flash_cmd = [esptool, "--chip", "esp32", "--port", port, "--baud", "460800", "write_flash", "-z", "0x290000", spiffs_bin]
    
    if subprocess.run(flash_cmd).returncode != 0:
        print("ERROR: Failed to flash image to the board.")
        sys.exit(1)
        
    print("\n✅ SUCCESS! The web dashboard has been successfully flashed to your ESP32.")
    
    # Cleanup
    if os.path.exists(spiffs_bin):
        os.remove(spiffs_bin)

if __name__ == "__main__":
    main()
