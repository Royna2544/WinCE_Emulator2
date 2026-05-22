#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

static constexpr double EARTH_RADIUS_KM = 6371.0;
static constexpr double KNOT_TO_KMH = 1.852;

double degToRad(double deg) {
    return deg * 3.14159265358979323846 / 180.0;
}

double radToDeg(double rad) {
    return rad * 180.0 / 3.14159265358979323846;
}

std::string nmeaChecksum(const std::string& body) {
    unsigned char checksum = 0;

    for (char c : body) {
        checksum ^= static_cast<unsigned char>(c);
    }

    std::ostringstream oss;
    oss << std::uppercase << std::hex << std::setw(2) << std::setfill('0')
        << static_cast<int>(checksum);

    return oss.str();
}

std::string makeSentence(const std::string& body) {
    return "$" + body + "*" + nmeaChecksum(body) + "\r\n";
}

std::string formatUtcTime(const std::tm& tm) {
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << tm.tm_hour
        << std::setw(2) << std::setfill('0') << tm.tm_min
        << std::setw(2) << std::setfill('0') << tm.tm_sec
        << ".000";
    return oss.str();
}

std::string formatUtcDate(const std::tm& tm) {
    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << tm.tm_mday
        << std::setw(2) << std::setfill('0') << (tm.tm_mon + 1)
        << std::setw(2) << std::setfill('0') << ((tm.tm_year + 1900) % 100);
    return oss.str();
}

std::string formatNmeaLat(double lat) {
    double absLat = std::fabs(lat);
    int degrees = static_cast<int>(absLat);
    double minutes = (absLat - degrees) * 60.0;

    std::ostringstream oss;
    oss << std::setw(2) << std::setfill('0') << degrees
        << std::fixed << std::setprecision(4)
        << std::setw(7) << std::setfill('0') << minutes;

    return oss.str();
}

std::string formatNmeaLon(double lon) {
    double absLon = std::fabs(lon);
    int degrees = static_cast<int>(absLon);
    double minutes = (absLon - degrees) * 60.0;

    std::ostringstream oss;
    oss << std::setw(3) << std::setfill('0') << degrees
        << std::fixed << std::setprecision(4)
        << std::setw(7) << std::setfill('0') << minutes;

    return oss.str();
}

char latHemisphere(double lat) {
    return lat >= 0.0 ? 'N' : 'S';
}

char lonHemisphere(double lon) {
    return lon >= 0.0 ? 'E' : 'W';
}

void movePosition(double& latDeg, double& lonDeg, double speedKnots, double courseDeg, int intervalMs) {
    if (speedKnots <= 0.0 || intervalMs <= 0) {
        return;
    }

    double speedKmh = speedKnots * KNOT_TO_KMH;
    double hours = static_cast<double>(intervalMs) / 3600000.0;
    double distanceKm = speedKmh * hours;

    double angularDistance = distanceKm / EARTH_RADIUS_KM;
    double bearing = degToRad(courseDeg);

    double lat1 = degToRad(latDeg);
    double lon1 = degToRad(lonDeg);

    double lat2 = std::asin(
        std::sin(lat1) * std::cos(angularDistance) +
        std::cos(lat1) * std::sin(angularDistance) * std::cos(bearing)
    );

    double lon2 = lon1 + std::atan2(
        std::sin(bearing) * std::sin(angularDistance) * std::cos(lat1),
        std::cos(angularDistance) - std::sin(lat1) * std::sin(lat2)
    );

    latDeg = radToDeg(lat2);
    lonDeg = radToDeg(lon2);

    if (lonDeg > 180.0) lonDeg -= 360.0;
    if (lonDeg < -180.0) lonDeg += 360.0;
}

HANDLE openSerialPort(const std::string& portName, DWORD baudRate) {
    std::string fullPortName = portName;

    // COM10 이상도 열 수 있게 처리.
    if (fullPortName.rfind("\\\\.\\", 0) != 0) {
        fullPortName = "\\\\.\\" + fullPortName;
    }

    HANDLE hSerial = CreateFileA(
        fullPortName.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        0,
        nullptr
    );

    if (hSerial == INVALID_HANDLE_VALUE) {
        std::cerr << "Failed to open serial port: " << portName
            << ", error=" << GetLastError() << "\n";
        return INVALID_HANDLE_VALUE;
    }

    DCB dcb = {};
    dcb.DCBlength = sizeof(dcb);

    if (!GetCommState(hSerial, &dcb)) {
        std::cerr << "GetCommState failed, error=" << GetLastError() << "\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    dcb.BaudRate = baudRate;
    dcb.ByteSize = 8;
    dcb.StopBits = ONESTOPBIT;
    dcb.Parity = NOPARITY;

    dcb.fBinary = TRUE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(hSerial, &dcb)) {
        std::cerr << "SetCommState failed, error=" << GetLastError() << "\n";
        CloseHandle(hSerial);
        return INVALID_HANDLE_VALUE;
    }

    COMMTIMEOUTS timeouts = {};
    timeouts.ReadIntervalTimeout = 50;
    timeouts.ReadTotalTimeoutConstant = 50;
    timeouts.ReadTotalTimeoutMultiplier = 10;
    timeouts.WriteTotalTimeoutConstant = 50;
    timeouts.WriteTotalTimeoutMultiplier = 10;

    SetCommTimeouts(hSerial, &timeouts);
    PurgeComm(hSerial, PURGE_RXCLEAR | PURGE_TXCLEAR);

    return hSerial;
}

bool writeSerial(HANDLE hSerial, const std::string& data) {
    DWORD bytesWritten = 0;

    BOOL ok = WriteFile(
        hSerial,
        data.data(),
        static_cast<DWORD>(data.size()),
        &bytesWritten,
        nullptr
    );

    return ok && bytesWritten == data.size();
}

std::string makeGGA(
    const std::string& utcTime,
    double lat,
    double lon
) {
    std::ostringstream body;

    body << "GPGGA,"
        << utcTime << ","
        << formatNmeaLat(lat) << "," << latHemisphere(lat) << ","
        << formatNmeaLon(lon) << "," << lonHemisphere(lon) << ","
        << "1,"       // fix quality: 1 = GPS fix
        << "08,"      // satellites
        << "0.9,"     // HDOP
        << "50.0,M,"  // altitude
        << "19.5,M,"  // geoid separation
        << ",";       // DGPS age, DGPS station id empty

    return makeSentence(body.str());
}

std::string makeRMC(
    const std::string& utcTime,
    const std::string& utcDate,
    double lat,
    double lon,
    double speedKnots,
    double courseDeg
) {
    std::ostringstream body;

    body << std::fixed << std::setprecision(1);

    body << "GPRMC,"
        << utcTime << ","
        << "A,"  // A = valid
        << formatNmeaLat(lat) << "," << latHemisphere(lat) << ","
        << formatNmeaLon(lon) << "," << lonHemisphere(lon) << ","
        << speedKnots << ","
        << courseDeg << ","
        << utcDate << ","
        << ","   // magnetic variation empty
        << ","
        << "A";  // mode: A = autonomous

    return makeSentence(body.str());
}

std::string makeVTG(double speedKnots, double courseDeg) {
    double speedKmh = speedKnots * KNOT_TO_KMH;

    std::ostringstream body;
    body << std::fixed << std::setprecision(1);

    body << "GPVTG,"
        << courseDeg << ",T,"
        << ",M,"
        << speedKnots << ",N,"
        << speedKmh << ",K,"
        << "A";

    return makeSentence(body.str());
}

void printUsage() {
    std::cout
        << "Usage:\n"
        << "  nmea_sender.exe COMx baud interval_ms lat lon speed_knots course_deg\n\n"
        << "Example:\n"
        << "  nmea_sender.exe COM7 9600 1000 37.5665 126.9780 10.0 90.0\n\n"
        << "Notes:\n"
        << "  lat/lon      : decimal degrees\n"
        << "  speed_knots  : knots, 0 means fixed position\n"
        << "  course_deg   : 0=north, 90=east, 180=south, 270=west\n";
}

int main(int argc, char* argv[]) {
    if (argc != 8) {
        printUsage();
        return 1;
    }

    std::string portName = argv[1];
    DWORD baudRate = static_cast<DWORD>(std::stoul(argv[2]));
    int intervalMs = std::stoi(argv[3]);

    double lat = std::stod(argv[4]);
    double lon = std::stod(argv[5]);
    double speedKnots = std::stod(argv[6]);
    double courseDeg = std::stod(argv[7]);

    if (intervalMs <= 0) {
        std::cerr << "interval_ms must be greater than 0.\n";
        return 1;
    }

    if (lat < -90.0 || lat > 90.0) {
        std::cerr << "latitude must be between -90 and 90.\n";
        return 1;
    }

    if (lon < -180.0 || lon > 180.0) {
        std::cerr << "longitude must be between -180 and 180.\n";
        return 1;
    }

    HANDLE hSerial = openSerialPort(portName, baudRate);
    if (hSerial == INVALID_HANDLE_VALUE) {
        return 1;
    }

    std::cout << "Sending NMEA to " << portName << " at " << baudRate << " baud.\n";
    std::cout << "Press Ctrl+C to stop.\n\n";

    while (true) {
        auto now = std::chrono::system_clock::now();
        std::time_t nowTime = std::chrono::system_clock::to_time_t(now);

        std::tm utcTm = {};
        gmtime_s(&utcTm, &nowTime);

        std::string utcTime = formatUtcTime(utcTm);
        std::string utcDate = formatUtcDate(utcTm);

        std::string gga = makeGGA(utcTime, lat, lon);
        std::string rmc = makeRMC(utcTime, utcDate, lat, lon, speedKnots, courseDeg);
        std::string vtg = makeVTG(speedKnots, courseDeg);

        std::string packet = gga + rmc + vtg;

        if (!writeSerial(hSerial, packet)) {
            std::cerr << "WriteFile failed, error=" << GetLastError() << "\n";
            break;
        }

        std::cout << packet;

        movePosition(lat, lon, speedKnots, courseDeg, intervalMs);

        std::this_thread::sleep_for(std::chrono::milliseconds(intervalMs));
    }

    CloseHandle(hSerial);
    return 0;
}