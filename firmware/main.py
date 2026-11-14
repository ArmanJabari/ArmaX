import machine
import sys
import select
import gc
import os
import time

boot = time.ticks_ms()

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
            sys.stdout.write(f"{machine.freq() // 1000000} MHZ\n")
        elif raw.startswith("cpu -f "):
            try:
                target_mhz = int(raw.split()[2])
                machine.freq(target_mhz * 1000000)
                sys.stdout.write(f"FREQ_SET_{target_mhz}MHZ\n")
            except Exception:
                sys.stdout.write("INVALID_FREQ\n")

        elif raw == "ram -f":
            sys.stdout.write(f"{gc.mem_free() // 1024} KB ({(gc.mem_free() * 100) // (gc.mem_free() + gc.mem_alloc())}%)\n")
        elif raw == "ram -a":
            sys.stdout.write(f"{gc.mem_alloc() // 1024} KB ({(gc.mem_alloc() * 100) // (gc.mem_free() + gc.mem_alloc())}%)\n")
        elif raw == "ram -t":
            sys.stdout.write(f"{(gc.mem_free() + gc.mem_alloc()) // 1024} KB\n")

        elif raw == "disk -f":
            stat = os.statvfs('/')
            sys.stdout.write(f"{(stat[0] * stat[3]) // 1024} KB ({(stat[3] * 100) // stat[2]}%)\n")
        elif raw == "disk -a":
            stat = os.statvfs('/')
            sys.stdout.write(f"{(stat[0] * (stat[2] - stat[3])) // 1024} KB ({((stat[2] - stat[3]) * 100) // stat[2]}%)\n")
        elif raw == "disk -t":
            stat = os.statvfs('/')
            sys.stdout.write(f"{(stat[0] * stat[2]) // 1024} KB\n")

        elif raw == "uptime -ms":
            sys.stdout.write(f"{time.ticks_diff(time.ticks_ms(), boot)}\n")
        elif raw == "uptime -s":
            sys.stdout.write(f"{time.ticks_diff(time.ticks_ms(), boot) // 1000}\n")
        elif raw == "uptime -m":
            sys.stdout.write(f"{time.ticks_diff(time.ticks_ms(), boot) // 60000}\n")
        elif raw == "uptime -h":
            sys.stdout.write(f"{time.ticks_diff(time.ticks_ms(), boot) // 3600000}\n")
