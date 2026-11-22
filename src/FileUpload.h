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
    enum class State {
        Idle,
        Querying,
        Ready,
        ConfirmOverwrite,
        Uploading
    };

    enum class Event {
        None,
        FileTooLarge,
        AskOverwrite,
        UploadSuccess,
        UploadFailed,
        Disconnected
    };

    FileUpload()
        : state(State::Idle),
          pendingEvent(Event::None),
          autoSubmit(false),
          stopRequested(false),
          fileSize(0),
          queryOk(false),
          freeBytes(0),
          fileExists(false),
          existingSize(0) {}

    ~FileUpload() {
        Stop();
    }

    void Stop() {
        stopRequested.store(true);
        if (worker.joinable())
            worker.join();
        stopRequested.store(false);
    }

    void Reset() {
        Stop();
        std::lock_guard<std::mutex> lock(mtx);
        state.store(State::Idle);
        pendingEvent.store(Event::None);
        autoSubmit.store(false);
        filePath.clear();
        originalName.clear();
        upperCommand.clear();
        fileSize = 0;
        queryOk = false;
        freeBytes = 0;
        fileExists = false;
        existingSize = 0;
    }

    bool IsBusy() const {
        State s = state.load();
        return s == State::Querying || s == State::Uploading;
    }

    bool IsUploading() const {
        return state.load() == State::Uploading;
    }

    bool IsConfirmingOverwrite() const {
        return state.load() == State::ConfirmOverwrite;
    }

    bool HasPendingDrop() const {
        State s = state.load();
        return s == State::Querying || s == State::Ready || s == State::ConfirmOverwrite;
    }

    bool MatchesCommand(const std::string& line) const {
        std::lock_guard<std::mutex> lock(mtx);
        return !upperCommand.empty() && line == upperCommand;
    }

    bool OnFileDropped(const char* droppedPath, HANDLE hSerial, std::string& outCommandLine) {
        if (!droppedPath || hSerial == INVALID_HANDLE_VALUE || IsUploading())
            return false;

        DWORD attrs = GetFileAttributesA(droppedPath);
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY))
            return false;

        std::string full(droppedPath);
        size_t slash = full.find_last_of("\\/");
        std::string name = (slash == std::string::npos) ? full : full.substr(slash + 1);
        if (name.empty())
            return false;

        if (_stricmp(name.c_str(), "main.py") == 0)
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

        Stop();

        {
            std::lock_guard<std::mutex> lock(mtx);
            filePath = full;
            originalName = name;
            upperCommand = "UPLOAD " + upperName;
            fileSize = sizeVal;
            queryOk = false;
            freeBytes = 0;
            fileExists = false;
            existingSize = 0;
            pendingEvent.store(Event::None);
            autoSubmit.store(false);
            state.store(State::Querying);
            outCommandLine = upperCommand;
        }

        worker = std::thread(&FileUpload::RunQuery, this, hSerial);
        return true;
    }

    void OnEnterPressed(HANDLE hSerial) {
        State s = state.load();
        if (s == State::Querying) {
            autoSubmit.store(true);
            return;
        }
        if (s == State::Ready) {
            EvaluateQueryResult(hSerial);
        }
    }

    void OnConfirmYes(HANDLE hSerial) {
        if (state.load() != State::ConfirmOverwrite)
            return;
        StartUpload(hSerial);
    }

    void OnConfirmNo() {
        if (state.load() != State::ConfirmOverwrite)
            return;
        state.store(State::Ready);
    }

    Event PollEvent() {
        return pendingEvent.exchange(Event::None);
    }

private:
    std::atomic<State> state;
    std::atomic<Event> pendingEvent;
    std::atomic<bool> autoSubmit;
    std::atomic<bool> stopRequested;

    mutable std::mutex mtx;
    std::thread worker;

    std::string filePath;
    std::string originalName;
    std::string upperCommand;
    uint64_t fileSize;

    bool queryOk;
    uint64_t freeBytes;
    bool fileExists;
    uint64_t existingSize;

    void EvaluateQueryResult(HANDLE hSerial) {
        bool ok = false;
        uint64_t fSize = 0;
        uint64_t freeB = 0;
        bool exists = false;
        uint64_t exSize = 0;

        {
            std::lock_guard<std::mutex> lock(mtx);
            ok = queryOk;
            fSize = fileSize;
            freeB = freeBytes;
            exists = fileExists;
            exSize = existingSize;
        }

        if (!ok) {
            state.store(State::Idle);
            pendingEvent.store(Event::UploadFailed);
            return;
        }

        if (fSize > freeB + exSize) {
            state.store(State::Idle);
            pendingEvent.store(Event::FileTooLarge);
            return;
        }

        if (exists) {
            state.store(State::ConfirmOverwrite);
            pendingEvent.store(Event::AskOverwrite);
            return;
        }

        StartUpload(hSerial);
    }

    void StartUpload(HANDLE hSerial) {
        if (worker.joinable())
            worker.join();
        state.store(State::Uploading);
        worker = std::thread(&FileUpload::RunUpload, this, hSerial);
    }

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

    void RunQuery(HANDLE hSerial) {
        std::string targetName;
        {
            std::lock_guard<std::mutex> lock(mtx);
            targetName = originalName;
        }

        bool disconnected = false;
        bool ok = false;
        uint64_t freeB = 0;
        bool exists = false;
        uint64_t exSize = 0;

        SetBlockingTimeouts(hSerial, true);
        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

        std::string cmd = "__stat__ " + targetName + "\n";
        if (WriteAll(hSerial, cmd, disconnected)) {
            std::string resp;
            if (ReadLine(hSerial, 1500, resp, disconnected)) {
                if (resp.compare(0, 5, "STAT:") == 0) {
                    unsigned long long parsedFree = 0;
                    int parsedEx = 0;
                    unsigned long long parsedExSize = 0;
                    if (sscanf(resp.c_str(), "STAT:%llu:%d:%llu", &parsedFree, &parsedEx, &parsedExSize) == 3) {
                        freeB = static_cast<uint64_t>(parsedFree);
                        exists = (parsedEx == 1);
                        exSize = static_cast<uint64_t>(parsedExSize);
                        ok = true;
                    }
                }
            }
        }

        SetBlockingTimeouts(hSerial, false);

        if (stopRequested.load())
            return;

        if (disconnected) {
            state.store(State::Idle);
            pendingEvent.store(Event::Disconnected);
            return;
        }

        {
            std::lock_guard<std::mutex> lock(mtx);
            queryOk = ok;
            freeBytes = freeB;
            fileExists = exists;
            existingSize = exSize;
        }

        state.store(State::Ready);

        if (autoSubmit.exchange(false))
            EvaluateQueryResult(hSerial);
    }

    void RunUpload(HANDLE hSerial) {
        std::string localPath;
        std::string targetName;
        {
            std::lock_guard<std::mutex> lock(mtx);
            localPath = filePath;
            targetName = originalName;
        }

        std::ifstream file(localPath, std::ios::binary);
        if (!file.is_open()) {
            state.store(State::Idle);
            pendingEvent.store(Event::UploadFailed);
            return;
        }
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();

        bool disconnected = false;
        bool success = false;

        SetBlockingTimeouts(hSerial, true);
        PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

        std::string header = "__put__ " + std::to_string(data.size()) + " " + targetName + "\n";
        if (WriteAll(hSerial, header, disconnected)) {
            std::string resp;
            if (ReadLine(hSerial, 2000, resp, disconnected) && resp == "PUT_READY") {
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
                    if (!ReadLine(hSerial, 2000, ack, disconnected) || ack != "ACK") {
                        transferOk = false;
                        break;
                    }
                }

                if (transferOk && !stopRequested.load() && !disconnected) {
                    std::string finalResp;
                    if (ReadLine(hSerial, 2500, finalResp, disconnected) && finalResp == "PUT_OK") {
                        success = true;
                    }
                }
            }
        }

        SetBlockingTimeouts(hSerial, false);

        if (stopRequested.load())
            return;

        state.store(State::Idle);

        if (disconnected) {
            pendingEvent.store(Event::Disconnected);
        } else if (success) {
            pendingEvent.store(Event::UploadSuccess);
        } else {
            pendingEvent.store(Event::UploadFailed);
        }
    }
};