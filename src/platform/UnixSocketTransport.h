#pragma once

#if defined(__APPLE__) || defined(__linux__)

#include <sulla/ILocalTransport.h>
#include <sulla/Logger.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <fcntl.h>

#include <atomic>
#include <thread>
#include <mutex>
#include <vector>
#include <algorithm>
#include <cstring>

namespace sulla {

/**
 * UnixSocketTransport — streams audio chunks over a Unix domain socket.
 *
 * Server side: creates a socket at the given path, accepts connections,
 * and broadcasts binary-framed audio chunks to all connected clients.
 *
 * Binary frame format (matches AudioDriverClient.ts in Sulla Desktop):
 *   [1 byte: source (0=mic, 1=speaker)]
 *   [4 bytes: payload length, big-endian]
 *   [N bytes: raw PCM audio]
 */
class UnixSocketTransport : public ILocalTransport {
public:
    ~UnixSocketTransport() override {
        stopServer();
        disconnect();
    }

    // ── Server side ─────────────────────────────────────────────

    bool startServer(const std::string& socketPath, uint16_t /*tcpPort*/) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (serverRunning_.load()) {
            SULLA_LOG_WARN("Transport", "Server already running");
            return true;
        }

        socketPath_ = socketPath;

        // Remove stale socket file — must succeed for bind() to work
        if (::unlink(socketPath.c_str()) < 0 && errno != ENOENT) {
            SULLA_LOG_WARN("Transport", "Could not remove stale socket "
                + socketPath + ": " + std::string(strerror(errno))
                + " — attempting bind anyway");
        }

        serverFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (serverFd_ < 0) {
            SULLA_LOG_ERROR("Transport", "socket() failed: " + std::string(strerror(errno)));
            return false;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        if (::bind(serverFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            SULLA_LOG_ERROR("Transport", "bind() failed on " + socketPath + ": " + std::string(strerror(errno)));
            ::close(serverFd_);
            serverFd_ = -1;
            return false;
        }

        // Allow non-root processes (Sulla Desktop) to connect
        ::chmod(socketPath.c_str(), 0777);

        if (::listen(serverFd_, 4) < 0) {
            SULLA_LOG_ERROR("Transport", "listen() failed: " + std::string(strerror(errno)));
            ::close(serverFd_);
            serverFd_ = -1;
            return false;
        }

        // Make the server socket non-blocking so accept thread can exit cleanly
        int flags = ::fcntl(serverFd_, F_GETFL, 0);
        ::fcntl(serverFd_, F_SETFL, flags | O_NONBLOCK);

        serverRunning_.store(true);

        acceptThread_ = std::thread([this]() {
            SULLA_LOG_INFO("Transport", "Accept thread started");
            while (serverRunning_.load()) {
                int clientFd = ::accept(serverFd_, nullptr, nullptr);
                if (clientFd >= 0) {
                    std::lock_guard<std::mutex> lock(mutex_);
                    clientFds_.push_back(clientFd);
                    SULLA_LOG_INFO("Transport", "Client connected (fd=" + std::to_string(clientFd)
                        + ", total=" + std::to_string(clientFds_.size()) + ")");
                    emitStatus(true, "Client connected");
                } else {
                    // Non-blocking — sleep briefly before retrying
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
            }
            SULLA_LOG_INFO("Transport", "Accept thread exiting");
        });

        SULLA_LOG_INFO("Transport", "Server listening on " + socketPath);
        emitStatus(true, "Listening on " + socketPath);
        return true;
    }

    bool sendChunk(const AudioChunk& chunk) override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (clientFds_.empty()) return true; // No clients — not an error

        // Encode: [1B source][4B length BE][NB audio]
        auto frame = encodeFrame(chunk);

        // Broadcast to all clients, remove dead ones
        std::vector<int> deadFds;
        for (int fd : clientFds_) {
            ssize_t written = ::send(fd, frame.data(), frame.size(), MSG_NOSIGNAL);
            if (written < 0) {
                SULLA_LOG_DEBUG("Transport", "Client fd=" + std::to_string(fd) + " dead, removing");
                ::close(fd);
                deadFds.push_back(fd);
            }
        }

        for (int fd : deadFds) {
            clientFds_.erase(std::remove(clientFds_.begin(), clientFds_.end(), fd), clientFds_.end());
        }

        if (!deadFds.empty() && clientFds_.empty()) {
            emitStatus(false, "All clients disconnected");
        }

        return true;
    }

    void stopServer() override {
        serverRunning_.store(false);

        if (acceptThread_.joinable()) {
            acceptThread_.join();
        }

        std::lock_guard<std::mutex> lock(mutex_);

        for (int fd : clientFds_) {
            ::close(fd);
        }
        clientFds_.clear();

        if (serverFd_ >= 0) {
            ::close(serverFd_);
            serverFd_ = -1;
        }

        if (!socketPath_.empty()) {
            ::unlink(socketPath_.c_str());
            socketPath_.clear();
        }

        SULLA_LOG_INFO("Transport", "Server stopped");
        emitStatus(false, "Server stopped");
    }

    uint32_t clientCount() const override {
        std::lock_guard<std::mutex> lock(mutex_);
        return static_cast<uint32_t>(clientFds_.size());
    }

    // ── Client side (not used by the driver binary) ─────────────

    bool connect(const std::string& socketPath, uint16_t /*tcpPort*/) override {
        clientConnFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (clientConnFd_ < 0) return false;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socketPath.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(clientConnFd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(clientConnFd_);
            clientConnFd_ = -1;
            return false;
        }

        return true;
    }

    void disconnect() override {
        if (clientConnFd_ >= 0) {
            ::close(clientConnFd_);
            clientConnFd_ = -1;
        }
    }

private:
    mutable std::mutex mutex_;
    int serverFd_ = -1;
    std::vector<int> clientFds_;
    std::string socketPath_;
    std::atomic<bool> serverRunning_{false};
    std::thread acceptThread_;
    int clientConnFd_ = -1;

    /**
     * Encode a chunk into the binary frame format:
     * [1B source][4B payload length big-endian][NB audio]
     */
    static std::vector<uint8_t> encodeFrame(const AudioChunk& chunk) {
        uint32_t payloadLen = static_cast<uint32_t>(chunk.audio.size());
        std::vector<uint8_t> frame(5 + payloadLen);

        frame[0] = static_cast<uint8_t>(chunk.source);
        frame[1] = static_cast<uint8_t>((payloadLen >> 24) & 0xFF);
        frame[2] = static_cast<uint8_t>((payloadLen >> 16) & 0xFF);
        frame[3] = static_cast<uint8_t>((payloadLen >> 8)  & 0xFF);
        frame[4] = static_cast<uint8_t>((payloadLen)       & 0xFF);

        std::memcpy(frame.data() + 5, chunk.audio.data(), payloadLen);
        return frame;
    }
};

} // namespace sulla

#endif // __APPLE__ || __linux__
