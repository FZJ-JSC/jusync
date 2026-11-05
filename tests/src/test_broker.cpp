// jusync_test_broker - a small, in-repo ANARI-USD file sender.
//
// The middleware is a ZeroMQ DEALER client; the production sender is the
// ANARI-USD worker/broker pair (UsdBridgeZmqBroker.cpp + UsdDevice.cpp), which
// needs a ParaView build, MPI and a foreign checkout.  This executable speaks
// the same wire protocol with the same framing so the opt-in
// "--mode=file-view-live" path can be exercised anywhere:
//
//   client sends  [empty][ZmqFileRequest]
//   broker replies [identity][empty][ZmqFileChunk / ZmqFileComplete]
//
// Supported requests:
//   REQ_LIST_FILES      -> one RESP_FILE_CHUNK carrying filelist JSON
//   REQ_GET_FILE        -> N RESP_FILE_CHUNK frames + RESP_FILE_COMPLETE
//   REQ_GET_PROPERTY    -> totalWorkerCount (also the client's idle heartbeat)
//
// The broker can run standalone (--serve-seconds) or launch the client itself
// (--run=EXE --run-arg=...) and return its exit code.  The latter is what the
// file_view_live_* CTest entries use: no shell backgrounding, no Python, and
// --port=0 chooses a free port (the real endpoint is substituted for
// "{endpoint}" in --run-arg values), so parallel CTest runs cannot collide.
//
// --chunks=N is a harness-only override: the production sender honours the
// client's requested chunk size, so a whole-file single chunk cannot be forced
// for an arbitrary file.  N=1 exercises the aliased FileView path, N>1 the
// assembled path.

#include "AnariUsdProtocol.h"

#include <zmq.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace anari_usd_protocol;

namespace
{

constexpr size_t kMaxChunkSize = 128ull * 1024ull * 1024ull;

struct Options
{
    std::string dir;
    std::string endpoint;          // explicit endpoint; overrides --port
    int port = 0;                  // 0 -> libzmq picks a free port
    int rank = 0;
    int chunks = 0;                // 0 -> honour the client's chunk_size
    int serveSeconds = 30;
    std::string readyFile;
    std::string runExe;
    std::vector<std::string> runArgs;
    bool verbose = false;
};

struct Server
{
    void* context = nullptr;
    void* socket = nullptr;
    std::string endpoint;
    std::map<std::string, std::vector<uint8_t>> files;
    int rank = 0;
    int chunks = 0;
    bool verbose = false;
};

void usage()
{
    std::cout
        << "Usage: jusync_test_broker --dir=DIR [options]\n"
        << "  --dir=DIR            directory to serve (top-level files)\n"
        << "  --port=N             bind port (0 = any free port, default 0)\n"
        << "  --endpoint=EP        bind this endpoint instead of --port\n"
        << "  --rank=N             source_rank in responses (default 0)\n"
        << "  --chunks=N           split every file into N chunks (0 = client size)\n"
        << "  --serve-seconds=N    standalone lifetime (default 30)\n"
        << "  --ready-file=PATH    write the bound endpoint once listening\n"
        << "  --run=EXE            run EXE once listening, then exit with its code\n"
        << "  --run-arg=ARG        argument for EXE ({endpoint} is substituted)\n"
        << "  --verbose            log every request\n";
}

std::string mimeFor(const std::string& name)
{
    const fs::path p(name);
    std::string ext = p.extension().string();
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext == ".usda") return "text/plain";
    if (ext == ".usdz") return "model/vnd.usdz+zip";
    return "application/octet-stream";
}

std::string jsonEscape(const std::string& in)
{
    std::string out;
    out.reserve(in.size() + 8);
    for (char c : in)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                out += buf;
            }
            else
            {
                out += c;
            }
        }
    }
    return out;
}

std::string buildFileListJson(const Server& server)
{
    std::ostringstream json;
    json << "{\"rank\":" << server.rank << ",\"files\":[";
    bool first = true;
    for (const auto& kv : server.files)
    {
        if (!first) json << ",";
        first = false;
        json << "{\"name\":\"" << jsonEscape(kv.first) << "\""
             << ",\"size\":" << kv.second.size()
             << ",\"mime\":\"" << mimeFor(kv.first) << "\""
             << ",\"hash_lo\":0,\"hash_hi\":0}";
    }
    json << "]}";
    return json.str();
}

bool loadFiles(Server& server, const std::string& dir, std::string& error)
{
    std::error_code ec;
    if (!fs::is_directory(dir, ec))
    {
        error = "not a directory: " + dir;
        return false;
    }

    std::vector<fs::path> entries;
    for (const auto& entry : fs::directory_iterator(dir, ec))
    {
        if (ec) break;
        if (entry.is_regular_file(ec))
        {
            entries.push_back(entry.path());
        }
    }
    if (ec)
    {
        error = "cannot list " + dir + ": " + ec.message();
        return false;
    }

    std::sort(entries.begin(), entries.end());
    for (const auto& path : entries)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in)
        {
            error = "cannot open " + path.string();
            return false;
        }
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
        server.files.emplace(path.filename().string(), std::move(bytes));
    }
    return true;
}

bool sendMultipart(void* socket, const std::string& identity,
                   const void* data, size_t size)
{
    if (zmq_send(socket, identity.data(), identity.size(), ZMQ_SNDMORE) < 0)
    {
        return false;
    }
    if (zmq_send(socket, "", 0, ZMQ_SNDMORE) < 0)
    {
        return false;
    }
    return zmq_send(socket, data, size, 0) >= 0;
}

bool recvMultipart(void* socket, std::string& identity, std::string& payload)
{
    identity.clear();
    payload.clear();
    bool first = true;
    while (true)
    {
        zmq_msg_t msg;
        zmq_msg_init(&msg);
        const int rc = zmq_msg_recv(&msg, socket, 0);
        if (rc < 0)
        {
            zmq_msg_close(&msg);
            return false;
        }
        const char* data = static_cast<const char*>(zmq_msg_data(&msg));
        const size_t size = zmq_msg_size(&msg);
        if (first)
        {
            identity.assign(data, size);
            first = false;
        }
        else if (size > 0)
        {
            payload.assign(data, size);
        }
        const int more = zmq_msg_more(&msg);
        zmq_msg_close(&msg);
        if (!more) break;
    }
    return !identity.empty();
}

void sendChunk(const Server& server, const std::string& identity,
               uint32_t requestId, const std::string& filename,
               const uint8_t* data, size_t size, uint64_t total, uint64_t offset)
{
    ZmqFileChunk header;
    header.request_id = requestId;
    header.source_rank = server.rank;
    header.setFilename(filename);
    header.file_size = total;
    header.chunk_offset = offset;
    header.chunk_size = static_cast<uint32_t>(size);

    std::vector<uint8_t> frame(sizeof(header) + size);
    std::memcpy(frame.data(), &header, sizeof(header));
    if (size > 0)
    {
        std::memcpy(frame.data() + sizeof(header), data, size);
    }
    sendMultipart(server.socket, identity, frame.data(), frame.size());
}

void sendComplete(const Server& server, const std::string& identity,
                  uint32_t requestId, const std::string& filename, uint64_t total)
{
    ZmqFileComplete complete;
    complete.request_id = requestId;
    complete.source_rank = server.rank;
    complete.setFilename(filename);
    complete.total_size = total;
    sendMultipart(server.socket, identity, &complete, sizeof(complete));
}

void sendNoFile(const Server& server, const std::string& identity,
                uint32_t requestId, const std::string& filename)
{
    // Production reuses ZmqFileComplete with RESP_NO_FILE and total_size 0.
    ZmqFileComplete complete;
    complete.message_type = static_cast<uint32_t>(ZmqMessageType::RESP_NO_FILE);
    complete.request_id = requestId;
    complete.source_rank = server.rank;
    complete.setFilename(filename);
    complete.total_size = 0;
    sendMultipart(server.socket, identity, &complete, sizeof(complete));
}

void sendWorkerCount(const Server& server, const std::string& identity,
                     uint32_t requestId)
{
    ZmqPropertyResponse response;
    response.request_id = requestId;
    response.property_type = 0;  // int32
    response.int_value = 1;      // rank 0 only
    sendMultipart(server.socket, identity, &response, sizeof(response));
}

size_t chunkSizeFor(const Server& server, const ZmqFileRequest& request, size_t total)
{
    if (server.chunks > 0 && total > 0)
    {
        const size_t n = static_cast<size_t>(server.chunks);
        return std::max<size_t>(1, (total + n - 1) / n);
    }
    size_t requested = request.chunk_size > 0
        ? static_cast<size_t>(request.chunk_size)
        : static_cast<size_t>(ANARI_USD_DEFAULT_CHUNK_SIZE);
    return std::min(requested, kMaxChunkSize);
}

void handleRequest(Server& server, const std::string& identity, const std::string& payload)
{
    if (payload.size() < sizeof(ZmqFileRequest))
    {
        return;
    }

    ZmqFileRequest request;
    std::memcpy(&request, payload.data(), sizeof(request));
    if (request.magic != ANARI_USD_MAGIC)
    {
        std::cerr << "[test-broker] bad magic from client: 0x" << std::hex
                  << request.magic << std::dec << std::endl;
        return;
    }

    switch (static_cast<ZmqMessageType>(request.message_type))
    {
    case ZmqMessageType::REQ_LIST_FILES:
    {
        const std::string json = buildFileListJson(server);
        sendChunk(server, identity, request.request_id, "__file_list__.json",
                  reinterpret_cast<const uint8_t*>(json.data()), json.size(),
                  json.size(), 0);
        sendComplete(server, identity, request.request_id, "__file_list__.json",
                     json.size());
        if (server.verbose)
        {
            std::cout << "[test-broker] listed " << server.files.size()
                      << " files (" << json.size() << " bytes)" << std::endl;
        }
        break;
    }

    case ZmqMessageType::REQ_GET_FILE:
    {
        const std::string filename = request.getFilename();
        auto it = server.files.find(filename);
        if (it == server.files.end())
        {
            sendNoFile(server, identity, request.request_id, filename);
            if (server.verbose)
            {
                std::cout << "[test-broker] no such file: " << filename << std::endl;
            }
            break;
        }

        const std::vector<uint8_t>& bytes = it->second;
        const size_t total = bytes.size();
        const size_t chunkSize = chunkSizeFor(server, request, total);
        size_t sentChunks = 0;
        for (size_t offset = 0; offset < total; offset += chunkSize)
        {
            const size_t sendSize = std::min(chunkSize, total - offset);
            sendChunk(server, identity, request.request_id, filename,
                      bytes.data() + offset, sendSize, total, offset);
            ++sentChunks;
        }
        sendComplete(server, identity, request.request_id, filename, total);
        if (server.verbose)
        {
            std::cout << "[test-broker] sent " << filename << ": " << total
                      << " bytes in " << sentChunks << " chunk(s)" << std::endl;
        }
        break;
    }

    case ZmqMessageType::REQ_GET_PROPERTY:
    {
        const std::string property = request.getFilename();
        if (property == "totalWorkerCount")
        {
            sendWorkerCount(server, identity, request.request_id);
        }
        if (server.verbose)
        {
            std::cout << "[test-broker] property: " << property << std::endl;
        }
        break;
    }

    default:
        if (server.verbose)
        {
            std::cout << "[test-broker] ignored message type "
                      << request.message_type << std::endl;
        }
        break;
    }
}

void serveLoop(Server& server, const std::atomic<bool>& stop)
{
    while (!stop.load(std::memory_order_acquire))
    {
        zmq_pollitem_t item{server.socket, 0, ZMQ_POLLIN, 0};
        const int rc = zmq_poll(&item, 1, 100);
        if (rc <= 0)
        {
            continue;
        }

        std::string identity, payload;
        if (recvMultipart(server.socket, identity, payload))
        {
            handleRequest(server, identity, payload);
        }
    }
}

std::string quoteArg(const std::string& arg)
{
    std::string quoted = "\"";
    for (char c : arg)
    {
        if (c == '"')
        {
            quoted += "\\\"";
        }
        else
        {
            quoted += c;
        }
    }
    quoted += "\"";
    return quoted;
}

std::string substituteEndpoint(const std::string& arg, const std::string& endpoint)
{
    static const std::string token = "{endpoint}";
    std::string out = arg;
    size_t pos = 0;
    while ((pos = out.find(token, pos)) != std::string::npos)
    {
        out.replace(pos, token.size(), endpoint);
        pos += endpoint.size();
    }
    return out;
}

int runChild(const Options& options, const std::string& endpoint)
{
    std::string command = quoteArg(options.runExe);
    for (const std::string& arg : options.runArgs)
    {
        command += " ";
        command += quoteArg(substituteEndpoint(arg, endpoint));
    }

    if (options.verbose)
    {
        std::cout << "[test-broker] running: " << command << std::endl;
    }

#ifdef _WIN32
    STARTUPINFOA startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::vector<char> mutableCommand(command.begin(), command.end());
    mutableCommand.push_back('\0');
    if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, TRUE,
                        0, nullptr, nullptr, &startup, &process))
    {
        std::cerr << "[test-broker] cannot start client: " << command << std::endl;
        return 127;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
#else
    const int result = std::system(command.c_str());
    if (result == -1)
    {
        return 127;
    }
    return WIFEXITED(result) ? WEXITSTATUS(result) : 1;
#endif
}

bool parseOptions(int argc, char** argv, Options& options, std::string& error)
{
    for (int i = 1; i < argc; ++i)
    {
        const std::string arg = argv[i];
        auto value = [&](const char* prefix) -> std::string {
            return arg.substr(std::strlen(prefix));
        };

        if (arg.rfind("--dir=", 0) == 0)
        {
            options.dir = value("--dir=");
        }
        else if (arg.rfind("--endpoint=", 0) == 0)
        {
            options.endpoint = value("--endpoint=");
        }
        else if (arg.rfind("--port=", 0) == 0)
        {
            options.port = std::atoi(value("--port=").c_str());
        }
        else if (arg.rfind("--rank=", 0) == 0)
        {
            options.rank = std::atoi(value("--rank=").c_str());
        }
        else if (arg.rfind("--chunks=", 0) == 0)
        {
            options.chunks = std::atoi(value("--chunks=").c_str());
        }
        else if (arg.rfind("--serve-seconds=", 0) == 0)
        {
            options.serveSeconds = std::atoi(value("--serve-seconds=").c_str());
        }
        else if (arg.rfind("--ready-file=", 0) == 0)
        {
            options.readyFile = value("--ready-file=");
        }
        else if (arg.rfind("--run=", 0) == 0)
        {
            options.runExe = value("--run=");
        }
        else if (arg.rfind("--run-arg=", 0) == 0)
        {
            options.runArgs.push_back(value("--run-arg="));
        }
        else if (arg == "--verbose")
        {
            options.verbose = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            usage();
            std::exit(0);
        }
        else
        {
            error = "unknown argument: " + arg;
            return false;
        }
    }

    if (options.dir.empty())
    {
        error = "--dir is required";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    Options options;
    std::string error;
    if (!parseOptions(argc, argv, options, error))
    {
        std::cerr << "[test-broker] " << error << std::endl;
        usage();
        return 2;
    }

    Server server;
    server.rank = options.rank;
    server.chunks = options.chunks;
    server.verbose = options.verbose;
    if (!loadFiles(server, options.dir, error))
    {
        std::cerr << "[test-broker] " << error << std::endl;
        return 2;
    }

    server.context = zmq_ctx_new();
    if (!server.context)
    {
        std::cerr << "[test-broker] zmq_ctx_new failed: "
                  << zmq_strerror(zmq_errno()) << std::endl;
        return 2;
    }
    zmq_ctx_set(server.context, ZMQ_IO_THREADS, 1);

    server.socket = zmq_socket(server.context, ZMQ_ROUTER);
    if (!server.socket)
    {
        std::cerr << "[test-broker] zmq_socket failed: "
                  << zmq_strerror(zmq_errno()) << std::endl;
        zmq_ctx_term(server.context);
        return 2;
    }

    const std::string bindEndpoint = !options.endpoint.empty()
        ? options.endpoint
        : ("tcp://127.0.0.1:" +
           (options.port > 0 ? std::to_string(options.port) : std::string("*")));

    if (zmq_bind(server.socket, bindEndpoint.c_str()) != 0)
    {
        std::cerr << "[test-broker] cannot bind " << bindEndpoint << ": "
                  << zmq_strerror(zmq_errno()) << std::endl;
        zmq_close(server.socket);
        zmq_ctx_term(server.context);
        return 2;
    }

    char endpointBuffer[256] = {0};
    size_t endpointSize = sizeof(endpointBuffer) - 1;
    if (zmq_getsockopt(server.socket, ZMQ_LAST_ENDPOINT, endpointBuffer, &endpointSize) != 0)
    {
        std::cerr << "[test-broker] cannot read bound endpoint: "
                  << zmq_strerror(zmq_errno()) << std::endl;
        zmq_close(server.socket);
        zmq_ctx_term(server.context);
        return 2;
    }
    server.endpoint = endpointBuffer;

    if (!options.readyFile.empty())
    {
        std::ofstream ready(options.readyFile, std::ios::binary | std::ios::trunc);
        ready << server.endpoint << "\n";
    }

    std::cout << "[test-broker] listening on " << server.endpoint
              << " (" << server.files.size() << " files, "
              << (options.chunks > 0
                      ? std::to_string(options.chunks) + " chunk(s) per file"
                      : std::string("client-sized chunks"))
              << ")" << std::endl;

    std::atomic<bool> stop{false};

    if (!options.runExe.empty())
    {
        std::thread serverThread([&server, &stop] { serveLoop(server, stop); });
        const int exitCode = runChild(options, server.endpoint);
        stop.store(true, std::memory_order_release);
        serverThread.join();
        zmq_close(server.socket);
        zmq_ctx_term(server.context);
        return exitCode;
    }

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(std::max(1, options.serveSeconds));
    while (!stop.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline)
    {
        zmq_pollitem_t item{server.socket, 0, ZMQ_POLLIN, 0};
        const int rc = zmq_poll(&item, 1, 100);
        if (rc <= 0) continue;
        std::string identity, payload;
        if (recvMultipart(server.socket, identity, payload))
        {
            handleRequest(server, identity, payload);
        }
    }

    zmq_close(server.socket);
    zmq_ctx_term(server.context);
    return 0;
}
