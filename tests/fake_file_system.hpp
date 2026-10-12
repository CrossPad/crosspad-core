// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "crosspad/platform/IFileSystem.hpp"
#include "crosspad/platform/PlatformServices.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace test {

/// A file system in memory: what portable code sees of directories and
/// metadata, with no disk under it. Lists in the order things were made,
/// as readdir does; reads nothing (openRead() is always -1).
class FakeFileSystem final : public crosspad::IFileSystem {
public:
    struct Node {
        std::string path;
        bool        dir;
        uint64_t    size;
    };

    void addDir(const std::string& path) {
        std::lock_guard<std::mutex> l(m_);
        nodes_.push_back({path, true, 0});
    }
    void addFile(const std::string& path, uint64_t size = 0) {
        std::lock_guard<std::mutex> l(m_);
        nodes_.push_back({path, false, size});
    }
    /// stat() of @p path answers Failed, as for a card that cannot be asked.
    void failStat(const std::string& path) {
        std::lock_guard<std::mutex> l(m_);
        failing_.insert(path);
    }
    bool has(const std::string& path) {
        std::lock_guard<std::mutex> l(m_);
        return find(path) != nodes_.end();
    }

    std::atomic<int> stats{0};
    std::atomic<int> lists{0};
    std::atomic<int> delivered{0};   ///< entries handed to list() callbacks

    bool list(const char* dir, EntryFn each, void* ctx) override {
        ++lists;
        std::vector<std::pair<std::string, bool>> entries;
        {
            std::lock_guard<std::mutex> l(m_);
            const auto d = find(dir);
            if (d == nodes_.end() || !d->dir) return false;
            for (const Node& n : nodes_) {
                if (parent(n.path) == dir) entries.emplace_back(n.path.substr(n.path.rfind('/') + 1), n.dir);
            }
        }
        for (const auto& e : entries) {
            ++delivered;
            if (!each(ctx, e.first.c_str(), e.second)) break;
        }
        return true;
    }

    crosspad::FsStat stat(const char* path, crosspad::FileStat* out) override {
        ++stats;
        std::lock_guard<std::mutex> l(m_);
        if (failing_.count(path)) return crosspad::FsStat::Failed;
        const auto n = find(path);
        if (n == nodes_.end()) return crosspad::FsStat::Missing;
        if (out) {
            out->isDir = n->dir;
            out->size = n->size;
            out->modified = 0;
        }
        return crosspad::FsStat::Found;
    }

    bool makeDir(const char* path) override {
        std::lock_guard<std::mutex> l(m_);
        const auto n = find(path);
        if (n != nodes_.end()) return n->dir;
        const auto p = find(parent(path));
        if (p == nodes_.end() || !p->dir) return false;
        nodes_.push_back({path, true, 0});
        return true;
    }

    bool remove(const char* path) override {
        std::lock_guard<std::mutex> l(m_);
        const auto n = find(path);
        if (n == nodes_.end() || n->dir) return false;
        nodes_.erase(n);
        return true;
    }

    bool removeDir(const char* path) override {
        std::lock_guard<std::mutex> l(m_);
        const auto n = find(path);
        if (n == nodes_.end() || !n->dir) return false;
        for (const Node& c : nodes_) {
            if (parent(c.path) == path) return false;
        }
        nodes_.erase(n);
        return true;
    }

    bool rename(const char* from, const char* to) override {
        std::lock_guard<std::mutex> l(m_);
        if (find(from) == nodes_.end() || find(to) != nodes_.end()) return false;
        const std::string f = from;
        for (Node& n : nodes_) {
            if (n.path == f) n.path = to;
            else if (n.path.compare(0, f.size() + 1, f + "/") == 0) n.path = to + n.path.substr(f.size());
        }
        return true;
    }

    int  openRead(const char*) override { return -1; }
    long read(int, void*, size_t) override { return -1; }
    bool seek(int, uint32_t) override { return false; }
    void close(int) override {}

private:
    static std::string parent(const std::string& p) {
        const auto slash = p.rfind('/');
        return slash == std::string::npos ? std::string() : p.substr(0, slash);
    }
    std::vector<Node>::iterator find(const std::string& path) {
        return std::find_if(nodes_.begin(), nodes_.end(), [&](const Node& n) { return n.path == path; });
    }

    std::mutex m_;
    std::vector<Node> nodes_;
    std::set<std::string> failing_;
};

/// @p fs as PlatformServices::fileSystem for as long as this lives.
struct UseFileSystem {
    explicit UseFileSystem(crosspad::IFileSystem& fs) { crosspad::getPlatformServices().setFileSystem(&fs); }
    ~UseFileSystem() { crosspad::getPlatformServices().setFileSystem(nullptr); }
    UseFileSystem(const UseFileSystem&) = delete;
    UseFileSystem& operator=(const UseFileSystem&) = delete;
};

} // namespace test
