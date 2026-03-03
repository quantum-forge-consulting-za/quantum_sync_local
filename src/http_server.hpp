#pragma once

#include <boost/asio.hpp>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <memory>

class HttpServer {
public:
    HttpServer(uint16_t port, const std::string& deviceName);
    ~HttpServer();

    void start();
    void stop();

private:
    void acceptLoop();
    void handleSession(boost::asio::ip::tcp::socket socket);
    std::string handleRequest(const std::string& method, const std::string& path,
                              const std::string& body);

    // API handlers
    std::string getStatusJson();
    std::string handleVolume(const std::string& body);
    std::string handleMute(const std::string& body);
    std::string handlePlayback(const std::string& body);
    std::string getJournalJson();

    // Pages
    std::string getMainPage();
    std::string getLogsPage();

    // System stats (Linux /proc)
    double getUptimeSeconds();
    double getCpuPercent();
    int getMemUsedMb();
    int getMemTotalMb();

    // Shell command helpers
    std::string execCommand(const std::string& cmd);
    int getVolume();
    void setVolume(int volume);

    uint16_t port_;
    std::string deviceName_;
    std::atomic<bool> running_{false};
    std::atomic<bool> muted_{false};
    int preMuteVolume_{50};

    boost::asio::io_context ioContext_;
    std::thread serverThread_;
    std::shared_ptr<boost::asio::ip::tcp::acceptor> acceptor_;
};
