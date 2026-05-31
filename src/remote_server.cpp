#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include "synthetic_dll.h"

#if defined(_WIN32)
#include <windows.h>
#include <objidl.h>
#include <gdiplus.h>

#include <boost/asio.hpp>
#include <boost/beast.hpp>

#define MA_NO_DEVICE_IO
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#pragma warning(push)
#pragma warning(disable: 4244)
#include <miniaudio.h>
#pragma warning(pop)

#include <spdlog/sinks/base_sink.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

#pragma comment(lib, "gdiplus.lib")

namespace {

namespace beast = boost::beast;
namespace http = beast::http;
namespace websocket = beast::websocket;
namespace net = boost::asio;
using tcp = net::ip::tcp;

constexpr const char* kRemoteBoundary = "frame";
constexpr size_t kRemoteAudioChunkDurationMs = 20;
constexpr size_t kMaxRemoteAudioQueuedChunks = 6;

struct RemoteAudioConverterState {
    ma_data_converter converter{};
    bool initialized{};
    uint16_t sourceFormatTag{};
    uint32_t sourceSampleRate{};
    uint16_t sourceChannels{};
    uint16_t sourceBlockAlign{};
    uint16_t sourceBitsPerSample{};
    std::string targetFormatName;
    uint32_t targetSampleRate{};
    uint16_t targetChannels{};
    uint64_t nextSourceStartMs{};
};

std::mutex g_remoteAudioConverterMutex;
std::unordered_map<SyntheticDllRuntime*, RemoteAudioConverterState> g_remoteAudioConverters;

class RemoteLogSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    std::vector<std::string> recent(size_t maxLines) const {
        std::lock_guard<std::mutex> lock(linesMutex_);
        const size_t count = std::min(maxLines, lines_.size());
        return std::vector<std::string>(lines_.end() - static_cast<std::ptrdiff_t>(count), lines_.end());
    }

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        std::string line(formatted.data(), formatted.size());
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        std::lock_guard<std::mutex> lock(linesMutex_);
        lines_.push_back(std::move(line));
        constexpr size_t kMaxLines = 4096;
        if (lines_.size() > kMaxLines) {
            lines_.erase(lines_.begin(), lines_.begin() + static_cast<std::ptrdiff_t>(lines_.size() - kMaxLines));
        }
    }

    void flush_() override {}

private:
    mutable std::mutex linesMutex_;
    std::vector<std::string> lines_;
};

std::shared_ptr<RemoteLogSink> remoteLogSink() {
    static auto sink = std::make_shared<RemoteLogSink>();
    return sink;
}

std::string clampText(std::string value, size_t maxLength) {
    if (value.size() > maxLength) value.resize(maxLength);
    return value;
}

std::string targetPath(const http::request<http::string_body>& req) {
    std::string target(req.target());
    const size_t query = target.find('?');
    if (query != std::string::npos) target.resize(query);
    return target;
}

std::string queryParam(const http::request<http::string_body>& req, const std::string& key) {
    std::string target(req.target());
    const size_t query = target.find('?');
    if (query == std::string::npos) return {};
    std::string_view text(target.data() + query + 1, target.size() - query - 1);
    while (!text.empty()) {
        const size_t amp = text.find('&');
        const std::string_view part = amp == std::string_view::npos ? text : text.substr(0, amp);
        const size_t eq = part.find('=');
        const std::string_view name = eq == std::string_view::npos ? part : part.substr(0, eq);
        if (name == key) {
            const std::string_view value = eq == std::string_view::npos ? std::string_view{} : part.substr(eq + 1);
            return std::string(value);
        }
        if (amp == std::string_view::npos) break;
        text.remove_prefix(amp + 1);
    }
    return {};
}

int queryInt(const http::request<http::string_body>& req, const std::string& key,
             int fallback, int minValue, int maxValue) {
    const std::string value = queryParam(req, key);
    if (value.empty()) return fallback;
    try {
        return std::clamp(std::stoi(value), minValue, maxValue);
    } catch (...) {
        return fallback;
    }
}

bool queryBool(const http::request<http::string_body>& req, const std::string& key) {
    const std::string value = queryParam(req, key);
    return value == "1" || value == "true" || value == "yes" || value == "on";
}

http::response<http::string_body> makeStringResponse(
    const http::request<http::string_body>& req,
    http::status status,
    std::string body,
    const char* contentType) {
    http::response<http::string_body> res{status, req.version()};
    res.set(http::field::server, "iNavi-Unicorn-Remote");
    res.set(http::field::content_type, contentType);
    res.keep_alive(false);
    res.body() = std::move(body);
    res.prepare_payload();
    return res;
}

http::response<http::string_body> makeJsonResponse(
    const http::request<http::string_body>& req,
    http::status status,
    const nlohmann::json& body) {
    return makeStringResponse(req, status, body.dump(), "application/json");
}

void writeResponse(tcp::socket& socket, http::response<http::string_body>&& res) {
    beast::error_code ec;
    http::write(socket, res, ec);
    socket.shutdown(tcp::socket::shutdown_send, ec);
}

int getEncoderClsid(const WCHAR* mimeType, CLSID& clsid) {
    UINT count = 0;
    UINT bytes = 0;
    Gdiplus::GetImageEncodersSize(&count, &bytes);
    if (!count || !bytes) return -1;
    std::vector<uint8_t> buffer(bytes);
    auto* encoders = reinterpret_cast<Gdiplus::ImageCodecInfo*>(buffer.data());
    if (Gdiplus::GetImageEncoders(count, bytes, encoders) != Gdiplus::Ok) return -1;
    for (UINT i = 0; i < count; ++i) {
        if (wcscmp(encoders[i].MimeType, mimeType) == 0) {
            clsid = encoders[i].Clsid;
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<uint8_t> encodeFramebufferImage(
    const std::vector<uint32_t>& pixels,
    int width,
    int height,
    const WCHAR* mimeType,
    int quality) {
    if (pixels.empty() || width <= 0 || height <= 0) return {};
    CLSID clsid{};
    if (getEncoderClsid(mimeType, clsid) < 0) return {};

    Gdiplus::Bitmap bitmap(width, height, width * 4, PixelFormat32bppARGB,
                           reinterpret_cast<BYTE*>(const_cast<uint32_t*>(pixels.data())));
    IStream* stream = nullptr;
    if (CreateStreamOnHGlobal(nullptr, TRUE, &stream) != S_OK || !stream) return {};

    Gdiplus::Status status = Gdiplus::GenericError;
    if (wcscmp(mimeType, L"image/jpeg") == 0) {
        ULONG q = static_cast<ULONG>(std::clamp(quality, 1, 100));
        Gdiplus::EncoderParameters params{};
        params.Count = 1;
        params.Parameter[0].Guid = Gdiplus::EncoderQuality;
        params.Parameter[0].Type = Gdiplus::EncoderParameterValueTypeLong;
        params.Parameter[0].NumberOfValues = 1;
        params.Parameter[0].Value = &q;
        status = bitmap.Save(stream, &clsid, &params);
    } else {
        status = bitmap.Save(stream, &clsid, nullptr);
    }
    if (status != Gdiplus::Ok) {
        stream->Release();
        return {};
    }

    STATSTG stat{};
    if (stream->Stat(&stat, STATFLAG_NONAME) != S_OK || stat.cbSize.QuadPart <= 0) {
        stream->Release();
        return {};
    }
    std::vector<uint8_t> bytes(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    stream->Seek(zero, STREAM_SEEK_SET, nullptr);
    ULONG read = 0;
    stream->Read(bytes.data(), static_cast<ULONG>(bytes.size()), &read);
    stream->Release();
    bytes.resize(read);
    return bytes;
}

std::string binaryString(const std::vector<uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::string nmeaChecksumLine(const std::string& body) {
    uint8_t checksum = 0;
    for (char ch : body) checksum ^= static_cast<uint8_t>(ch);
    std::ostringstream out;
    out << '$' << body << '*' << std::uppercase << std::hex << std::setw(2)
        << std::setfill('0') << static_cast<int>(checksum) << "\r\n";
    return out.str();
}

std::string formatNmeaCoord(double value, bool latitude, char& hemisphere) {
    const double absValue = std::abs(value);
    const int degrees = static_cast<int>(absValue);
    const double minutes = (absValue - degrees) * 60.0;
    hemisphere = latitude ? (value >= 0.0 ? 'N' : 'S') : (value >= 0.0 ? 'E' : 'W');
    std::ostringstream out;
    out << std::setfill('0') << std::setw(latitude ? 2 : 3) << degrees
        << std::fixed << std::setprecision(4) << std::setw(7) << minutes;
    return out.str();
}

std::vector<std::string> makeLocationNmea(const nlohmann::json& body) {
    const double lat = body.at("lat").get<double>();
    const double lon = body.at("lon").get<double>();
    const double altitudeM = body.value("altitudeM", 50.0);
    const double speedMps = body.value("speedMps", 0.0);
    const double bearingDeg = body.value("bearingDeg", 0.0);
    const uint64_t timestampMs = body.value("timestampMs", uint64_t(0));

    auto now = timestampMs
        ? std::chrono::system_clock::time_point(std::chrono::milliseconds(timestampMs))
        : std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm utc{};
    gmtime_s(&utc, &t);

    std::ostringstream timeText;
    timeText << std::setfill('0') << std::setw(2) << utc.tm_hour
             << std::setw(2) << utc.tm_min << std::setw(2) << utc.tm_sec
             << ".000";
    std::ostringstream dateText;
    dateText << std::setfill('0') << std::setw(2) << utc.tm_mday
             << std::setw(2) << (utc.tm_mon + 1)
             << std::setw(2) << (utc.tm_year % 100);

    char ns = 'N';
    char ew = 'E';
    const std::string latText = formatNmeaCoord(lat, true, ns);
    const std::string lonText = formatNmeaCoord(lon, false, ew);
    const double speedKnots = speedMps * 1.9438444924406;
    const double speedKmh = speedMps * 3.6;

    std::ostringstream rmc;
    rmc << "GPRMC," << timeText.str() << ",A," << latText << ',' << ns << ','
        << lonText << ',' << ew << ',' << std::fixed << std::setprecision(1)
        << speedKnots << ',' << bearingDeg << ',' << dateText.str() << ",,,A";

    std::ostringstream gga;
    gga << "GPGGA," << timeText.str() << ',' << latText << ',' << ns << ','
        << lonText << ',' << ew << ",1,08,0.9," << std::fixed << std::setprecision(1)
        << altitudeM << ",M,19.5,M,,";

    std::ostringstream vtg;
    vtg << "GPVTG," << std::fixed << std::setprecision(1) << bearingDeg
        << ",T,,M," << speedKnots << ",N," << speedKmh << ",K,A";

    return {nmeaChecksumLine(rmc.str()), nmeaChecksumLine(gga.str()), nmeaChecksumLine(vtg.str())};
}

std::string normalizeNmeaSentence(std::string sentence) {
    while (!sentence.empty() && (sentence.back() == '\r' || sentence.back() == '\n')) {
        sentence.pop_back();
    }
    sentence += "\r\n";
    return sentence;
}

ma_format remoteAudioFormat(std::string_view name) {
    if (name == "u8") return ma_format_u8;
    if (name == "s16le" || name == "s16") return ma_format_s16;
    if (name == "s24le" || name == "s24") return ma_format_s24;
    if (name == "s32le" || name == "s32") return ma_format_s32;
    if (name == "f32le" || name == "f32") return ma_format_f32;
    return ma_format_unknown;
}

ma_format guestWaveFormat(uint16_t formatTag, uint16_t bitsPerSample) {
    constexpr uint16_t kWaveFormatPcm = 0x0001;
    constexpr uint16_t kWaveFormatIeeeFloat = 0x0003;
    if (formatTag == kWaveFormatPcm) {
        switch (bitsPerSample) {
        case 8: return ma_format_u8;
        case 16: return ma_format_s16;
        case 24: return ma_format_s24;
        case 32: return ma_format_s32;
        default: return ma_format_unknown;
        }
    }
    if (formatTag == kWaveFormatIeeeFloat && bitsPerSample == 32) {
        return ma_format_f32;
    }
    return ma_format_unknown;
}

bool remotePcmFormatsMatch(uint16_t sourceFormatTag,
                           uint32_t sourceSampleRate,
                           uint16_t sourceChannels,
                           uint16_t sourceBlockAlign,
                           uint16_t sourceBitsPerSample,
                           std::string_view targetFormatName,
                           uint32_t targetSampleRate,
                           uint16_t targetChannels) {
    const ma_format sourceFormat = guestWaveFormat(sourceFormatTag, sourceBitsPerSample);
    const ma_format targetFormat = remoteAudioFormat(targetFormatName);
    if (sourceFormat == ma_format_unknown || sourceFormat != targetFormat) return false;
    if (sourceSampleRate != targetSampleRate || sourceChannels != targetChannels) return false;
    const size_t frameBytes = size_t(sourceChannels) * ma_get_bytes_per_sample(sourceFormat);
    return frameBytes != 0 && (!sourceBlockAlign || sourceBlockAlign == frameBytes);
}

std::vector<uint8_t> convertRemotePcm(const std::vector<uint8_t>& pcm,
                                      uint16_t sourceFormatTag,
                                      uint32_t sourceSampleRate,
                                      uint16_t sourceChannels,
                                      uint16_t sourceBlockAlign,
                                      uint16_t sourceBitsPerSample,
                                      std::string_view targetFormatName,
                                      uint32_t targetSampleRate,
                                      uint16_t targetChannels) {
    const ma_format sourceFormat = guestWaveFormat(sourceFormatTag, sourceBitsPerSample);
    const ma_format targetFormat = remoteAudioFormat(targetFormatName);
    if (sourceFormat == ma_format_unknown || targetFormat == ma_format_unknown ||
        !sourceSampleRate || !targetSampleRate || !sourceChannels || !targetChannels) {
        return {};
    }

    const size_t sourceFrameBytes = size_t(sourceChannels) * ma_get_bytes_per_sample(sourceFormat);
    const size_t targetFrameBytes = size_t(targetChannels) * ma_get_bytes_per_sample(targetFormat);
    if (!sourceFrameBytes || !targetFrameBytes) return {};
    if (sourceBlockAlign && sourceBlockAlign != sourceFrameBytes) return {};

    const ma_uint64 sourceFrames = pcm.size() / sourceFrameBytes;
    if (!sourceFrames) return {};

    ma_data_converter_config config = ma_data_converter_config_init(sourceFormat,
                                                                    targetFormat,
                                                                    sourceChannels,
                                                                    targetChannels,
                                                                    sourceSampleRate,
                                                                    targetSampleRate);
    ma_data_converter converter{};
    ma_result result = ma_data_converter_init(&config, nullptr, &converter);
    if (result != MA_SUCCESS) return {};

    ma_uint64 outputFrameCapacity =
        ((sourceFrames * targetSampleRate) + sourceSampleRate - 1) / sourceSampleRate + 16;
    outputFrameCapacity = std::max<ma_uint64>(outputFrameCapacity, 1);

    std::vector<uint8_t> converted(static_cast<size_t>(outputFrameCapacity) * targetFrameBytes);
    ma_uint64 framesIn = sourceFrames;
    ma_uint64 framesOut = outputFrameCapacity;
    result = ma_data_converter_process_pcm_frames(&converter,
                                                  pcm.data(),
                                                  &framesIn,
                                                  converted.data(),
                                                  &framesOut);
    ma_data_converter_uninit(&converter, nullptr);
    if (result != MA_SUCCESS && result != MA_AT_END) return {};

    converted.resize(static_cast<size_t>(framesOut) * targetFrameBytes);
    return converted;
}

uint32_t pcmDurationMs(size_t bytes, uint32_t sampleRate, uint16_t channels, ma_format format) {
    const size_t frameBytes = size_t(channels) * ma_get_bytes_per_sample(format);
    if (!bytes || !frameBytes || !sampleRate) return 1;
    const size_t frames = bytes / frameBytes;
    return static_cast<uint32_t>(std::max<uint64_t>(1, (uint64_t(frames) * 1000ull) / sampleRate));
}

void resetRemoteAudioConverter(SyntheticDllRuntime* runtime) {
    std::lock_guard<std::mutex> lock(g_remoteAudioConverterMutex);
    auto it = g_remoteAudioConverters.find(runtime);
    if (it == g_remoteAudioConverters.end()) return;
    if (it->second.initialized) {
        ma_data_converter_uninit(&it->second.converter, nullptr);
    }
    g_remoteAudioConverters.erase(it);
}

std::vector<uint8_t> convertRemotePcmContinuous(SyntheticDllRuntime* runtime,
                                                const std::vector<uint8_t>& pcm,
                                                uint16_t sourceFormatTag,
                                                uint32_t sourceSampleRate,
                                                uint16_t sourceChannels,
                                                uint16_t sourceBlockAlign,
                                                uint16_t sourceBitsPerSample,
                                                std::string_view targetFormatName,
                                                uint32_t targetSampleRate,
                                                uint16_t targetChannels,
                                                uint64_t sourceStartMs,
                                                uint32_t sourceDurationMs) {
    const ma_format sourceFormat = guestWaveFormat(sourceFormatTag, sourceBitsPerSample);
    const ma_format targetFormat = remoteAudioFormat(targetFormatName);
    if (sourceFormat == ma_format_unknown || targetFormat == ma_format_unknown ||
        !sourceSampleRate || !targetSampleRate || !sourceChannels || !targetChannels) {
        resetRemoteAudioConverter(runtime);
        return {};
    }

    const size_t sourceFrameBytes = size_t(sourceChannels) * ma_get_bytes_per_sample(sourceFormat);
    const size_t targetFrameBytes = size_t(targetChannels) * ma_get_bytes_per_sample(targetFormat);
    if (!sourceFrameBytes || !targetFrameBytes || (sourceBlockAlign && sourceBlockAlign != sourceFrameBytes)) {
        resetRemoteAudioConverter(runtime);
        return {};
    }
    const ma_uint64 sourceFrames = pcm.size() / sourceFrameBytes;
    if (!sourceFrames) return {};

    std::lock_guard<std::mutex> lock(g_remoteAudioConverterMutex);
    RemoteAudioConverterState& state = g_remoteAudioConverters[runtime];
    const std::string targetFormatKey(targetFormatName);
    const bool configChanged =
        !state.initialized ||
        state.sourceFormatTag != sourceFormatTag ||
        state.sourceSampleRate != sourceSampleRate ||
        state.sourceChannels != sourceChannels ||
        state.sourceBlockAlign != sourceBlockAlign ||
        state.sourceBitsPerSample != sourceBitsPerSample ||
        state.targetFormatName != targetFormatKey ||
        state.targetSampleRate != targetSampleRate ||
        state.targetChannels != targetChannels;
    const bool discontinuous =
        state.initialized &&
        (sourceStartMs > state.nextSourceStartMs + 2 ||
         sourceStartMs + 2 < state.nextSourceStartMs);

    if (configChanged || discontinuous) {
        if (state.initialized) ma_data_converter_uninit(&state.converter, nullptr);
        state = {};
        ma_data_converter_config config = ma_data_converter_config_init(sourceFormat,
                                                                        targetFormat,
                                                                        sourceChannels,
                                                                        targetChannels,
                                                                        sourceSampleRate,
                                                                        targetSampleRate);
        if (ma_data_converter_init(&config, nullptr, &state.converter) != MA_SUCCESS) {
            state = {};
            g_remoteAudioConverters.erase(runtime);
            return {};
        }
        state.initialized = true;
        state.sourceFormatTag = sourceFormatTag;
        state.sourceSampleRate = sourceSampleRate;
        state.sourceChannels = sourceChannels;
        state.sourceBlockAlign = sourceBlockAlign;
        state.sourceBitsPerSample = sourceBitsPerSample;
        state.targetFormatName = targetFormatKey;
        state.targetSampleRate = targetSampleRate;
        state.targetChannels = targetChannels;
    }

    ma_uint64 outputFrameCapacity =
        ((sourceFrames * targetSampleRate) + sourceSampleRate - 1) / sourceSampleRate + 32;
    outputFrameCapacity = std::max<ma_uint64>(outputFrameCapacity, 1);
    std::vector<uint8_t> converted(static_cast<size_t>(outputFrameCapacity) * targetFrameBytes);

    ma_uint64 framesIn = sourceFrames;
    ma_uint64 framesOut = outputFrameCapacity;
    const ma_result result = ma_data_converter_process_pcm_frames(&state.converter,
                                                                  pcm.data(),
                                                                  &framesIn,
                                                                  converted.data(),
                                                                  &framesOut);
    if (result != MA_SUCCESS && result != MA_AT_END) {
        ma_data_converter_uninit(&state.converter, nullptr);
        g_remoteAudioConverters.erase(runtime);
        return {};
    }

    converted.resize(static_cast<size_t>(framesOut) * targetFrameBytes);
    state.nextSourceStartMs = sourceStartMs + std::max<uint32_t>(1, sourceDurationMs);
    return converted;
}

} // namespace

void installRemoteLogSink() {
    auto logger = spdlog::default_logger();
    if (!logger) return;
    auto sink = remoteLogSink();
    auto& sinks = logger->sinks();
    if (std::find(sinks.begin(), sinks.end(), sink) == sinks.end()) {
        sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] %v");
        sinks.push_back(std::move(sink));
    }
}

struct RemoteServerHandle {
    SyntheticDllRuntime& runtime;
    SyntheticDllRuntime::RemoteServerConfig config;
    net::io_context ioc;
    tcp::acceptor acceptor;
    std::thread acceptThread;
    std::mutex sessionMutex;
    std::vector<std::thread> sessions;
    std::atomic<bool> stopping{false};
    ULONG_PTR gdiplusToken{};

    RemoteServerHandle(SyntheticDllRuntime& runtime_,
                       SyntheticDllRuntime::RemoteServerConfig config_)
        : runtime(runtime_), config(std::move(config_)), ioc(1), acceptor(ioc) {}

    ~RemoteServerHandle() {
        stop();
    }

    bool authenticated(const http::request<http::string_body>& req) const {
        if (config.token.empty()) return true;
        const std::string expected = "Bearer " + config.token;
        return std::string(req[http::field::authorization]) == expected;
    }

    void start() {
        Gdiplus::GdiplusStartupInput gdiplusInput{};
        if (Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusInput, nullptr) != Gdiplus::Ok) {
            throw std::runtime_error("GDI+ startup failed for remote server");
        }

        beast::error_code ec;
        const auto address = net::ip::make_address(config.bind, ec);
        if (ec) throw std::runtime_error("invalid remote bind address: " + config.bind);
        tcp::endpoint endpoint(address, config.port);
        acceptor.open(endpoint.protocol(), ec);
        if (ec) throw std::runtime_error("remote acceptor open failed: " + ec.message());
        acceptor.set_option(net::socket_base::reuse_address(true), ec);
        acceptor.bind(endpoint, ec);
        if (ec) throw std::runtime_error("remote bind failed: " + ec.message());
        acceptor.listen(net::socket_base::max_listen_connections, ec);
        if (ec) throw std::runtime_error("remote listen failed: " + ec.message());

        acceptThread = std::thread([this] { acceptLoop(); });
        spdlog::info("remote server listening on {}:{}", config.bind, config.port);
    }

    void stop() {
        if (stopping.exchange(true)) return;
        beast::error_code ec;
        acceptor.close(ec);
        ioc.stop();
        if (acceptThread.joinable()) acceptThread.join();
        {
            std::lock_guard<std::mutex> lock(sessionMutex);
            for (auto& session : sessions) {
                if (session.joinable()) session.join();
            }
            sessions.clear();
        }
        if (gdiplusToken) {
            Gdiplus::GdiplusShutdown(gdiplusToken);
            gdiplusToken = 0;
        }
    }

    void acceptLoop() {
        while (!stopping.load()) {
            beast::error_code ec;
            tcp::socket socket(ioc);
            acceptor.accept(socket, ec);
            if (ec) {
                if (!stopping.load()) spdlog::warn("remote accept failed: {}", ec.message());
                continue;
            }
            std::lock_guard<std::mutex> lock(sessionMutex);
            sessions.emplace_back([this, socket = std::move(socket)]() mutable {
                handleSession(std::move(socket));
            });
        }
    }

    void handleSession(tcp::socket socket) {
        beast::error_code ec;
        beast::flat_buffer buffer;
        http::request<http::string_body> req;
        http::read(socket, buffer, req, ec);
        if (ec) return;
        const std::string path = targetPath(req);

        if (!authenticated(req)) {
            writeResponse(socket, makeJsonResponse(req, http::status::unauthorized,
                                                   {{"ok", false}, {"error", "unauthorized"}}));
            return;
        }

        if (websocket::is_upgrade(req)) {
            if (path == "/api/v1/audio/ws") {
                handleAudioWebSocket(std::move(socket), req);
                return;
            }
            if (path == "/api/v1/control/ws") {
                handleControlWebSocket(std::move(socket), req);
                return;
            }
            writeResponse(socket, makeJsonResponse(req, http::status::not_found,
                                                   {{"ok", false}, {"error", "not found"}}));
            return;
        }

        if (req.method() == http::verb::get && path == "/api/v1/status") {
            writeResponse(socket, makeJsonResponse(req, http::status::ok, runtime.remoteStatusJson()));
        } else if (req.method() == http::verb::get && path == "/api/v1/frame.jpg") {
            handleFrame(socket, req, false);
        } else if (req.method() == http::verb::get && path == "/api/v1/debug/screenshot.png") {
            handleFrame(socket, req, true);
        } else if (req.method() == http::verb::get && path == "/api/v1/video.mjpg") {
            handleMjpeg(socket, req);
        } else if (req.method() == http::verb::post && path == "/api/v1/input/touch") {
            handleTouch(socket, req);
        } else if (req.method() == http::verb::post && path == "/api/v1/input/key") {
            handleKey(socket, req);
        } else if (req.method() == http::verb::post && path == "/api/v1/sensors/location") {
            handleLocation(socket, req);
        } else if (req.method() == http::verb::post && path == "/api/v1/sensors/nmea") {
            handleNmea(socket, req);
        } else if (req.method() == http::verb::post && path == "/api/v1/sensors/imu") {
            const auto body = nlohmann::json::parse(req.body(), nullptr, false);
            if (!body.is_discarded()) runtime.updateRemoteImuState(body);
            writeResponse(socket, makeJsonResponse(req, http::status::ok, {{"ok", true}}));
        } else if (req.method() == http::verb::get && path == "/api/v1/logs/recent") {
            const int lines = queryInt(req, "lines", 200, 1, 4096);
            writeResponse(socket, makeJsonResponse(req, http::status::ok,
                                                   {{"ok", true}, {"lines", runtime.recentRemoteLogLines(lines)}}));
        } else if (req.method() == http::verb::post && path == "/api/v1/control/pause") {
            {
                std::lock_guard<std::mutex> lock(runtime.ceRemote_.mutex());
                runtime.ceRemote_.paused() = true;
            }
            writeResponse(socket, makeJsonResponse(req, http::status::ok, {{"ok", true}, {"paused", true}}));
        } else if (req.method() == http::verb::post && path == "/api/v1/control/resume") {
            {
                std::lock_guard<std::mutex> lock(runtime.ceRemote_.mutex());
                runtime.ceRemote_.paused() = false;
            }
            writeResponse(socket, makeJsonResponse(req, http::status::ok, {{"ok", true}, {"paused", false}}));
        } else {
            writeResponse(socket, makeJsonResponse(req, http::status::not_found,
                                                   {{"ok", false}, {"error", "not found"}}));
        }
    }

    std::vector<uint8_t> latestImage(bool png, int quality) {
        int width = 0;
        int height = 0;
        auto pixels = runtime.copyRemoteFramebuffer(width, height);
        return encodeFramebufferImage(pixels, width, height, png ? L"image/png" : L"image/jpeg", quality);
    }

    void handleFrame(tcp::socket& socket, const http::request<http::string_body>& req, bool png) {
        const int quality = queryInt(req, "quality", config.jpegQuality, 1, 100);
        auto bytes = latestImage(png, quality);
        if (bytes.empty()) {
            writeResponse(socket, makeJsonResponse(req, http::status::service_unavailable,
                                                   {{"ok", false}, {"error", "no framebuffer"}}));
            return;
        }
        writeResponse(socket, makeStringResponse(req, http::status::ok, binaryString(bytes),
                                                 png ? "image/png" : "image/jpeg"));
    }

    void handleMjpeg(tcp::socket& socket, const http::request<http::string_body>& req) {
        const int fps = queryInt(req, "fps", config.videoFps, 1, 60);
        const int quality = queryInt(req, "quality", config.jpegQuality, 1, 100);
        http::response<http::empty_body> res{http::status::ok, req.version()};
        res.set(http::field::server, "iNavi-Unicorn-Remote");
        res.set(http::field::content_type, "multipart/x-mixed-replace; boundary=frame");
        res.keep_alive(false);
        http::response_serializer<http::empty_body> serializer{res};
        beast::error_code ec;
        http::write_header(socket, serializer, ec);
        if (ec) return;

        const auto frameDelay = std::chrono::milliseconds(std::max(1, 1000 / fps));
        while (!stopping.load()) {
            auto bytes = latestImage(false, quality);
            if (bytes.empty()) break;
            std::ostringstream header;
            header << "--" << kRemoteBoundary << "\r\n"
                   << "Content-Type: image/jpeg\r\n"
                   << "Content-Length: " << bytes.size() << "\r\n\r\n";
            const std::string head = header.str();
            const std::string tail = "\r\n";
            std::array<net::const_buffer, 3> buffers{
                net::buffer(head),
                net::buffer(bytes),
                net::buffer(tail),
            };
            net::write(socket, buffers, ec);
            if (ec) break;
            std::this_thread::sleep_for(frameDelay);
        }
        socket.shutdown(tcp::socket::shutdown_send, ec);
    }

    void handleTouch(tcp::socket& socket, const http::request<http::string_body>& req) {
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("type") || !body.contains("x") || !body.contains("y")) {
            writeResponse(socket, makeJsonResponse(req, http::status::bad_request,
                                                   {{"ok", false}, {"error", "invalid touch body"}}));
            return;
        }
        std::string error;
        const bool ok = runtime.enqueueRemoteTouch(body.value("type", std::string{}),
                                                   body.value("x", 0),
                                                   body.value("y", 0),
                                                   error);
        writeResponse(socket, makeJsonResponse(req, ok ? http::status::ok : http::status::bad_request,
                                               ok ? nlohmann::json{{"ok", true}}
                                                  : nlohmann::json{{"ok", false}, {"error", error}}));
    }

    void handleKey(tcp::socket& socket, const http::request<http::string_body>& req) {
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("type") || !body.contains("vk")) {
            writeResponse(socket, makeJsonResponse(req, http::status::bad_request,
                                                   {{"ok", false}, {"error", "invalid key body"}}));
            return;
        }
        std::string error;
        const bool ok = runtime.enqueueRemoteKey(body.value("type", std::string{}),
                                                 body.value("vk", uint32_t(0)),
                                                 error);
        writeResponse(socket, makeJsonResponse(req, ok ? http::status::ok : http::status::bad_request,
                                               ok ? nlohmann::json{{"ok", true}}
                                                  : nlohmann::json{{"ok", false}, {"error", error}}));
    }

    void handleLocation(tcp::socket& socket, const http::request<http::string_body>& req) {
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("lat") || !body.contains("lon")) {
            writeResponse(socket, makeJsonResponse(req, http::status::bad_request,
                                                   {{"ok", false}, {"error", "invalid location body"}}));
            return;
        }
        try {
            const auto sentences = makeLocationNmea(body);
            std::string data;
            for (const auto& sentence : sentences) data += sentence;
            runtime.injectRemoteSerialBytes(data);
            writeResponse(socket, makeJsonResponse(req, http::status::ok,
                                                   {{"ok", true}, {"sentencesGenerated", sentences.size()}}));
        } catch (const std::exception& e) {
            writeResponse(socket, makeJsonResponse(req, http::status::bad_request,
                                                   {{"ok", false}, {"error", e.what()}}));
        }
    }

    void handleNmea(tcp::socket& socket, const http::request<http::string_body>& req) {
        const auto body = nlohmann::json::parse(req.body(), nullptr, false);
        if (body.is_discarded() || !body.contains("sentences") || !body["sentences"].is_array()) {
            writeResponse(socket, makeJsonResponse(req, http::status::bad_request,
                                                   {{"ok", false}, {"error", "invalid nmea body"}}));
            return;
        }
        std::string data;
        uint32_t accepted = 0;
        for (const auto& item : body["sentences"]) {
            if (!item.is_string()) continue;
            data += normalizeNmeaSentence(item.get<std::string>());
            ++accepted;
        }
        if (!data.empty()) runtime.injectRemoteSerialBytes(data);
        writeResponse(socket, makeJsonResponse(req, http::status::ok,
                                               {{"ok", true}, {"accepted", accepted}}));
    }

    nlohmann::json dispatchControlMessage(const nlohmann::json& message) {
        if (!message.is_object() || !message.contains("type")) {
            return {{"type", "error"}, {"ok", false}, {"error", "invalid control message"}};
        }
        const std::string type = message.value("type", std::string{});
        try {
            if (type == "touch") {
                std::string error;
                const bool ok = runtime.enqueueRemoteTouch(message.value("phase", std::string{}),
                                                           message.value("x", 0),
                                                           message.value("y", 0),
                                                           error);
                return ok ? nlohmann::json{{"type", "status"}, {"ok", true}, {"status", runtime.remoteStatusJson()}}
                          : nlohmann::json{{"type", "error"}, {"ok", false}, {"error", error}};
            }
            if (type == "key") {
                std::string error;
                const bool ok = runtime.enqueueRemoteKey(message.value("phase", std::string{}),
                                                         message.value("vk", uint32_t(0)),
                                                         error);
                return ok ? nlohmann::json{{"type", "status"}, {"ok", true}, {"status", runtime.remoteStatusJson()}}
                          : nlohmann::json{{"type", "error"}, {"ok", false}, {"error", error}};
            }
            if (type == "location") {
                const auto sentences = makeLocationNmea(message);
                std::string data;
                for (const auto& sentence : sentences) data += sentence;
                runtime.injectRemoteSerialBytes(data);
                return {{"type", "status"}, {"ok", true}, {"sentencesGenerated", sentences.size()}};
            }
            if (type == "nmea") {
                if (!message.contains("sentences") || !message["sentences"].is_array()) {
                    return {{"type", "error"}, {"ok", false}, {"error", "invalid nmea message"}};
                }
                std::string data;
                uint32_t accepted = 0;
                for (const auto& item : message["sentences"]) {
                    if (!item.is_string()) continue;
                    data += normalizeNmeaSentence(item.get<std::string>());
                    ++accepted;
                }
                if (!data.empty()) runtime.injectRemoteSerialBytes(data);
                return {{"type", "status"}, {"ok", true}, {"accepted", accepted}};
            }
            if (type == "imu") {
                runtime.updateRemoteImuState(message);
                return {{"type", "status"}, {"ok", true}};
            }
            if (type == "pause") {
                {
                    std::lock_guard<std::mutex> lock(runtime.ceRemote_.mutex());
                    runtime.ceRemote_.paused() = true;
                }
                return {{"type", "status"}, {"ok", true}, {"paused", true}};
            }
            if (type == "resume") {
                {
                    std::lock_guard<std::mutex> lock(runtime.ceRemote_.mutex());
                    runtime.ceRemote_.paused() = false;
                }
                return {{"type", "status"}, {"ok", true}, {"paused", false}};
            }
            if (type == "status") {
                return {{"type", "status"}, {"ok", true}, {"status", runtime.remoteStatusJson()}};
            }
            if (type == "logs") {
                const size_t lines = std::clamp<size_t>(message.value("lines", 200), 1, 4096);
                return {{"type", "log"}, {"ok", true}, {"lines", runtime.recentRemoteLogLines(lines)}};
            }
        } catch (const std::exception& e) {
            return {{"type", "error"}, {"ok", false}, {"error", e.what()}};
        }
        return {{"type", "error"}, {"ok", false}, {"error", "unsupported control message type"}};
    }

    void handleAudioWebSocket(tcp::socket socket, const http::request<http::string_body>& req) {
        beast::error_code ec;
        socket.set_option(tcp::no_delay(true), ec);
        websocket::stream<tcp::socket> ws(std::move(socket));
        ws.accept(req, ec);
        if (ec) return;
        ws.next_layer().set_option(tcp::no_delay(true), ec);
        struct AudioClientRegistration {
            SyntheticDllRuntime& runtime;
            explicit AudioClientRegistration(SyntheticDllRuntime& owner) : runtime(owner) {
                runtime.registerRemoteAudioClient();
            }
            ~AudioClientRegistration() {
                runtime.unregisterRemoteAudioClient();
            }
        } audioClient{runtime};
        ws.binary(true);
        const int chunkMs = queryInt(req, "chunkMs", 20, 5, 250);
        while (!stopping.load()) {
            runtime.waitForRemoteAudioChunks(static_cast<uint32_t>(chunkMs));
            const size_t available = ws.next_layer().available(ec);
            if (ec) return;
            if (available) {
                beast::flat_buffer input;
                ws.read(input, ec);
                if (ec) return;
            }
            auto chunks = runtime.takeRemoteAudioChunks(1);
            for (auto& chunk : chunks) {
                ws.binary(true);
                ws.write(net::buffer(chunk.payload), ec);
                if (ec) return;
            }
            if (!chunks.empty()) {
                const uint32_t paceMs = std::clamp<uint32_t>(chunks.back().durationMs, 1, 250);
                std::this_thread::sleep_for(std::chrono::milliseconds(paceMs));
            }
        }
        ws.close(websocket::close_code::normal, ec);
    }

    void handleControlWebSocket(tcp::socket socket, const http::request<http::string_body>& req) {
        websocket::stream<tcp::socket> ws(std::move(socket));
        beast::error_code ec;
        ws.accept(req, ec);
        if (ec) return;
        ws.text(true);
        ws.write(net::buffer(nlohmann::json{{"type", "status"},
                                            {"ok", true},
                                            {"status", runtime.remoteStatusJson()}}.dump()), ec);
        if (ec) return;

        while (!stopping.load()) {
            const size_t available = ws.next_layer().available(ec);
            if (ec) return;
            if (!available) {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                continue;
            }
            beast::flat_buffer buffer;
            ws.read(buffer, ec);
            if (ec) return;
            std::string text = beast::buffers_to_string(buffer.data());
            auto request = nlohmann::json::parse(text, nullptr, false);
            nlohmann::json response = request.is_discarded()
                ? nlohmann::json{{"type", "error"}, {"ok", false}, {"error", "invalid json"}}
                : dispatchControlMessage(request);
            ws.text(true);
            ws.write(net::buffer(response.dump()), ec);
            if (ec) return;
        }
        ws.close(websocket::close_code::normal, ec);
    }
};

void RemoteServerHandleDeleter::operator()(RemoteServerHandle* handle) const {
    delete handle;
}

SyntheticDllRuntime::~SyntheticDllRuntime() {
    stopHostAudioBackend();
    stopRemoteServer();
}

void SyntheticDllRuntime::setRemoteServerConfig(RemoteServerConfig config) {
    stopRemoteServer();
    remoteConfig_ = std::move(config);
    if (remoteConfig_.enabled) startRemoteServer();
}

void SyntheticDllRuntime::startRemoteServer() {
    if (remoteServer_ || !remoteConfig_.enabled) return;
    try {
        remoteServer_.reset(new RemoteServerHandle(*this, remoteConfig_));
        remoteServer_->start();
    } catch (const std::exception& e) {
        spdlog::error("remote server failed to start: {}", e.what());
        remoteServer_.reset();
    }
}

void SyntheticDllRuntime::stopRemoteServer() {
    if (!remoteServer_) return;
    remoteServer_->stop();
    remoteServer_.reset();
}

void SyntheticDllRuntime::drainRemoteInputEvents() {
    std::deque<RemoteTouchEvent> events;
    std::deque<RemoteKeyEvent> keyEvents;
    {
        std::lock_guard<std::mutex> lock(ceRemote_.mutex());
        events.swap(ceRemote_.touchEvents());
        keyEvents.swap(ceRemote_.keyEvents());
    }
    for (const auto& event : events) {
        queueHostMouseMessage(hostPresenterGuestHwnd_, event.message, event.x, event.y);
    }
    for (const auto& event : keyEvents) {
        uint32_t hwnd = focusedWindow_;
        auto focused = ceGwe_.windows().find(hwnd);
        if (!hwnd || focused == ceGwe_.windows().end() || focused->second.destroyed || !focused->second.enabled) {
            hwnd = hostPresenterGuestHwnd_;
        }
        if (!hwnd) continue;
        GuestMessage guest{};
        guest.hwnd = hwnd;
        guest.message = event.message;
        guest.wParam = event.vk;
        guest.lParam = 1;
        guest.time = uint32_t(++tick_ * 16);
        ceGwe_.postInputMessage(guest);
        lastHostInputQueuedAt_ = std::chrono::steady_clock::now();
        spdlog::info("queued remote key msg=0x{:04x} hwnd=0x{:08x} vk=0x{:02x} queued={}",
                     event.message, hwnd, event.vk, ceGwe_.messageCount());
        uc_emu_stop(uc_);
    }
}

bool SyntheticDllRuntime::enqueueRemoteTouch(const std::string& phase, int32_t x, int32_t y, std::string& error) {
    uint32_t message = 0;
    if (phase == "down") {
        message = 0x0201;
    } else if (phase == "move") {
        message = 0x0200;
    } else if (phase == "up" || phase == "cancel") {
        message = 0x0202;
    } else {
        error = "unsupported touch type";
        return false;
    }
    if (x < 0 || y < 0 || x >= framebufferWidth_ || y >= framebufferHeight_) {
        error = "touch point outside guest framebuffer";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(ceRemote_.mutex());
        ceRemote_.touchEvents().push_back({message, x, y});
    }
    return true;
}

bool SyntheticDllRuntime::enqueueRemoteKey(const std::string& phase, uint32_t vk, std::string& error) {
    uint32_t message = 0;
    if (phase == "down") {
        message = 0x0100;
    } else if (phase == "up") {
        message = 0x0101;
    } else {
        error = "unsupported key type";
        return false;
    }
    if (!vk || vk > 0xff) {
        error = "vk must be between 1 and 255";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(ceRemote_.mutex());
        ceRemote_.keyEvents().push_back({message, vk});
    }
    return true;
}

void SyntheticDllRuntime::updateRemoteImuState(const nlohmann::json& state) {
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    ceRemote_.imuState() = state;
}

void SyntheticDllRuntime::injectRemoteSerialBytes(const std::string& bytes) {
    if (bytes.empty()) return;
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    for (char ch : bytes) ceRemote_.serialBytes().push_back(static_cast<uint8_t>(ch));
    constexpr size_t kMaxRemoteSerialBytes = 64 * 1024;
    while (ceRemote_.serialBytes().size() > kMaxRemoteSerialBytes) ceRemote_.serialBytes().pop_front();
    spdlog::info("remote injected serial bytes={} queued={}", bytes.size(), ceRemote_.serialBytes().size());
}

size_t SyntheticDllRuntime::readRemoteSerialBytes(uint8_t* dst, size_t maxBytes) {
    if (!dst || !maxBytes) return 0;
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    const size_t count = std::min(maxBytes, ceRemote_.serialBytes().size());
    for (size_t i = 0; i < count; ++i) {
        dst[i] = ceRemote_.serialBytes().front();
        ceRemote_.serialBytes().pop_front();
    }
    return count;
}

size_t SyntheticDllRuntime::remoteSerialByteCount() const {
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    return ceRemote_.serialBytes().size();
}

void SyntheticDllRuntime::publishRemoteAudioChunk(const std::vector<uint8_t>& pcm,
                                                  uint16_t sourceFormatTag,
                                                  uint32_t sourceSampleRate,
                                                  uint16_t sourceChannels,
                                                  uint16_t sourceBlockAlign,
                                                  uint16_t sourceBitsPerSample) {
    if (pcm.empty()) return;

    uint32_t targetSampleRate = 0;
    uint16_t targetChannels = 0;
    std::string targetFormatName;
    {
        std::lock_guard<std::mutex> lock(ceRemote_.mutex());
        if (!remoteConfig_.audioEnabled || ceRemote_.audioClientCount() == 0) return;
        targetSampleRate = static_cast<uint32_t>(std::max(1, remoteConfig_.audioSampleRate));
        targetChannels = static_cast<uint16_t>(std::max(1, remoteConfig_.audioChannels));
        targetFormatName = remoteConfig_.audioFormat;
    }

    const ma_format targetFormat = remoteAudioFormat(targetFormatName);
    const size_t targetSampleBytes = ma_get_bytes_per_sample(targetFormat);
    if (targetFormat == ma_format_unknown || !targetSampleBytes) return;

    std::vector<uint8_t> converted = convertRemotePcm(pcm,
                                                      sourceFormatTag,
                                                      sourceSampleRate,
                                                      sourceChannels,
                                                      sourceBlockAlign,
                                                      sourceBitsPerSample,
                                                      targetFormatName,
                                                      targetSampleRate,
                                                      targetChannels);
    const bool exactRemoteFormat = remotePcmFormatsMatch(sourceFormatTag,
                                                        sourceSampleRate,
                                                        sourceChannels,
                                                        sourceBlockAlign,
                                                        sourceBitsPerSample,
                                                        targetFormatName,
                                                        targetSampleRate,
                                                        targetChannels);
    const std::vector<uint8_t>& payload = converted.empty() && exactRemoteFormat ? pcm : converted;
    if (payload.empty()) return;
    const size_t remoteFrameBytes = size_t(targetChannels) * targetSampleBytes;
    const size_t chunkBytes = std::max<size_t>(
        remoteFrameBytes,
        (size_t(targetSampleRate) * remoteFrameBytes * kRemoteAudioChunkDurationMs) / 1000u);

    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    if (ceRemote_.audioClientCount() == 0) {
        ceRemote_.audioChunks().clear();
        return;
    }
    const bool wasEmpty = ceRemote_.audioChunks().empty();
    for (size_t offset = 0; offset < payload.size(); offset += chunkBytes) {
        const size_t count = std::min(chunkBytes, payload.size() - offset);
        RemoteAudioChunk chunk;
        chunk.payload.assign(payload.begin() + offset, payload.begin() + offset + count);
        chunk.sequence = ++ceRemote_.audioSequence();
        chunk.ptsMs = ceRemote_.audioNextPtsMs();
        chunk.durationMs = static_cast<uint32_t>(
            std::max<uint64_t>(1, (uint64_t(count / remoteFrameBytes) * 1000ull) / targetSampleRate));
        ceRemote_.audioNextPtsMs() += chunk.durationMs;
        ceRemote_.audioChunks().push_back(std::move(chunk));
    }
    while (ceRemote_.audioChunks().size() > kMaxRemoteAudioQueuedChunks) ceRemote_.audioChunks().pop_front();
    if (wasEmpty && !ceRemote_.audioChunks().empty()) ceRemote_.audioCv().notify_all();
}

bool SyntheticDllRuntime::materializeRemoteAudioChunkLocked(uint32_t durationMs) {
    if (!remoteConfig_.audioEnabled || ceRemote_.audioClientCount() == 0) return false;

    const uint64_t nowMs = GetTickCount64();
    if (!ceRemote_.audioNextPtsMs() || ceRemote_.audioNextPtsMs() + 250 < nowMs) {
        ceRemote_.audioNextPtsMs() = nowMs;
    }

    const std::optional<CeAudio::LiveSlice> slice =
        ceAudio_.liveSlice(ceRemote_.audioNextPtsMs(), std::max<uint32_t>(1, durationMs));
    if (!slice || slice->pcm.empty()) return false;

    const uint32_t targetSampleRate = static_cast<uint32_t>(std::max(1, remoteConfig_.audioSampleRate));
    const uint16_t targetChannels = static_cast<uint16_t>(std::max(1, remoteConfig_.audioChannels));
    const std::string targetFormatName = remoteConfig_.audioFormat;
    const ma_format targetFormat = remoteAudioFormat(targetFormatName);
    const size_t targetSampleBytes = ma_get_bytes_per_sample(targetFormat);
    if (targetFormat == ma_format_unknown || !targetSampleBytes) return false;

    const bool exactRemoteFormat = remotePcmFormatsMatch(slice->format.formatTag,
                                                        slice->format.samplesPerSec,
                                                        slice->format.channels,
                                                        slice->format.blockAlign,
                                                        slice->format.bitsPerSample,
                                                        targetFormatName,
                                                        targetSampleRate,
                                                        targetChannels);
    std::vector<uint8_t> converted;
    if (exactRemoteFormat) {
        resetRemoteAudioConverter(this);
    } else {
        converted = convertRemotePcmContinuous(this,
                                               slice->pcm,
                                               slice->format.formatTag,
                                               slice->format.samplesPerSec,
                                               slice->format.channels,
                                               slice->format.blockAlign,
                                               slice->format.bitsPerSample,
                                               targetFormatName,
                                               targetSampleRate,
                                               targetChannels,
                                               slice->startMs,
                                               slice->durationMs);
    }
    const std::vector<uint8_t>& payload = converted.empty() && exactRemoteFormat ? slice->pcm : converted;
    if (payload.empty()) return false;

    RemoteAudioChunk chunk;
    chunk.payload = payload;
    chunk.sequence = ++ceRemote_.audioSequence();
    chunk.ptsMs = ceRemote_.audioNextPtsMs();
    chunk.durationMs = pcmDurationMs(chunk.payload.size(), targetSampleRate, targetChannels, targetFormat);
    ceRemote_.audioNextPtsMs() = slice->startMs + std::max<uint32_t>(1, slice->durationMs);
    ceRemote_.audioChunks().push_back(std::move(chunk));
    while (ceRemote_.audioChunks().size() > kMaxRemoteAudioQueuedChunks) ceRemote_.audioChunks().pop_front();
    return true;
}

void SyntheticDllRuntime::registerRemoteAudioClient() {
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    if (ceRemote_.audioClientCount() == 0) {
        ceRemote_.audioChunks().clear();
        ceRemote_.audioNextPtsMs() = GetTickCount64();
        resetRemoteAudioConverter(this);
    }
    ++ceRemote_.audioClientCount();
    materializeRemoteAudioChunkLocked(kRemoteAudioChunkDurationMs);
    ceRemote_.audioCv().notify_all();
}

void SyntheticDllRuntime::unregisterRemoteAudioClient() {
    {
        std::lock_guard<std::mutex> lock(ceRemote_.mutex());
        if (ceRemote_.audioClientCount() > 0) --ceRemote_.audioClientCount();
        if (ceRemote_.audioClientCount() == 0) {
            ceRemote_.audioChunks().clear();
            ceRemote_.audioNextPtsMs() = 0;
            resetRemoteAudioConverter(this);
        }
    }
    ceRemote_.audioCv().notify_all();
}

void SyntheticDllRuntime::clearRemoteAudioChunks() {
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    ceRemote_.audioChunks().clear();
    ceRemote_.audioNextPtsMs() = GetTickCount64();
    resetRemoteAudioConverter(this);
}

bool SyntheticDllRuntime::waitForRemoteAudioChunks(uint32_t timeoutMs) {
    std::unique_lock<std::mutex> lock(ceRemote_.mutex());
    if (ceRemote_.audioChunks().empty()) materializeRemoteAudioChunkLocked(kRemoteAudioChunkDurationMs);
    if (!ceRemote_.audioChunks().empty() || ceRemote_.audioClientCount() == 0) return !ceRemote_.audioChunks().empty();
    ceRemote_.audioCv().wait_for(lock,
                            std::chrono::milliseconds(std::max<uint32_t>(1, timeoutMs)),
                            [&] {
                                if (ceRemote_.audioChunks().empty()) {
                                    materializeRemoteAudioChunkLocked(kRemoteAudioChunkDurationMs);
                                }
                                return !ceRemote_.audioChunks().empty() || ceRemote_.audioClientCount() == 0;
                            });
    return !ceRemote_.audioChunks().empty();
}

std::vector<SyntheticDllRuntime::RemoteAudioChunk> SyntheticDllRuntime::takeRemoteAudioChunks(size_t maxChunks) {
    std::vector<RemoteAudioChunk> chunks;
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    if (ceRemote_.audioChunks().empty()) materializeRemoteAudioChunkLocked(kRemoteAudioChunkDurationMs);
    while (!ceRemote_.audioChunks().empty() && chunks.size() < maxChunks) {
        chunks.push_back(std::move(ceRemote_.audioChunks().front()));
        ceRemote_.audioChunks().pop_front();
    }
    return chunks;
}

std::vector<uint32_t> SyntheticDllRuntime::copyRemoteFramebuffer(int& width, int& height) const {
    width = framebufferWidth_;
    height = framebufferHeight_;
    if (!framebuffer_ || width <= 0 || height <= 0) return {};
    return std::vector<uint32_t>(framebuffer_, framebuffer_ + (size_t(width) * size_t(height)));
}

nlohmann::json SyntheticDllRuntime::remoteStatusJson() const {
    std::lock_guard<std::mutex> lock(ceRemote_.mutex());
    return {
        {"running", true},
        {"guestWidth", framebufferWidth_},
        {"guestHeight", framebufferHeight_},
        {"guestFps", remoteConfig_.videoFps},
        {"videoEnabled", remoteConfig_.enabled},
        {"videoCodec", "mjpeg"},
        {"audioEnabled", remoteConfig_.audioEnabled},
        {"audioCodec", "pcm"},
        {"audioSampleRate", remoteConfig_.audioSampleRate},
        {"audioChannels", remoteConfig_.audioChannels},
        {"audioFormat", remoteConfig_.audioFormat},
        {"gpsEnabled", true},
        {"gpsTarget", remoteGpsTarget()},
        {"paused", ceRemote_.paused()},
    };
}

std::vector<std::string> SyntheticDllRuntime::recentRemoteLogLines(size_t maxLines) const {
    return remoteLogSink()->recent(std::clamp<size_t>(maxLines, 1, 4096));
}

std::string SyntheticDllRuntime::remoteGpsTarget() const {
    std::string fallback;
    for (const auto& [guest, device] : serialDevicesByGuest_) {
        if (device.type == "serial" && device.enabled) {
            if (device.backend == "win32_com" && !device.host.empty()) return device.host;
            if (fallback.empty()) fallback = device.guest.empty() ? guest : device.guest;
        }
    }
    return fallback;
}

#else

void installRemoteLogSink() {}

SyntheticDllRuntime::~SyntheticDllRuntime() = default;

void SyntheticDllRuntime::setRemoteServerConfig(RemoteServerConfig config) {
    remoteConfig_ = std::move(config);
}

void SyntheticDllRuntime::startRemoteServer() {}
void SyntheticDllRuntime::stopRemoteServer() {}
void SyntheticDllRuntime::drainRemoteInputEvents() {}

bool SyntheticDllRuntime::enqueueRemoteTouch(const std::string&, int32_t, int32_t, std::string& error) {
    error = "remote server is only available on Windows";
    return false;
}
bool SyntheticDllRuntime::enqueueRemoteKey(const std::string&, uint32_t, std::string& error) {
    error = "remote server is only available on Windows";
    return false;
}
void SyntheticDllRuntime::updateRemoteImuState(const nlohmann::json&) {}

void SyntheticDllRuntime::injectRemoteSerialBytes(const std::string&) {}
size_t SyntheticDllRuntime::readRemoteSerialBytes(uint8_t*, size_t) { return 0; }
void SyntheticDllRuntime::publishRemoteAudioChunk(const std::vector<uint8_t>&,
                                                  uint16_t,
                                                  uint32_t,
                                                  uint16_t,
                                                  uint16_t,
                                                  uint16_t) {}
void SyntheticDllRuntime::registerRemoteAudioClient() {}
void SyntheticDllRuntime::unregisterRemoteAudioClient() {}
void SyntheticDllRuntime::clearRemoteAudioChunks() {}
bool SyntheticDllRuntime::waitForRemoteAudioChunks(uint32_t) { return false; }
std::vector<SyntheticDllRuntime::RemoteAudioChunk> SyntheticDllRuntime::takeRemoteAudioChunks(size_t) { return {}; }
std::vector<uint32_t> SyntheticDllRuntime::copyRemoteFramebuffer(int& width, int& height) const {
    width = 0;
    height = 0;
    return {};
}
nlohmann::json SyntheticDllRuntime::remoteStatusJson() const { return nlohmann::json::object(); }
std::vector<std::string> SyntheticDllRuntime::recentRemoteLogLines(size_t) const { return {}; }
std::string SyntheticDllRuntime::remoteGpsTarget() const { return {}; }

#endif
