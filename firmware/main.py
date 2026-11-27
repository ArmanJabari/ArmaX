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

PROTECTED = ("main.py",)

def get_short_path():
    cwd = os.getcwd().upper()
    if cwd == "/":
        return "ROOT:/"
    parts = [p for p in cwd.split("/") if p]
    if len(parts) <= 2:
        return "ROOT:/" + "/".join(parts)
    return "ROOT:/.../" + parts[-2] + "/" + parts[-1]

def is_protected(name):
    return name.rstrip("/").split("/")[-1].lower() in PROTECTED

def drain_stdin():
    while poller.poll(0):
        sys.stdin.read(1)

def format_entry(entry):
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
        return f"{tag_padded}{name}/\n"
    return f"{tag_padded}{name} ({entry[3]}B)\n"

def render_ls_page(entries, page, total_pages):
    sys.stdout.write("__clear__\n")
    sys.stdout.write(get_short_path() + "\n")
    start = page * 17
    chunk = entries[start:start + 17]
    for entry in chunk:
        sys.stdout.write(format_entry(entry))
    for _ in range(17 - len(chunk)):
        sys.stdout.write(" \n")
    sys.stdout.write(f"[{page + 1:02d}|{total_pages:02d}] (B)ACK (N)EXT (Q)UIT\n")

def confirm_prompt(msg):
    drain_stdin()
    sys.stdout.write(msg + "\n")
    sys.stdout.write("__key_start__\n")
    result = False
    try:
        while True:
            ch = sys.stdin.read(1)
            if not ch or ch == "\x03":
                break
            c = ch.lower()
            if c == "y":
                result = True
                break
            elif c == "n":
                break
    except KeyboardInterrupt:
        result = False
    finally:
        sys.stdout.write("__key_end__\n")
    return result

def receive_file(fname, total):
    drain_stdin()
    sys.stdout.write("__send_file__\n")
    if not poller.poll(3000):
        sys.stdout.write("UPLOAD FAILED\n")
        return
    go = sys.stdin.readline().strip()
    if go != "PUT_GO":
        sys.stdout.write("UPLOAD FAILED\n")
        return
    f = None
    try:
        f = open(fname, "wb")
        sys.stdout.write("__put_ready__\n")
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
            sys.stdout.write("__ack__\n")
        f.close()
        f = None
        if ok and written == total:
            sys.stdout.write("__put_ok__\n")
            sys.stdout.write("UPLOAD WAS SUCCESSFUL\n")
        else:
            try:
                os.remove(fname)
            except Exception:
                pass
            sys.stdout.write("__put_err__\n")
            sys.stdout.write("UPLOAD FAILED\n")
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
        sys.stdout.write("__put_err__\n")
        sys.stdout.write("UPLOAD FAILED\n")

while True:
    events = poller.poll(50)
    if events:
        line = sys.stdin.readline().strip()
        raw = line.lower()

        if raw == "identify":
            sys.stdout.write("PICO_W_OK\n")

        elif raw == "clear":
            sys.stdout.write("__clear__\n")

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
                if 48 <= target_mhz <= 250:
                    machine.freq(target_mhz * 1000000)
                    sys.stdout.write(f"FREQ_SET_{target_mhz}MHZ\n")
                else:
                    sys.stdout.write("INVALID_FREQ\n")
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

        elif raw == "uptime":
            total = time.ticks_diff(time.ticks_ms(), boot) // 1000
            sys.stdout.write(f"{total // 3600:02d}:{(total // 60) % 60:02d}:{total % 60:02d}\n")

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
            entries = [e for e in os.ilistdir() if not is_protected(e[0])]
            entries.sort(key=lambda e: (0 if e[1] == 0x4000 else 1, e[0].lower()))
            if not entries:
                sys.stdout.write(get_short_path() + "\n")
                sys.stdout.write("EMPTY\n")
            elif len(entries) <= 17:
                sys.stdout.write(get_short_path() + "\n")
                for entry in entries:
                    sys.stdout.write(format_entry(entry))
            else:
                total_pages = (len(entries) + 16) // 17
                page = 0
                drain_stdin()
                sys.stdout.write("__run_start__\n__key_start__\n")
                render_ls_page(entries, page, total_pages)
                try:
                    while True:
                        ch = sys.stdin.read(1)
                        if not ch or ch == "\x03":
                            break
                        cmd = ch.lower()
                        if cmd == "q":
                            sys.stdout.write("__clear__\n")
                            break
                        elif cmd == "n":
                            if page + 1 < total_pages:
                                page += 1
                                render_ls_page(entries, page, total_pages)
                        elif cmd == "b":
                            if page > 0:
                                page -= 1
                                render_ls_page(entries, page, total_pages)
                except KeyboardInterrupt:
                    pass
                sys.stdout.write("__key_end__\n__run_end__\n")

        elif raw.startswith("mk -f ") or raw.startswith("mk -d "):
            parts = raw.split()
            if len(parts) < 3:
                sys.stdout.write("MISSING_NAME\n")
            else:
                mode = parts[1]
                for name in parts[2:]:
                    if len(name) > 20 or is_protected(name):
                        sys.stdout.write(f"MK_FAILED: {name.upper()}\n")
                    else:
                        try:
                            if mode == "-f":
                                open(name, "w").close()
                            else:
                                os.mkdir(name)
                        except Exception:
                            sys.stdout.write(f"MK_FAILED: {name.upper()}\n")

        elif raw.startswith("rm -f ") or raw.startswith("rm -d "):
            parts = raw.split()
            if len(parts) < 3:
                sys.stdout.write("MISSING_NAME\n")
            else:
                mode = parts[1]
                for name in parts[2:]:
                    if is_protected(name):
                        sys.stdout.write(f"RM_FAILED: {name.upper()}\n")
                        continue
                    try:
                        if mode == "-f":
                            os.remove(name)
                        else:
                            os.rmdir(name)
                    except Exception:
                        sys.stdout.write(f"RM_FAILED: {name.upper()}\n")

        elif raw.startswith("write "):
            parts = raw.split(None, 2)
            if len(parts) < 3 or not parts[1].strip():
                sys.stdout.write("INVALID_ARGS\n")
            else:
                name = parts[1].strip()
                text = parts[2].replace("\\n", "\n").replace("\\t", "\t")
                if is_protected(name):
                    sys.stdout.write("WRITE_FAILED\n")
                else:
                    try:
                        with open(name, "w") as f:
                            f.write(text)
                        sys.stdout.write("FILE_WRITTEN\n")
                    except Exception:
                        sys.stdout.write("WRITE_FAILED\n")

        elif raw == "run" or raw.startswith("run "):
            parts = line.split(None, 1)
            if len(parts) < 2 or not parts[1].strip():
                sys.stdout.write("MISSING_NAME\n")
            else:
                req_name = parts[1].strip()
                fname = None
                for entry in os.ilistdir():
                    if not (entry[1] & 0x4000) and entry[0].lower() == req_name.lower():
                        fname = entry[0]
                        break
                if fname is None:
                    sys.stdout.write("FILE_NOT_FOUND\n")
                else:
                    try:
                        with open(fname, "r") as f:
                            code = f.read()
                        drain_stdin()
                        gc.collect()
                        sys.stdout.write("__run_start__\n")
                        def _run_input(prompt=""):
                            if prompt:
                                sys.stdout.write(str(prompt))
                            s = sys.stdin.readline()
                            if not s or "\x03" in s:
                                raise KeyboardInterrupt
                            return s.rstrip("\r\n")
                        def _run_clear():
                            sys.stdout.write("\n__clear__\n")
                        def _run_getch():
                            drain_stdin()
                            sys.stdout.write("\n__key_start__\n")
                            try:
                                ch = sys.stdin.read(1)
                                if not ch or ch == "\x03":
                                    raise KeyboardInterrupt
                                return ch
                            finally:
                                sys.stdout.write("__key_end__\n")
                        env = {
                            "__name__": "__main__",
                            "input": _run_input,
                            "clear": _run_clear,
                            "getch": _run_getch
                        }
                        try:
                            exec(code, env)
                        except KeyboardInterrupt:
                            sys.stdout.write("\nSTOPPED\n")
                        except Exception as e:
                            sys.stdout.write(f"\nERR: {e}\n")
                        sys.stdout.write("\n__run_end__\n")
                        gc.collect()
                    except Exception:
                        sys.stdout.write("RUN_FAILED\n")

        elif raw.startswith("__drop__ "):
            parts = line.split(None, 2)
            if len(parts) < 3 or not parts[2].strip():
                sys.stdout.write("UPLOAD FAILED\n")
            else:
                try:
                    total = int(parts[1])
                    fname = parts[2].strip()
                except Exception:
                    sys.stdout.write("UPLOAD FAILED\n")
                    continue
                if is_protected(fname):
                    sys.stdout.write("PROTECTED FILE\n")
                else:
                    try:
                        st = os.statvfs("/")
                        free_bytes = st[0] * st[3]
                        try:
                            existing_size = os.stat(fname)[6]
                            exists = True
                        except Exception:
                            existing_size = 0
                            exists = False
                        if total > free_bytes + existing_size:
                            sys.stdout.write("FILE IS HUGE\n")
                        elif exists:
                            if confirm_prompt("FILE EXISTS. REPLACE? Y/N"):
                                receive_file(fname, total)
                            else:
                                sys.stdout.write("UPLOAD CANCELED\n")
                        else:
                            receive_file(fname, total)
                    except Exception:
                        sys.stdout.write("UPLOAD FAILED\n")

        elif raw == "sys -u":
            try:
                os.remove("/main.py")
            except Exception:
                sys.stdout.write("UPDATE_FAILED\n")
            else:
                sys.stdout.write("UPDATING\n")
                time.sleep_ms(50)
                machine.reset()