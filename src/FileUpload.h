#pragma once

#include <windows.h>
#include <string>
#include <vector>
#include <fstream>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

class FileUpload {
public:
    enum class Event {
        None,
        Done,
        Disconnected
    };

    FileUpload()
        : uploading(false),
          stopRequested(false),
          pendingDrop(false),
          pendingEvent(Event::None),
          staged(false),
          fileSize(0) {}

    ~FileUpload() {
        Stop();
    }

    void Stop() {
        stopRequested.store(true);
        if (worker.joinable())
            worker.join();
        stopRequested.store(false);
        uploading.store(false);
    }

    void Reset() {
        Stop();
        std::lock_guard<std::mutex> lock(mtx);
        pendingDrop.store(false);
        pendingEvent.store(Event::None);
        staged = false;
        localPath.clear();
        fileName.clear();
        upperCommand.clear();
        fileSize = 0;
    }

    bool IsUploading() const {
        return uploading.load();
    }

    bool HasPendingDrop() const {
        return pendingDrop.load();
    }

    bool MatchesCommand(const std::string& line) const {
        std::lock_guard<std::mutex> lock(mtx);
        return pendingDrop.load() && !upperCommand.empty() && line == upperCommand;
    }

    Event PollEvent() {
        return pendingEvent.exchange(Event::None);
    }

    bool StageFile(const char* path, std::string& outCommandLine) {
        if (!path || IsUploading())
            return false;

        DWORD attrs = GetFileAttributesA(path);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return false;

        std::string full(path);
        size_t slash = full.find_last_of("\\/");
        std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);
        if (name.empty())
            return false;

        std::ifstream f(full, std::ios::binary | std::ios::ate);
        if (!f.is_open())
            return false;
        std::streamoff sz = f.tellg();
        if (sz < 0)
            return false;
        uint64_t sizeVal = static_cast<uint64_t>(sz);
        f.close();

        std::string upperName = name;
        for (char& c : upperName) {
            if (c >= 'a' && c <= 'z')
                c = static_cast<char>(c - 32);
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            localPath = full;
            fileName = name;
            upperCommand = "UPLOAD " + upperName;
            fileSize = sizeVal;
            staged = true;
            pendingDrop.store(true);
            outCommandLine = upperCommand;
        }

        return true;
    }

    bool ConsumeDropCommand(std::string& outDropCmd) {
        std::lock_guard<std::mutex> lock(mtx);
        if (!staged || !pendingDrop.load())
            return false;
        pendingDrop.store(false);
        outDropCmd = "__drop__ " + std::to_string(fileSize) + " " + fileName + "\n";
        return true;
    }

    bool StartUpload(HANDLE hSerial) {
        if (hSerial == INVALID_HANDLE_VALUE || IsUploading())
            return false;

        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!staged)
                return false;
        }

        if (worker.joinable())
            worker.join();

        stopRequested.store(false);
        pendingEvent.store(Event::None);
        uploading.store(true);
        worker = std::thread(&FileUpload::RunUpload, this, hSerial);
        return true;
    }

private:
    std::atomic<bool> uploading;
    std::atomic<bool> stopRequested;
    std::atomic<bool> pendingDrop;
    std::atomic<Event> pendingEvent;

    mutable std::mutex mtx;
    std::thread worker;

    bool staged;
    std::string localPath;
    std::string fileName;
    std::string upperCommand;
    uint64_t fileSize;

    static void SetBlockingTimeouts(HANDLE hSerial, bool blocking) {
        COMMTIMEOUTS timeouts = { 0 };
        if (blocking) {
            timeouts.ReadIntervalTimeout = 10;
            timeouts.ReadTotalTimeoutConstant = 40;
            timeouts.ReadTotalTimeoutMultiplier = 0;
            timeouts.WriteTotalTimeoutConstant = 500;
            timeouts.WriteTotalTimeoutMultiplier = 0;
        } else {
            timeouts.ReadIntervalTimeout = MAXDWORD;
        }
        SetCommTimeouts(hSerial, &timeouts);
    }

    bool WriteAll(HANDLE hSerial, const std::string& data, bool& disconnected) {
        DWORD totalWritten = 0;
        DWORD totalSize = static_cast<DWORD>(data.size());
        while (totalWritten < totalSize && !stopRequested.load()) {
            DWORD errors = 0;
            COMSTAT stat;
            if (!ClearCommError(hSerial, &errors, &stat)) {
                disconnected = true;
                return false;
            }
            DWORD written = 0;
            if (!WriteFile(hSerial, data.data() + totalWritten, totalSize - totalWritten, &written, NULL)) {
                disconnected = true;
                return false;
            }
            if (written == 0)
                return false;
            totalWritten += written;
        }
        return totalWritten == totalSize;
    }

    bool ReadLine(HANDLE hSerial, ULONGLONG maxWaitMs, std::string& outLine, bool& disconnected) {
        outLine.clear();
        ULONGLONG start = GetTickCount64();
        char ch = 0;

        while (!stopRequested.load() && (GetTickCount64() - start < maxWaitMs)) {
            DWORD errors = 0;
            COMSTAT stat;
            if (!ClearCommError(hSerial, &errors, &stat)) {
                disconnected = true;
                return false;
            }

            DWORD bytesRead = 0;
            if (!ReadFile(hSerial, &ch, 1, &bytesRead, NULL)) {
                disconnected = true;
                return false;
            }

            if (bytesRead > 0) {
                if (ch == '\n') {
                    if (!outLine.empty() && outLine.back() == '\r')
                        outLine.pop_back();
                    return true;
                }
                outLine.push_back(ch);
            }
        }
        return false;
    }

    void RunUpload(HANDLE hSerial) {
        std::string pathCopy;
        {
            std::lock_guard<std::mutex> lock(mtx);
            pathCopy = localPath;
            staged = false;
        }

        bool disconnected = false;
        SetBlockingTimeouts(hSerial, true);
        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

        std::ifstream file(pathCopy, std::ios::binary);
        if (!file.is_open()) {
            WriteAll(hSerial, "PUT_ABORT\n", disconnected);
            SetBlockingTimeouts(hSerial, false);
            uploading.store(false);
            if (!stopRequested.load())
                pendingEvent.store(disconnected ? Event::Disconnected : Event::Done);
            return;
        }

        std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        if (WriteAll(hSerial, "PUT_GO\n", disconnected)) {
            std::string resp;
            if (ReadLine(hSerial, 2500, resp, disconnected) && resp == "__put_ready__") {
                bool transferOk = true;
                const size_t chunkSize = 64;
                char hexPair[3];

                for (size_t offset = 0; offset < data.size() && !stopRequested.load(); offset += chunkSize) {
                    size_t end = offset + chunkSize;
                    if (end > data.size())
                        end = data.size();

                    std::string line;
                    line.reserve((end - offset) * 2 + 1);
                    for (size_t i = offset; i < end; i++) {
                        sprintf(hexPair, "%02x", data[i]);
                        line += hexPair;
                    }
                    line.push_back('\n');

                    if (!WriteAll(hSerial, line, disconnected)) {
                        transferOk = false;
                        break;
                    }

                    std::string ack;
                    if (!ReadLine(hSerial, 2500, ack, disconnected) || ack != "__ack__") {
                        transferOk = false;
                        break;
                    }
                }

                if (transferOk && !stopRequested.load() && !disconnected) {
                    std::string finalResp;
                    ReadLine(hSerial, 3000, finalResp, disconnected);
                }
            }
        }

        SetBlockingTimeouts(hSerial, false);
        uploading.store(false);

        if (stopRequested.load())
            return;

        pendingEvent.store(disconnected ? Event::Disconnected : Event::Done);
    }
};