#pragma once

#include <string>
#include <cstddef>

class CmdHistory {
public:
    static const size_t CAPACITY = 32;

    CmdHistory() : head(0), count(0), pos(0), browsing(false) {
        for (size_t i = 0; i < CAPACITY; i++)
            hasEdit[i] = false;
    }

    void Push(const std::string& line) {
        ResetBrowse();
        if (line.empty())
            return;
        if (count > 0 && At(count - 1) == line)
            return;

        if (count < CAPACITY) {
            items[(head + count) % CAPACITY] = line;
            count++;
        } else {
            items[head] = line;
            head = (head + 1) % CAPACITY;
        }
    }

    bool Prev(const std::string& current, std::string& out) {
        if (count == 0)
            return false;

        if (!browsing) {
            browsing = true;
            pos = count;
        }

        SaveCurrent(current);

        if (pos == 0)
            return false;

        pos--;
        out = View(pos);
        return true;
    }

    bool Next(const std::string& current, std::string& out) {
        if (!browsing || pos >= count)
            return false;

        SaveCurrent(current);

        pos++;
        out = (pos == count) ? draft : View(pos);
        return true;
    }

    void ResetBrowse() {
        browsing = false;
        pos = 0;
        draft.clear();
        for (size_t i = 0; i < CAPACITY; i++) {
            hasEdit[i] = false;
            edited[i].clear();
        }
    }

    bool IsBrowsing() const {
        return browsing;
    }

    size_t Size() const {
        return count;
    }

private:
    std::string items[CAPACITY];
    std::string edited[CAPACITY];
    bool hasEdit[CAPACITY];
    size_t head;
    size_t count;
    size_t pos;
    bool browsing;
    std::string draft;

    const std::string& At(size_t i) const {
        return items[(head + i) % CAPACITY];
    }

    const std::string& View(size_t i) const {
        return hasEdit[i] ? edited[i] : At(i);
    }

    void SaveCurrent(const std::string& current) {
        if (pos == count) {
            draft = current;
        } else if (current != At(pos)) {
            edited[pos] = current;
            hasEdit[pos] = true;
        } else {
            hasEdit[pos] = false;
            edited[pos].clear();
        }
    }
};
