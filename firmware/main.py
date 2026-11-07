import machine
import sys
import select

led = machine.Pin("LED", machine.Pin.OUT)
sensor_temp = machine.ADC(4)
conversion_factor = 3.3 / 65535

poller = select.poll()
poller.register(sys.stdin, select.POLLIN)

while True:
    events = poller.poll(50)
    if events:
        raw = sys.stdin.readline().strip().lower()
        
        if raw == "identify":
            sys.stdout.write("PICO_W_OK\n")
            
        elif raw == "led 1":
            led.value(1)
            sys.stdout.write("LED_ON\n")
        elif raw == "led 0":
            led.value(0)
            sys.stdout.write("LED_OFF\n")

        elif raw == "cpu -t":
            reading = sensor_temp.read_u16() * conversion_factor
            temperature = 27 - (reading - 0.706) / 0.001721
            sys.stdout.write(f"{temperature:.1f} C\n")
        elif raw == "cpu -f":
            freq_mhz = machine.freq() // 1000000
            sys.stdout.write(f"{freq_mhz} MHZ\n")
        elif raw.startswith("cpu -f "):
            try:
                target_mhz = int(raw.split()[2])
                machine.freq(target_mhz * 1000000)
                sys.stdout.write(f"FREQ_SET_{target_mhz}MHZ\n")
            except Exception:
                sys.stdout.write("INVALID_FREQ\n")
                
