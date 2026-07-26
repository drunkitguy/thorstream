#include "web_console.h"

#include <WinSock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <sstream>

namespace thorstream {
namespace {

constexpr uintptr_t kInvalid = ~uintptr_t{0};

std::string JsonEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                // Control characters are the only thing left that would produce
                // invalid JSON; nothing here is expected to contain any.
                if (static_cast<unsigned char>(c) < 0x20) {
                    char escape[8];
                    snprintf(escape, sizeof(escape), "\\u%04x", c);
                    out += escape;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

void SendResponse(SOCKET socket, const char* statusLine, const char* contentType,
                  const std::string& body) {
    std::ostringstream response;
    response << "HTTP/1.1 " << statusLine << "\r\n"
             << "Content-Type: " << contentType << "\r\n"
             << "Content-Length: " << body.size() << "\r\n"
             // The page polls; caching it would freeze the readout.
             << "Cache-Control: no-store\r\n"
             << "Connection: close\r\n\r\n"
             << body;
    const std::string text = response.str();

    size_t offset = 0;
    while (offset < text.size()) {
        const int sent = send(socket, text.data() + offset,
                              static_cast<int>(text.size() - offset), 0);
        if (sent <= 0) return;
        offset += static_cast<size_t>(sent);
    }
}

std::string FormatBytes(uint64_t bytes) {
    char text[64];
    if (bytes >= 1024ull * 1024 * 1024) {
        snprintf(text, sizeof(text), "%.1f GB", bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= 1024 * 1024) {
        snprintf(text, sizeof(text), "%.1f MB", bytes / (1024.0 * 1024));
    } else {
        snprintf(text, sizeof(text), "%.0f KB", bytes / 1024.0);
    }
    return text;
}

std::string StatusJson(const HostStatus& status) {
    std::ostringstream json;
    json << "{"
         << "\"serving\":" << (status.serving ? "true" : "false")
         << ",\"lastError\":\"" << JsonEscape(status.lastError) << "\""
         << ",\"controlPort\":" << status.controlPort
         << ",\"uptimeSeconds\":" << status.uptimeSeconds
         << ",\"servingSeconds\":" << status.servingSeconds
         << ",\"clientConnected\":" << (status.clientConnected ? "true" : "false")
         << ",\"streaming\":" << (status.streaming ? "true" : "false")
         << ",\"framesSent\":" << status.framesSent
         << ",\"bytes\":\"" << FormatBytes(status.bytesSent) << "\""
         << ",\"gamepad\":\"" << JsonEscape(status.gamepad) << "\""
         << ",\"logPath\":\"" << JsonEscape(status.logPath) << "\""
         << ",\"addresses\":[";
    for (size_t i = 0; i < status.addresses.size(); ++i) {
        if (i) json << ",";
        json << "\"" << JsonEscape(status.addresses[i]) << "\"";
    }
    json << "]}";
    return json.str();
}

// Served as one file so the console has no external dependencies: it has to work
// on a handheld's browser with no internet, and from a PC whose whole problem
// might be that the host is not answering.
constexpr char kPage[] = R"PAGE(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>thorstream host</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin:0; padding:24px; background:#12141a; color:#e6e8ee;
         font:15px/1.5 ui-sans-serif,system-ui,"Segoe UI",sans-serif; }
  .wrap { max-width:620px; margin:0 auto; }
  h1 { font-size:19px; margin:0 0 2px; font-weight:600; letter-spacing:.2px; }
  .sub { color:#8b91a1; font-size:13px; margin-bottom:20px; }
  .card { background:#1a1d26; border:1px solid #272b37; border-radius:12px;
          padding:18px 20px; margin-bottom:14px; }
  .state { display:flex; align-items:center; gap:10px; font-size:17px; font-weight:600; }
  .dot { width:11px; height:11px; border-radius:50%; flex:none; }
  .on { background:#3ddc84; box-shadow:0 0 10px #3ddc8455; }
  .off { background:#6b7280; }
  .busy { background:#f5a623; box-shadow:0 0 10px #f5a62355; }
  .err { color:#ff8080; font-size:13px; margin-top:8px; word-break:break-word; }
  table { width:100%; border-collapse:collapse; margin-top:14px; font-size:14px; }
  td { padding:5px 0; vertical-align:top; }
  td:first-child { color:#8b91a1; width:42%; }
  .addr { font-family:ui-monospace,Consolas,monospace; font-size:13px; }
  .row { display:flex; gap:10px; margin-top:4px; flex-wrap:wrap; }
  button { flex:1 1 120px; padding:11px 16px; border-radius:9px; cursor:pointer;
           border:1px solid #323746; background:#232734; color:#e6e8ee;
           font-size:14px; font-weight:550; font-family:inherit; }
  button:hover:not(:disabled) { background:#2c3140; }
  button:disabled { opacity:.4; cursor:default; }
  button.primary { background:#2b6df5; border-color:#2b6df5; color:#fff; }
  button.primary:hover:not(:disabled) { background:#245fd8; }
  button.danger { color:#ff9a9a; }
  .note { color:#6d7382; font-size:12px; margin-top:16px; }
  code { background:#232734; padding:1px 5px; border-radius:4px; font-size:12px; }
</style>
</head>
<body>
<div class="wrap">
  <h1>thorstream host</h1>
  <div class="sub">Windows 11 &rarr; Ayn Thor</div>

  <div class="card">
    <div class="state"><span id="dot" class="dot off"></span><span id="state">checking...</span></div>
    <div id="error" class="err" hidden></div>
    <table>
      <tr><td>Client</td><td id="client">-</td></tr>
      <tr><td>Frames sent</td><td id="frames">-</td></tr>
      <tr><td>Data sent</td><td id="bytes">-</td></tr>
      <tr><td>Gamepad</td><td id="pad">-</td></tr>
      <tr><td>Process uptime</td><td id="uptime">-</td></tr>
      <tr><td>Connect to</td><td id="addrs" class="addr">-</td></tr>
    </table>
  </div>

  <div class="card">
    <div class="row">
      <button id="start" class="primary" onclick="act('start')">Start</button>
      <button id="stop" class="danger" onclick="act('stop')">Stop</button>
      <button id="restart" onclick="act('restart')">Restart</button>
    </div>
    <div class="note">
      Start and Stop control the streaming listener. The host process itself keeps
      running so this page stays reachable &mdash; to stop it entirely, run
      <code>thorstream-host.exe --stop</code>.
    </div>
  </div>
</div>
<script>
let busy = false;

function seconds(total) {
  const h = Math.floor(total / 3600), m = Math.floor(total % 3600 / 60), s = total % 60;
  return h ? h + "h " + m + "m" : m ? m + "m " + s + "s" : s + "s";
}

function render(d) {
  const dot = document.getElementById("dot");
  const state = document.getElementById("state");
  if (busy) {
    dot.className = "dot busy";
    state.textContent = "working...";
  } else if (d.serving) {
    dot.className = "dot on";
    state.textContent = d.streaming ? "Streaming" : "Running";
  } else {
    dot.className = "dot off";
    state.textContent = "Stopped";
  }

  const error = document.getElementById("error");
  error.hidden = !d.lastError;
  error.textContent = d.lastError || "";

  document.getElementById("client").textContent =
    d.streaming ? "connected, streaming" : d.clientConnected ? "connected, idle" : "none";
  document.getElementById("frames").textContent = d.framesSent.toLocaleString();
  document.getElementById("bytes").textContent = d.bytes;
  document.getElementById("pad").textContent = d.gamepad;
  document.getElementById("uptime").textContent =
    seconds(d.uptimeSeconds) + (d.serving ? " (listening " + seconds(d.servingSeconds) + ")" : "");
  document.getElementById("addrs").innerHTML =
    d.addresses.length ? d.addresses.join("<br>") : "-";

  document.getElementById("start").disabled = busy || d.serving;
  document.getElementById("stop").disabled = busy || !d.serving;
  document.getElementById("restart").disabled = busy;
}

async function refresh() {
  try {
    render(await (await fetch("status", {cache: "no-store"})).json());
  } catch (e) {
    document.getElementById("dot").className = "dot off";
    document.getElementById("state").textContent = "host not responding";
  }
}

async function act(what) {
  busy = true;
  refresh();
  try {
    const r = await (await fetch(what, {method: "POST"})).json();
    if (!r.ok) alert(r.error || "That did not work.");
  } catch (e) {
    // Restart briefly drops the listener; the next poll picks it back up.
  }
  busy = false;
  refresh();
}

refresh();
setInterval(refresh, 2000);
</script>
</body>
</html>
)PAGE";

}  // namespace

WebConsole::~WebConsole() { Stop(); }

bool WebConsole::Start(uint16_t port, std::string* error) {
    // Refcounted, so calling it again after the stream server did is harmless
    // and removes any ordering dependency between the two.
    WSADATA wsa{};
    WSAStartup(MAKEWORD(2, 2), &wsa);

    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        *error = "could not create the web console socket";
        return false;
    }

    // Without this, restarting the host inside the TIME_WAIT window fails to
    // bind and the console silently does not come back.
    BOOL reuse = TRUE;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse),
               sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        *error = "port " + std::to_string(port) + " is already in use";
        closesocket(listener);
        return false;
    }
    if (listen(listener, 8) == SOCKET_ERROR) {
        *error = "could not listen on port " + std::to_string(port);
        closesocket(listener);
        return false;
    }

    listenSocket_ = static_cast<uintptr_t>(listener);
    running_ = true;
    thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void WebConsole::Stop() {
    if (!running_.exchange(false)) return;

    if (listenSocket_ != kInvalid) {
        // Closing the listener is what wakes accept(); there is no portable
        // cancel for it.
        closesocket(static_cast<SOCKET>(listenSocket_));
        listenSocket_ = kInvalid;
    }
    if (thread_.joinable()) thread_.join();
}

void WebConsole::AcceptLoop() {
    while (running_) {
        const SOCKET client = accept(static_cast<SOCKET>(listenSocket_), nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running_) break;
            continue;
        }
        // Handled inline. A control page serves one browser; concurrency here
        // would buy nothing and add a thread per refresh.
        HandleConnection(static_cast<uintptr_t>(client));
        closesocket(client);
    }
}

void WebConsole::HandleConnection(uintptr_t socketHandle) {
    const SOCKET socket = static_cast<SOCKET>(socketHandle);

    // Read only far enough to see the end of the headers. Requests here carry no
    // body, and the cap stops a malformed one from growing without bound.
    std::string request;
    char buffer[2048];
    while (request.size() < 16 * 1024) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) break;
        request.append(buffer, static_cast<size_t>(received));
        if (request.find("\r\n\r\n") != std::string::npos) break;
    }
    if (request.empty()) return;

    const size_t methodEnd = request.find(' ');
    if (methodEnd == std::string::npos) return;
    const size_t pathEnd = request.find(' ', methodEnd + 1);
    if (pathEnd == std::string::npos) return;

    const std::string method = request.substr(0, methodEnd);
    std::string path = request.substr(methodEnd + 1, pathEnd - methodEnd - 1);
    if (const size_t query = path.find('?'); query != std::string::npos) path.resize(query);
    // The page fetches relative URLs, so both "/status" and "status" arrive here
    // depending on how the browser resolved them.
    if (!path.empty() && path.front() == '/') path.erase(0, 1);

    if (method == "GET" && (path.empty() || path == "index.html")) {
        SendResponse(socket, "200 OK", "text/html; charset=utf-8", kPage);
        return;
    }
    if (method == "GET" && path == "status") {
        SendResponse(socket, "200 OK", "application/json",
                     status ? StatusJson(status()) : "{}");
        return;
    }

    if (method == "POST" && (path == "start" || path == "stop" || path == "restart")) {
        std::string actionError;
        bool ok = true;
        if (path == "start") {
            ok = onStart ? onStart(&actionError) : false;
        } else if (path == "stop") {
            if (onStop) onStop();
        } else {
            ok = onRestart ? onRestart(&actionError) : false;
        }
        const std::string body = ok ? "{\"ok\":true}"
                                    : "{\"ok\":false,\"error\":\"" + JsonEscape(actionError) + "\"}";
        SendResponse(socket, "200 OK", "application/json", body);
        return;
    }

    SendResponse(socket, "404 Not Found", "text/plain", "no such page\n");
}

}  // namespace thorstream
