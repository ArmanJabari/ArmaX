#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <atomic>
#include <cstdio>
#include <cstring>

enum class FwStatus {
    Finding,
    Installing,
    NoFirmware,
    Connected
};

class FwInstaller {
public:
    static HANDLE ScanOrInstall(std::string& foundPortName, std::atomic<FwStatus>& status) {
        std::vector<std::string> ports = GetActivePicoPorts();
        if (ports.empty()) {
            status.store(FwStatus::Finding);
            return INVALID_HANDLE_VALUE;
        }

        for (const auto& port : ports) {
            std::string portPath = "\\\\.\\" + port;
            HANDLE hSerial = CreateFileA(portPath.c_str(), GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            if (hSerial == INVALID_HANDLE_VALUE)
                continue;

            if (!ConfigurePort(hSerial, false)) {
                CloseHandle(hSerial);
                continue;
            }

            if (TryIdentify(hSerial)) {
                ConfigurePort(hSerial, true);
                foundPortName = port;
                status.store(FwStatus::Connected);
                return hSerial;
            }

            std::vector<unsigned char> fwData;
            if (!LoadFirmwareFile(fwData)) {
                status.store(FwStatus::NoFirmware);
                CloseHandle(hSerial);
                return INVALID_HANDLE_VALUE;
            }

            status.store(FwStatus::Installing);

            if (UploadMainPy(hSerial, fwData)) {
                for (int attempt = 0; attempt < 12; attempt++) {
                    if (TryIdentify(hSerial)) {
                        ConfigurePort(hSerial, true);
                        foundPortName = port;
                        status.store(FwStatus::Connected);
                        return hSerial;
                    }
                    Sleep(150);
                }
            }

            CloseHandle(hSerial);
        }

        if (status.load() != FwStatus::NoFirmware)
            status.store(FwStatus::Finding);

        return INVALID_HANDLE_VALUE;
    }

private:
    static bool ConfigurePort(HANDLE hSerial, bool nonBlocking) {
        DCB dcbSerialParams = { 0 };
        dcbSerialParams.DCBlength = sizeof(dcbSerialParams);
        if (!GetCommState(hSerial, &dcbSerialParams))
            return false;

        dcbSerialParams.BaudRate = CBR_115200;
        dcbSerialParams.ByteSize = 8;
        dcbSerialParams.StopBits = ONESTOPBIT;
        dcbSerialParams.Parity = NOPARITY;
        dcbSerialParams.fDtrControl = DTR_CONTROL_ENABLE;
        dcbSerialParams.fRtsControl = RTS_CONTROL_ENABLE;

        if (!SetCommState(hSerial, &dcbSerialParams))
            return false;

        COMMTIMEOUTS timeouts = { 0 };
        if (nonBlocking) {
            timeouts.ReadIntervalTimeout = MAXDWORD;
        } else {
            timeouts.ReadIntervalTimeout = 10;
            timeouts.ReadTotalTimeoutConstant = 40;
            timeouts.ReadTotalTimeoutMultiplier = 0;
            timeouts.WriteTotalTimeoutConstant = 100;
            timeouts.WriteTotalTimeoutMultiplier = 0;
        }
        SetCommTimeouts(hSerial, &timeouts);
        return true;
    }

    static std::vector<std::string> GetActivePicoPorts() {
        std::vector<std::string> activePorts;
        HKEY hSerialComm = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "HARDWARE\\DEVICEMAP\\SERIALCOMM", 0, KEY_READ, &hSerialComm) == ERROR_SUCCESS) {
            char valueName[256];
            BYTE valueData[256];
            DWORD index = 0;
            while (true) {
                DWORD nameLen = sizeof(valueName);
                DWORD dataLen = sizeof(valueData);
                DWORD type = 0;
                LONG res = RegEnumValueA(hSerialComm, index++, valueName, &nameLen, NULL, &type, valueData, &dataLen);
                if (res != ERROR_SUCCESS)
                    break;
                if (type == REG_SZ && dataLen > 1) {
                    activePorts.emplace_back(reinterpret_cast<char*>(valueData));
                }
            }
            RegCloseKey(hSerialComm);
        }

        std::vector<std::string> picoPorts;
        if (activePorts.empty())
            return picoPorts;

        HKEY hUsb = NULL;
        if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SYSTEM\\CurrentControlSet\\Enum\\USB", 0, KEY_READ, &hUsb) == ERROR_SUCCESS) {
            char vidPidName[256];
            DWORD usbIndex = 0;
            while (true) {
                DWORD vidPidLen = sizeof(vidPidName);
                if (RegEnumKeyExA(hUsb, usbIndex++, vidPidName, &vidPidLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                    break;

                if (_strnicmp(vidPidName, "VID_2E8A", 8) != 0)
                    continue;

                HKEY hDeviceKey = NULL;
                if (RegOpenKeyExA(hUsb, vidPidName, 0, KEY_READ, &hDeviceKey) != ERROR_SUCCESS)
                    continue;

                char instanceName[256];
                DWORD instIndex = 0;
                while (true) {
                    DWORD instLen = sizeof(instanceName);
                    if (RegEnumKeyExA(hDeviceKey, instIndex++, instanceName, &instLen, NULL, NULL, NULL, NULL) != ERROR_SUCCESS)
                        break;

                    std::string paramSubKey = std::string(instanceName) + "\\Device Parameters";
                    HKEY hParamKey = NULL;
                    if (RegOpenKeyExA(hDeviceKey, paramSubKey.c_str(), 0, KEY_READ, &hParamKey) == ERROR_SUCCESS) {
                        char portBuf[64] = { 0 };
                        DWORD portBufLen = sizeof(portBuf);
                        DWORD type = 0;
                        if (RegQueryValueExA(hParamKey, "PortName", NULL, &type, reinterpret_cast<BYTE*>(portBuf), &portBufLen) == ERROR_SUCCESS && type == REG_SZ) {
                            std::string portStr(portBuf);
                            for (const auto& active : activePorts) {
                                if (_stricmp(active.c_str(), portStr.c_str()) == 0) {
                                    bool exists = false;
                                    for (const auto& p : picoPorts) {
                                        if (_stricmp(p.c_str(), portStr.c_str()) == 0) {
                                            exists = true;
                                            break;
                                        }
                                    }
                                    if (!exists)
                                        picoPorts.push_back(portStr);
                                }
                            }
                        }
                        RegCloseKey(hParamKey);
                    }
                }
                RegCloseKey(hDeviceKey);
            }
            RegCloseKey(hUsb);
        }

        return picoPorts;
    }

    static bool LoadFirmwareFile(std::vector<unsigned char>& outData) {
        char fwPath[MAX_PATH];
        GetModuleFileNameA(NULL, fwPath, MAX_PATH);
        char* slash = strrchr(fwPath, '\\');
        if (!slash)
            return false;

        *(slash + 1) = '\0';
        strncat(fwPath, "firmware\\main.py", sizeof(fwPath) - strlen(fwPath) - 1);

        std::ifstream file(fwPath, std::ios::binary);
        if (!file.is_open())
            return false;

        outData.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
        return !outData.empty();
    }

    static bool ReadUntil(HANDLE hSerial, const std::string& target, ULONGLONG maxWaitMs, std::string* outAccum = nullptr) {
        std::string accum;
        ULONGLONG start = GetTickCount64();
        char buf[128];

        while (GetTickCount64() - start < maxWaitMs) {
            DWORD bytesRead = 0;
            if (ReadFile(hSerial, buf, sizeof(buf) - 1, &bytesRead, NULL) && bytesRead > 0) {
                accum.append(buf, bytesRead);
                if (accum.find(target) != std::string::npos) {
                    if (outAccum)
                        *outAccum = accum;
                    return true;
                }
            }
        }

        if (outAccum)
            *outAccum = accum;
        return false;
    }

    static bool TryIdentify(HANDLE hSerial) {
        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
        const char* ping = "IDENTIFY\r\n";
        DWORD written = 0;
        if (!WriteFile(hSerial, ping, (DWORD)strlen(ping), &written, NULL))
            return false;

        return ReadUntil(hSerial, "PICO_W_OK", 90);
    }

    static bool EnterRawRepl(HANDLE hSerial) {
        for (int attempt = 0; attempt < 2; attempt++) {
            PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
            DWORD written = 0;
            const char ctrlC[] = "\r\x03\x03";
            if (!WriteFile(hSerial, ctrlC, 3, &written, NULL))
                return false;

            Sleep(60);
            PurgeComm(hSerial, PURGE_RXCLEAR);

            const char ctrlA[] = "\r\x01";
            if (!WriteFile(hSerial, ctrlA, 2, &written, NULL))
                return false;

            if (ReadUntil(hSerial, "raw REPL; CTRL-B to exit", 500)) {
                ReadUntil(hSerial, ">", 200);
                PurgeComm(hSerial, PURGE_RXCLEAR);
                return true;
            }
        }
        return false;
    }

    static bool ExecRawCommand(HANDLE hSerial, const std::string& cmd) {
        std::string payload = cmd + "\x04";
        DWORD written = 0;
        if (!WriteFile(hSerial, payload.c_str(), (DWORD)payload.size(), &written, NULL))
            return false;

        std::string resp;
        if (!ReadUntil(hSerial, "\x04\x04>", 2000, &resp))
            return false;

        if (resp.find("OK") == std::string::npos)
            return false;

        size_t firstEot = resp.find('\x04');
        size_t secondEot = resp.find('\x04', firstEot + 1);
        if (firstEot == std::string::npos || secondEot == std::string::npos || secondEot != firstEot + 1)
            return false;

        return true;
    }

    static bool UploadMainPy(HANDLE hSerial, const std::vector<unsigned char>& data) {
        if (!EnterRawRepl(hSerial))
            return false;

        if (!ExecRawCommand(hSerial, "f = open('main.py', 'wb')"))
            return false;

        const size_t chunkSize = 128;
        char hexPair[3];
        for (size_t offset = 0; offset < data.size(); offset += chunkSize) {
            size_t end = offset + chunkSize;
            if (end > data.size())
                end = data.size();

            std::string cmd = "f.write(bytes.fromhex('";
            cmd.reserve(cmd.size() + (end - offset) * 2 + 4);
            for (size_t i = offset; i < end; i++) {
                sprintf(hexPair, "%02x", data[i]);
                cmd += hexPair;
            }
            cmd += "'))";

            if (!ExecRawCommand(hSerial, cmd)) {
                ExecRawCommand(hSerial, "f.close()");
                return false;
            }
        }

        if (!ExecRawCommand(hSerial, "f.flush(); f.close()"))
            return false;

        DWORD written = 0;
        const char exitRaw = '\x02';
        WriteFile(hSerial, &exitRaw, 1, &written, NULL);
        Sleep(40);

        const char softReset = '\x04';
        WriteFile(hSerial, &softReset, 1, &written, NULL);
        Sleep(250);

        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);
        return true;
    }
};