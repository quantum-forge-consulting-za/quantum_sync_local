#include "http_server.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>

using boost::asio::ip::tcp;

HttpServer::HttpServer(uint16_t port, const std::string& deviceName)
    : port_(port)
    , deviceName_(deviceName)
{
}

HttpServer::~HttpServer() {
    stop();
}

void HttpServer::start() {
    if (running_) return;
    running_ = true;

    serverThread_ = std::thread([this]() {
        try {
            acceptLoop();
        } catch (const std::exception& e) {
            std::cerr << "HTTP server error: " << e.what() << std::endl;
        }
    });

    std::cout << "QuantumSync Local web GUI on port " << port_ << std::endl;
}

void HttpServer::stop() {
    if (!running_.exchange(false)) return;

    if (acceptor_) {
        boost::system::error_code ec;
        acceptor_->close(ec);
    }
    ioContext_.stop();

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

void HttpServer::acceptLoop() {
    acceptor_ = std::make_shared<tcp::acceptor>(ioContext_, tcp::endpoint(tcp::v4(), port_));
    acceptor_->set_option(boost::asio::socket_base::reuse_address(true));

    while (running_) {
        try {
            tcp::socket socket(ioContext_);
            acceptor_->accept(socket);

            try {
                handleSession(std::move(socket));
            } catch (const std::exception& e) {
                std::cerr << "HTTP session error: " << e.what() << std::endl;
            }
        } catch (const boost::system::system_error& e) {
            if (running_) {
                std::cerr << "HTTP accept error: " << e.what() << std::endl;
            }
        }
    }
}

void HttpServer::handleSession(tcp::socket socket) {
    boost::asio::streambuf request;
    boost::asio::read_until(socket, request, "\r\n\r\n");

    std::istream request_stream(&request);
    std::string method, path, version;
    request_stream >> method >> path >> version;

    // Parse headers for Content-Length
    std::string header_line;
    std::getline(request_stream, header_line);
    int content_length = 0;
    while (std::getline(request_stream, header_line) && header_line != "\r") {
        if (header_line.find("Content-Length:") == 0) {
            std::string len_str = header_line.substr(15);
            len_str.erase(0, len_str.find_first_not_of(" \t"));
            len_str.erase(len_str.find_last_not_of(" \r\n\t") + 1);
            try {
                content_length = std::stoi(len_str);
                if (content_length < 0 || content_length > 10240) content_length = 0;
            } catch (...) { content_length = 0; }
        }
    }

    // Read body if present
    std::string body;
    if (content_length > 0) {
        size_t already_read = request.size();
        if (already_read > 0) {
            std::vector<char> buf(already_read);
            request_stream.read(buf.data(), already_read);
            body.assign(buf.begin(), buf.end());
        }
        if (static_cast<int>(body.size()) < content_length) {
            size_t remaining = content_length - body.size();
            std::vector<char> extra(remaining);
            boost::asio::read(socket, boost::asio::buffer(extra));
            body.append(extra.begin(), extra.end());
        }
    }

    std::string content = handleRequest(method, path, body);

    std::string content_type = "text/html";
    if (path.find("/api/") == 0) content_type = "application/json";

    std::ostringstream response;
    response << "HTTP/1.1 200 OK\r\n";
    response << "Content-Type: " << content_type << "\r\n";
    response << "Content-Length: " << content.length() << "\r\n";
    response << "Access-Control-Allow-Origin: *\r\n";
    response << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    response << "Access-Control-Allow-Headers: Content-Type\r\n";
    response << "Connection: close\r\n\r\n";
    response << content;

    boost::asio::write(socket, boost::asio::buffer(response.str()));
}

std::string HttpServer::handleRequest(const std::string& method,
                                       const std::string& path,
                                       const std::string& body) {
    if (path == "/" || path == "/index.html") return getMainPage();
    else if (path == "/api/status" && method == "GET") return getStatusJson();
    else if (path == "/api/volume" && method == "POST") return handleVolume(body);
    else if (path == "/api/mute" && method == "POST") return handleMute(body);
    else if (path == "/api/playback" && method == "POST") return handlePlayback(body);
    else if (path == "/api/journal" && method == "GET") return getJournalJson();
    else if (path == "/logs" || path == "/logs.html") return getLogsPage();
    else if (method == "OPTIONS") return "";
    return "{\"error\":\"Not found\"}";
}

// ─── Shell command helper ──────────────────────────────────────

std::string HttpServer::execCommand(const std::string& cmd) {
    std::string result;
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        result += buffer;
    }
    pclose(pipe);
    // Trim trailing newline
    while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) {
        result.pop_back();
    }
    return result;
}

// ─── Volume via amixer ─────────────────────────────────────────

int HttpServer::getVolume() {
    // Try to parse amixer output for current volume percentage
    std::string output = execCommand("amixer sget Headphone 2>/dev/null || amixer sget Master 2>/dev/null || amixer sget PCM 2>/dev/null");
    // Look for [XX%] pattern
    size_t pos = output.find('[');
    while (pos != std::string::npos) {
        size_t end = output.find('%', pos);
        if (end != std::string::npos && end > pos + 1) {
            std::string pct = output.substr(pos + 1, end - pos - 1);
            try {
                int vol = std::stoi(pct);
                if (vol >= 0 && vol <= 100) return vol;
            } catch (...) {}
        }
        pos = output.find('[', pos + 1);
    }
    return 50;  // Fallback
}

void HttpServer::setVolume(int volume) {
    volume = std::clamp(volume, 0, 100);
    std::string cmd = "amixer sset Headphone " + std::to_string(volume) + "% 2>/dev/null"
                      " || amixer sset Master " + std::to_string(volume) + "% 2>/dev/null"
                      " || amixer sset PCM " + std::to_string(volume) + "% 2>/dev/null";
    execCommand(cmd);
}

// ─── API Handlers ──────────────────────────────────────────────

std::string HttpServer::getStatusJson() {
    std::string currentTrack = execCommand("mpc current 2>/dev/null");
    if (currentTrack.empty()) currentTrack = "No music loaded";

    // Parse mpc status for state
    std::string mpcStatus = execCommand("mpc status 2>/dev/null");
    std::string state = "stopped";
    if (mpcStatus.find("[playing]") != std::string::npos) state = "playing";
    else if (mpcStatus.find("[paused]") != std::string::npos) state = "paused";

    // Track count
    std::string trackCount = execCommand("mpc playlist 2>/dev/null | wc -l");

    int volume = muted_.load() ? 0 : getVolume();

    std::ostringstream json;
    json << std::fixed;
    json.precision(1);
    json << "{"
         << "\"track\":\"";
    // Escape special chars in track name
    for (char c : currentTrack) {
        if (c == '"') json << "\\\"";
        else if (c == '\\') json << "\\\\";
        else json << c;
    }
    json << "\","
         << "\"state\":\"" << state << "\","
         << "\"volume\":" << volume << ","
         << "\"muted\":" << (muted_.load() ? "true" : "false") << ","
         << "\"deviceName\":\"" << deviceName_ << "\","
         << "\"trackCount\":" << (trackCount.empty() ? "0" : trackCount) << ","
         << "\"uptimeSec\":" << getUptimeSeconds() << ","
         << "\"cpuPercent\":" << getCpuPercent() << ","
         << "\"memUsedMb\":" << getMemUsedMb() << ","
         << "\"memTotalMb\":" << getMemTotalMb()
         << "}";
    return json.str();
}

std::string HttpServer::handleVolume(const std::string& body) {
    size_t pos = body.find("\"volume\"");
    if (pos != std::string::npos) {
        size_t colon = body.find(':', pos);
        if (colon != std::string::npos) {
            size_t end = body.find_first_of(",}", colon);
            if (end != std::string::npos) {
                std::string val = body.substr(colon + 1, end - colon - 1);
                val.erase(0, val.find_first_not_of(" \t"));
                try {
                    int volume = std::stoi(val);
                    volume = std::clamp(volume, 0, 100);
                    setVolume(volume);
                    if (muted_.load()) {
                        muted_ = false;  // Unmute when volume is explicitly set
                    }
                    return "{\"result\":\"OK\",\"volume\":" + std::to_string(volume) + "}";
                } catch (...) {}
            }
        }
    }
    return "{\"error\":\"Invalid volume\"}";
}

std::string HttpServer::handleMute(const std::string& body) {
    if (body.find("true") != std::string::npos) {
        preMuteVolume_ = getVolume();
        muted_ = true;
        setVolume(0);
        return "{\"result\":\"OK\",\"muted\":true}";
    } else {
        muted_ = false;
        setVolume(preMuteVolume_);
        return "{\"result\":\"OK\",\"muted\":false,\"volume\":" +
               std::to_string(preMuteVolume_) + "}";
    }
}

std::string HttpServer::handlePlayback(const std::string& body) {
    std::string action;

    // Parse action from JSON
    size_t pos = body.find("\"action\"");
    if (pos != std::string::npos) {
        size_t start = body.find('"', pos + 8);
        if (start != std::string::npos) {
            start++;
            size_t end = body.find('"', start);
            if (end != std::string::npos) {
                action = body.substr(start, end - start);
            }
        }
    }

    if (action == "play") {
        execCommand("mpc play 2>/dev/null");
    } else if (action == "pause") {
        execCommand("mpc pause 2>/dev/null");
    } else if (action == "toggle") {
        execCommand("mpc toggle 2>/dev/null");
    } else if (action == "next") {
        execCommand("mpc next 2>/dev/null");
    } else if (action == "prev") {
        execCommand("mpc prev 2>/dev/null");
    } else {
        return "{\"error\":\"Unknown action: " + action + "\"}";
    }

    return "{\"result\":\"OK\",\"action\":\"" + action + "\"}";
}

// ─── System stats (Linux /proc) ────────────────────────────────

double HttpServer::getUptimeSeconds() {
    try {
        std::ifstream f("/proc/uptime");
        if (f.is_open()) {
            double uptime = 0.0;
            f >> uptime;
            return uptime;
        }
    } catch (...) {}
    return -1.0;
}

double HttpServer::getCpuPercent() {
    auto readCpuTimes = []() -> std::pair<long long, long long> {
        std::ifstream f("/proc/stat");
        if (!f.is_open()) return {0, 0};
        std::string cpu;
        long long user, nice, system, idle, iowait, irq, softirq, steal;
        f >> cpu >> user >> nice >> system >> idle >> iowait >> irq >> softirq >> steal;
        long long totalIdle = idle + iowait;
        long long total = user + nice + system + idle + iowait + irq + softirq + steal;
        return {total, totalIdle};
    };

    auto [total1, idle1] = readCpuTimes();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    auto [total2, idle2] = readCpuTimes();

    long long totalDelta = total2 - total1;
    long long idleDelta = idle2 - idle1;

    if (totalDelta <= 0) return 0.0;
    return 100.0 * (1.0 - static_cast<double>(idleDelta) / static_cast<double>(totalDelta));
}

int HttpServer::getMemUsedMb() {
    try {
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return -1;
        long long memTotal = 0, memAvailable = 0;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                iss >> memTotal;
            } else if (line.find("MemAvailable:") == 0) {
                std::istringstream iss(line.substr(13));
                iss >> memAvailable;
            }
        }
        return static_cast<int>((memTotal - memAvailable) / 1024);
    } catch (...) {}
    return -1;
}

int HttpServer::getMemTotalMb() {
    try {
        std::ifstream f("/proc/meminfo");
        if (!f.is_open()) return -1;
        std::string line;
        while (std::getline(f, line)) {
            if (line.find("MemTotal:") == 0) {
                std::istringstream iss(line.substr(9));
                long long kb = 0;
                iss >> kb;
                return static_cast<int>(kb / 1024);
            }
        }
    } catch (...) {}
    return -1;
}

// ─── Journal API ───────────────────────────────────────────────

std::string HttpServer::getJournalJson() {
    std::string result;
    FILE* pipe = popen("journalctl -u quantumsync-local -u mpd -n 100 --no-pager 2>&1", "r");
    if (!pipe) {
        return "{\"error\":\"Failed to read journal\",\"lines\":[],\"count\":0}";
    }

    std::vector<std::string> lines;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        std::string line(buffer);
        if (!line.empty() && line.back() == '\n') line.pop_back();
        lines.push_back(line);
    }
    pclose(pipe);

    std::ostringstream json;
    json << "{\"lines\":[";
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i > 0) json << ",";
        json << "\"";
        for (char c : lines[i]) {
            if (c == '"') json << "\\\"";
            else if (c == '\\') json << "\\\\";
            else if (c == '\n') json << "\\n";
            else if (c == '\r') json << "\\r";
            else if (c == '\t') json << "\\t";
            else if (static_cast<unsigned char>(c) < 0x20) json << " ";
            else json << c;
        }
        json << "\"";
    }
    json << "],\"count\":" << lines.size() << "}";
    return json.str();
}

// ─── Embedded HTML GUI ─────────────────────────────────────────

std::string HttpServer::getMainPage() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>QuantumSync Local</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', Roboto, sans-serif;
            background: linear-gradient(135deg, #0f0c29, #302b63, #24243e);
            color: #fff;
            min-height: 100vh;
            display: flex;
            align-items: center;
            justify-content: center;
        }
        .card {
            background: rgba(255,255,255,0.08);
            border-radius: 16px;
            padding: 32px;
            width: 380px;
            backdrop-filter: blur(10px);
            border: 1px solid rgba(255,255,255,0.1);
        }
        h1 { font-size: 1.4em; margin-bottom: 2px; }
        .subtitle { color: rgba(255,255,255,0.5); font-size: 0.85em; margin-bottom: 20px; }

        .now-playing {
            background: rgba(255,255,255,0.05);
            border-radius: 10px;
            padding: 16px;
            margin-bottom: 20px;
            text-align: center;
        }
        .np-label { color: rgba(255,255,255,0.4); font-size: 0.75em; text-transform: uppercase; letter-spacing: 1px; margin-bottom: 6px; }
        .np-track { font-size: 1em; font-weight: 500; margin-bottom: 4px; word-break: break-word; min-height: 1.2em; }
        .np-state {
            display: inline-block;
            font-size: 0.75em;
            padding: 2px 10px;
            border-radius: 10px;
            background: rgba(255,255,255,0.1);
            color: rgba(255,255,255,0.6);
        }
        .np-state.playing { background: rgba(76,175,80,0.2); color: #81c784; }

        .controls {
            display: flex;
            justify-content: center;
            gap: 16px;
            margin-bottom: 24px;
        }
        .ctrl-btn {
            width: 48px; height: 48px;
            border-radius: 50%;
            border: 1px solid rgba(255,255,255,0.15);
            background: rgba(255,255,255,0.06);
            color: #fff;
            font-size: 1.2em;
            cursor: pointer;
            display: flex;
            align-items: center;
            justify-content: center;
            transition: all 0.2s;
        }
        .ctrl-btn:hover { background: rgba(124,77,255,0.3); border-color: #7c4dff; }
        .ctrl-btn.play-btn { width: 56px; height: 56px; font-size: 1.4em; background: rgba(124,77,255,0.2); border-color: #7c4dff; }
        .ctrl-btn.play-btn:hover { background: rgba(124,77,255,0.5); }

        .volume-section { margin-bottom: 20px; }
        .volume-section label { display: block; margin-bottom: 8px; color: rgba(255,255,255,0.6); font-size: 0.85em; }
        .volume-row { display: flex; align-items: center; gap: 12px; }
        .mute-btn {
            background: none; border: 1px solid rgba(255,255,255,0.15);
            color: rgba(255,255,255,0.6); border-radius: 6px;
            padding: 4px 8px; cursor: pointer; font-size: 0.9em;
            transition: all 0.2s;
        }
        .mute-btn:hover { border-color: #7c4dff; color: #7c4dff; }
        .mute-btn.muted { background: rgba(244,67,54,0.2); border-color: #f44336; color: #f44336; }
        input[type=range] {
            flex: 1; height: 6px; -webkit-appearance: none; appearance: none;
            background: rgba(255,255,255,0.2); border-radius: 3px; outline: none;
        }
        input[type=range]::-webkit-slider-thumb {
            -webkit-appearance: none; width: 20px; height: 20px;
            background: #7c4dff; border-radius: 50%; cursor: pointer;
        }
        .vol-num { min-width: 36px; text-align: right; font-weight: 600; }

        .sys-info { padding-top: 16px; border-top: 1px solid rgba(255,255,255,0.06); }
        .status-row {
            display: flex; justify-content: space-between; align-items: center;
            padding: 6px 0; font-size: 0.85em;
        }
        .status-label { color: rgba(255,255,255,0.5); }
        .status-value { font-weight: 500; }
        .back-link { color: #7c4dff; text-decoration: none; font-size: 0.8em; }
        .back-link:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <div class="card">
        <h1>QuantumSync Local</h1>
        <div class="subtitle" id="deviceName">Loading...</div>

        <div class="now-playing">
            <div class="np-label">Now Playing</div>
            <div class="np-track" id="trackName">--</div>
            <span class="np-state" id="playState">--</span>
        </div>

        <div class="controls">
            <button class="ctrl-btn" id="prevBtn" title="Previous">&#9198;</button>
            <button class="ctrl-btn play-btn" id="playBtn" title="Play/Pause">&#9654;</button>
            <button class="ctrl-btn" id="nextBtn" title="Next">&#9197;</button>
        </div>

        <div class="volume-section">
            <label>Volume</label>
            <div class="volume-row">
                <button class="mute-btn" id="muteBtn" title="Mute">&#128264;</button>
                <input type="range" id="volumeSlider" min="0" max="100" value="50">
                <span class="vol-num" id="volumeVal">50%</span>
            </div>
        </div>

        <div class="sys-info">
            <div class="status-row">
                <span class="status-label">Tracks</span>
                <span class="status-value" id="trackCount">--</span>
            </div>
            <div class="status-row">
                <span class="status-label">CPU</span>
                <span class="status-value" id="cpuInfo">--</span>
            </div>
            <div class="status-row">
                <span class="status-label">Memory</span>
                <span class="status-value" id="memInfo">--</span>
            </div>
            <div class="status-row">
                <span class="status-label">Uptime</span>
                <span class="status-value" id="uptimeInfo">--</span>
            </div>
            <div class="status-row">
                <span class="status-label"></span>
                <a href="/logs" class="back-link">View Logs</a>
            </div>
        </div>
    </div>

    <script>
        const slider = document.getElementById('volumeSlider');
        const volVal = document.getElementById('volumeVal');
        const playBtn = document.getElementById('playBtn');
        const prevBtn = document.getElementById('prevBtn');
        const nextBtn = document.getElementById('nextBtn');
        const muteBtn = document.getElementById('muteBtn');
        let currentState = 'stopped';
        let isMuted = false;

        function formatUptime(sec) {
            if (sec < 0) return '--';
            const h = Math.floor(sec / 3600);
            const m = Math.floor((sec % 3600) / 60);
            if (h > 0) return h + 'h ' + m + 'm';
            return m + 'm';
        }

        function updateStatus() {
            fetch('/api/status')
                .then(r => r.json())
                .then(data => {
                    document.getElementById('deviceName').textContent = data.deviceName || 'Local Player';
                    document.getElementById('trackName').textContent = data.track || 'No music loaded';

                    const stateEl = document.getElementById('playState');
                    currentState = data.state;
                    stateEl.textContent = data.state.charAt(0).toUpperCase() + data.state.slice(1);
                    stateEl.className = 'np-state' + (data.state === 'playing' ? ' playing' : '');

                    // Update play button icon
                    playBtn.innerHTML = (data.state === 'playing') ? '&#10074;&#10074;' : '&#9654;';

                    // Volume
                    isMuted = data.muted;
                    if (!isMuted) {
                        slider.value = data.volume;
                        volVal.textContent = data.volume + '%';
                    } else {
                        volVal.textContent = 'Muted';
                    }
                    muteBtn.className = 'mute-btn' + (isMuted ? ' muted' : '');
                    muteBtn.innerHTML = isMuted ? '&#128263;' : '&#128264;';

                    document.getElementById('trackCount').textContent = data.trackCount || '0';
                    document.getElementById('cpuInfo').textContent =
                        (data.cpuPercent >= 0 ? data.cpuPercent.toFixed(1) + '%' : '--');
                    document.getElementById('memInfo').textContent =
                        (data.memUsedMb >= 0 ? data.memUsedMb + ' / ' + (data.memTotalMb || '?') + ' MB' : '--');
                    document.getElementById('uptimeInfo').textContent = formatUptime(data.uptimeSec);
                })
                .catch(() => {});
        }

        // Volume slider
        let debounce = null;
        slider.addEventListener('input', function() {
            volVal.textContent = this.value + '%';
            clearTimeout(debounce);
            debounce = setTimeout(() => {
                fetch('/api/volume', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body: JSON.stringify({volume: parseInt(this.value)})
                });
            }, 100);
        });

        // Playback controls
        playBtn.addEventListener('click', () => {
            fetch('/api/playback', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({action: 'toggle'})
            }).then(() => setTimeout(updateStatus, 300));
        });

        prevBtn.addEventListener('click', () => {
            fetch('/api/playback', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({action: 'prev'})
            }).then(() => setTimeout(updateStatus, 300));
        });

        nextBtn.addEventListener('click', () => {
            fetch('/api/playback', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({action: 'next'})
            }).then(() => setTimeout(updateStatus, 300));
        });

        muteBtn.addEventListener('click', () => {
            fetch('/api/mute', {
                method: 'POST',
                headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({muted: !isMuted})
            }).then(() => setTimeout(updateStatus, 300));
        });

        updateStatus();
        setInterval(updateStatus, 3000);
    </script>
</body>
</html>)HTML";
}

// ─── Logs Page ─────────────────────────────────────────────────

std::string HttpServer::getLogsPage() {
    return R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>QuantumSync Local - Logs</title>
    <style>
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Courier New', monospace;
            background: #1a1a2e;
            color: #e0e0e0;
            min-height: 100vh;
            padding: 20px;
        }
        .header {
            display: flex; justify-content: space-between; align-items: center;
            margin-bottom: 16px; flex-wrap: wrap; gap: 10px;
        }
        h1 { font-size: 1.2em; color: #7c4dff; }
        .controls { display: flex; gap: 10px; align-items: center; }
        .btn {
            padding: 6px 14px; border: 1px solid #7c4dff; background: transparent;
            color: #7c4dff; border-radius: 6px; cursor: pointer; font-size: 0.85em;
            transition: all 0.2s;
        }
        .btn:hover { background: #7c4dff; color: #fff; }
        .btn.active { background: #7c4dff; color: #fff; }
        .status-badge {
            font-size: 0.75em; padding: 3px 8px; border-radius: 10px;
            background: #2d2d44; color: #aaa;
        }
        .status-badge.live { background: #1b5e20; color: #81c784; }
        #logContainer {
            background: #0d0d1a; border-radius: 8px; padding: 16px;
            max-height: calc(100vh - 100px); overflow-y: auto;
            font-size: 0.8em; line-height: 1.6;
            border: 1px solid #2d2d44;
        }
        .log-line { white-space: pre-wrap; word-break: break-all; padding: 1px 0; }
        .log-line:hover { background: rgba(124, 77, 255, 0.1); }
        .back-link { color: #7c4dff; text-decoration: none; font-size: 0.9em; }
        .back-link:hover { text-decoration: underline; }
    </style>
</head>
<body>
    <div class="header">
        <div>
            <h1>QuantumSync Local Logs</h1>
            <a href="/" class="back-link">&larr; Back to Player</a>
        </div>
        <div class="controls">
            <span class="status-badge" id="statusBadge">Paused</span>
            <button class="btn active" id="autoRefreshBtn">Auto-refresh: ON</button>
            <button class="btn" id="refreshBtn">Refresh Now</button>
        </div>
    </div>
    <div id="logContainer">Loading...</div>
    <script>
        let autoRefresh = true;
        let timer = null;

        function fetchLogs() {
            fetch('/api/journal')
                .then(r => r.json())
                .then(data => {
                    const container = document.getElementById('logContainer');
                    const wasAtBottom = container.scrollHeight - container.scrollTop - container.clientHeight < 50;
                    container.textContent = '';
                    (data.lines || []).forEach(line => {
                        const div = document.createElement('div');
                        div.className = 'log-line';
                        div.textContent = line;
                        container.appendChild(div);
                    });
                    if (wasAtBottom) container.scrollTop = container.scrollHeight;
                    document.getElementById('statusBadge').textContent =
                        data.count + ' lines | ' + new Date().toLocaleTimeString();
                    document.getElementById('statusBadge').className =
                        'status-badge' + (autoRefresh ? ' live' : '');
                })
                .catch(() => {
                    document.getElementById('statusBadge').textContent = 'Error';
                    document.getElementById('statusBadge').className = 'status-badge';
                });
        }

        function toggleAutoRefresh() {
            autoRefresh = !autoRefresh;
            const btn = document.getElementById('autoRefreshBtn');
            btn.textContent = 'Auto-refresh: ' + (autoRefresh ? 'ON' : 'OFF');
            btn.className = 'btn' + (autoRefresh ? ' active' : '');
            if (autoRefresh) {
                fetchLogs();
                timer = setInterval(fetchLogs, 3000);
            } else {
                clearInterval(timer);
            }
        }

        document.getElementById('autoRefreshBtn').addEventListener('click', toggleAutoRefresh);
        document.getElementById('refreshBtn').addEventListener('click', fetchLogs);

        fetchLogs();
        timer = setInterval(fetchLogs, 3000);
    </script>
</body>
</html>)HTML";
}
