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

def get_short_path():
    cwd = os.getcwd().upper()
    if cwd == "/":
        return "ROOT:/"
    parts = [p for p in cwd.split("/") if p]
    if len(parts) <= 2:
        return "ROOT:/" + "/".join(parts)
    return "ROOT:/.../" + parts[-2] + "/" + parts[-1]

while True:
    events = poller.poll(50)
    if events:
        line = sys.stdin.readline().strip()
        raw = line.lower()

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

        elif raw.startswith("cd "):
            parts = raw.split(None, 1)
            if len(parts) < 2 or not parts[1].strip():
                sys.stdout.write("MISSING_PATH\n")
            else:
                path = parts[1].strip()
                try:
                    os.chdir(path)
                    sys.stdout.write("DIR_CHANGED\n")
                except Exception:
                    sys.stdout.write("INVALID_PATH\n")

        elif raw == "ls":
            sys.stdout.write(get_short_path() + "\n")
            entries = list(os.ilistdir())
            if not entries:
                sys.stdout.write("EMPTY\n")
            else:
                for entry in entries:
                    full_name = entry[0].upper()
                    if entry[1] == 0x4000:
                        tag = "[DIR]"
                        name = full_name
                    else:
                        if "." in full_name and not full_name.startswith("."):
                            name, ext = full_name.rsplit(".", 1)
                            tag = f"[{ext[:4]}]"
                        else:
                            name = full_name
                            tag = "[FILE]"

                    if len(name) > 20:
                        name = name[:17] + "..."

                    tag_padded = tag + " " * (7 - len(tag))

                    if entry[1] == 0x4000:
                        sys.stdout.write(f"{tag_padded}{name}/\n")
                    else:
                        sys.stdout.write(f"{tag_padded}{name} ({entry[3]}B)\n")

        elif raw.startswith("mk -f ") or raw.startswith("mk -d "):
            parts = raw.split(None, 2)
            if len(parts) < 3 or not parts[2].strip():
                sys.stdout.write("MISSING_NAME\n")
            else:
                name = parts[2].strip()
                if len(name) > 20:
                    sys.stdout.write("NAME_TOO_LONG\n")
                else:
                    try:
                        if parts[1] == "-f":
                            open(name, "w").close()
                            sys.stdout.write("FILE_CREATED\n")
                        else:
                            os.mkdir(name)
                            sys.stdout.write("DIR_CREATED\n")
                    except Exception:
                        sys.stdout.write("MK_FAILED\n")

        elif raw.startswith("rm -f ") or raw.startswith("rm -d "):
            parts = raw.split(None, 2)
            if len(parts) < 3 or not parts[2].strip():
                sys.stdout.write("MISSING_NAME\n")
            else:
                name = parts[2].strip()
                try:
                    if parts[1] == "-f":
                        os.remove(name)
                        sys.stdout.write("FILE_REMOVED\n")
                    else:
                        os.rmdir(name)
                        sys.stdout.write("DIR_REMOVED\n")
                except Exception:
                    sys.stdout.write("RM_FAILED\n")

        elif raw.startswith("write "):
            parts = raw.split(None, 2)
            if len(parts) < 3 or not parts[1].strip():
                sys.stdout.write("INVALID_ARGS\n")
            else:
                name = parts[1].strip()
                text = parts[2].replace("\\n", "\n").replace("\\t", "\t")
                try:
                    with open(name, "w") as f:
                        f.write(text)
                    sys.stdout.write("FILE_WRITTEN\n")
                except Exception:
                    sys.stdout.write("WRITE_FAILED\n")

        elif raw.startswith("__stat__ "):
            fname = line[9:].strip()
            if not fname:
                sys.stdout.write("STAT_ERR\n")
            else:
                try:
                    st = os.statvfs("/")
                    fb = st[0] * st[3]
                    try:
                        es = os.stat(fname)[6]
                        ex = 1
                    except Exception:
                        es = 0
                        ex = 0
                    sys.stdout.write(f"STAT:{fb}:{ex}:{es}\n")
                except Exception:
                    sys.stdout.write("STAT_ERR\n")

        elif raw.startswith("__put__ "):
            parts = line.split(None, 2)
            if len(parts) < 3 or not parts[2].strip():
                sys.stdout.write("PUT_ERR\n")
            else:
                fname = parts[2].strip()
                f = None
                try:
                    total = int(parts[1])
                    f = open(fname, "wb")
                    sys.stdout.write("PUT_READY\n")
                    written = 0
                    ok = True
                    while written < total:
                        if not poller.poll(3000):
                            ok = False
                            break
                        chunk_line = sys.stdin.readline()
                        if not chunk_line:
                            ok = False
                            break
                        hex_str = chunk_line.strip()
                        if not hex_str:
                            continue
                        raw_bytes = bytes.fromhex(hex_str)
                        f.write(raw_bytes)
                        written += len(raw_bytes)
                        sys.stdout.write("ACK\n")
                    f.close()
                    f = None
                    if ok and written == total:
                        sys.stdout.write("PUT_OK\n")
                    else:
                        try:
                            os.remove(fname)
                        except Exception:
                            pass
                        sys.stdout.write("PUT_ERR\n")
                except Exception:
                    if f is not None:
                        try:
                            f.close()
                        except Exception:
                            pass
                    try:
                        os.remove(fname)
                    except Exception:
                        pass
                    sys.stdout.write("PUT_ERR\n")