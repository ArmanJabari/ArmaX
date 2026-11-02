import machine
import sys
import select

led = machine.Pin("LED", machine.Pin.OUT)

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
