
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/serial.h>
#include <poll.h>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;

volatile std::sig_atomic_t running = 1;

void onSignal(int)
{
    running = 0;
}

bool configureRs485(int fd)
{
    serial_rs485 config{};
    config.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;

    if (::ioctl(fd, TIOCSRS485, &config) < 0) {
        std::perror("TIOCSRS485");
        return false;
    }

    serial_rs485 actual{};

    if (::ioctl(fd, TIOCGRS485, &actual) < 0) {
        std::perror("TIOCGRS485");
        return false;
    }

    constexpr unsigned mask =
        SER_RS485_ENABLED |
        SER_RS485_RTS_ON_SEND |
        SER_RS485_RTS_AFTER_SEND;

    if ((actual.flags & mask) != config.flags) {
        std::fprintf(stderr,
                     "Unexpected RS485 flags: 0x%x\n",
                     actual.flags);
        return false;
    }

    return true;
}

int openSerial(const char *device)
{
    int fd = ::open(device, O_RDWR | O_NOCTTY | O_NONBLOCK);

    if (fd < 0) {
        std::perror("open serial");
        return -1;
    }

    termios tty{};

    if (::tcgetattr(fd, &tty) < 0) {
        std::perror("tcgetattr");
        ::close(fd);
        return -1;
    }

    ::cfmakeraw(&tty);

    ::cfsetispeed(&tty, B115200);
    ::cfsetospeed(&tty, B115200);

    tty.c_cflag &= ~(PARENB | CSTOPB | CSIZE);
    tty.c_cflag |= CS8 | CLOCAL | CREAD;

#ifdef CRTSCTS
    tty.c_cflag &= ~CRTSCTS;
#endif

    tty.c_cc[VMIN] = 0;
    tty.c_cc[VTIME] = 0;

    if (::tcsetattr(fd, TCSANOW, &tty) < 0) {
        std::perror("tcsetattr");
        ::close(fd);
        return -1;
    }

    if (!configureRs485(fd)) {
        ::close(fd);
        return -1;
    }

    ::tcflush(fd, TCIOFLUSH);

    return fd;
}

bool writeAll(int fd, const std::string &data)
{
    std::size_t offset = 0;

    while (offset < data.size() && running) {
        ssize_t n = ::write(
            fd,
            data.data() + offset,
            data.size() - offset
        );

        if (n > 0) {
            offset += static_cast<std::size_t>(n);
            continue;
        }

        if (n < 0 && errno == EINTR)
            continue;

        if (n < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK)) {

            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLOUT;

            int ret = ::poll(&pfd, 1, 100);

            if (ret < 0 && errno != EINTR)
                return false;

            if (ret == 0) {
                std::fprintf(stderr, "TX timeout\n");
                return false;
            }

            continue;
        }

        std::perror("write");
        return false;
    }

    if (!running)
        return false;

    // Дожидаемся передачи всех байтов UART.
    if (::tcdrain(fd) < 0) {
        std::perror("tcdrain");
        return false;
    }

    return true;
}

bool sendPacket(int fd, const std::string &packet)
{
    std::printf("TX: %s\n", packet.c_str());
    std::fflush(stdout);

    return writeAll(fd, packet);
}

double normalizeAngle(double angle)
{
    angle = std::fmod(angle, 360.0);

    if (angle < 0.0)
        angle += 360.0;

    return angle;
}

bool parseSpeed(const std::string &packet, double &speed)
{
    if (packet.size() < 5 ||
        packet.compare(0, 3, "$i,") != 0 ||
        packet.back() != '#')
        return false;

    std::string value =
        packet.substr(3, packet.size() - 4);

    if (value.empty())
        return false;

    char *end = nullptr;
    errno = 0;

    double parsed = std::strtod(value.c_str(), &end);

    if (end == value.c_str() ||
        *end != '\0' ||
        errno == ERANGE ||
        !std::isfinite(parsed))
        return false;

    speed = parsed;
    return true;
}

class Simulator {
public:
    explicit Simulator(int serialFd)
        : fd(serialFd),
          lastUpdate(Clock::now())
    {
    }

    void run()
    {
        std::printf("Techlaser RS485 simulator started\n");
        std::printf("Baud: 115200, 8N1\n");
        std::printf("Device: Ready\n\n");

        while (running) {
            updateMotion();

            pollfd pfd{};
            pfd.fd = fd;
            pfd.events = POLLIN;

            int result = ::poll(&pfd, 1, 10);

            if (result < 0) {
                if (errno == EINTR)
                    continue;

                std::perror("poll");
                break;
            }

            if (result == 0)
                continue;

            if (pfd.revents & (POLLERR | POLLNVAL)) {
                std::fprintf(stderr, "Serial poll error\n");
                break;
            }

            if (!(pfd.revents & POLLIN))
                continue;

            char buffer[256];

            ssize_t n = ::read(fd, buffer, sizeof(buffer));

            if (n < 0) {
                if (errno == EINTR ||
                    errno == EAGAIN ||
                    errno == EWOULDBLOCK)
                    continue;

                std::perror("read");
                break;
            }

            if (n == 0)
                continue;

            for (ssize_t i = 0; i < n; ++i)
                receiveByte(buffer[i]);
        }
    }

private:
    int fd;

    double angle = 0.0;
    double speed = 0.0;

    int deviceStatus = 2; // Ready
    int motorStatus = 0;  // Idle

    unsigned motorFaults = 0;

    Clock::time_point lastUpdate;

    std::string input;

    void updateMotion()
    {
        auto now = Clock::now();

        double dt =
            std::chrono::duration<double>(
                now - lastUpdate
            ).count();

        lastUpdate = now;

        angle = normalizeAngle(angle + speed * dt);
    }

    void receiveByte(char c)
    {
        if (c == '$') {
            input.clear();
            input.push_back(c);
            return;
        }

        if (input.empty())
            return;

        // Защита от слишком длинного пакета.
        if (input.size() >= 128) {
            input.clear();
            return;
        }

        input.push_back(c);

        if (c == '#') {
            std::string packet = input;
            input.clear();

            updateMotion();
            processPacket(packet);
        }
    }

    void processPacket(const std::string &packet)
    {
        std::printf("RX: %s\n", packet.c_str());

        if (packet == "$a#") {
            sendPacket(fd,
                "$a," + std::to_string(deviceStatus) + "#");
        }
        else if (packet == "$b#") {
            char response[32];

            std::snprintf(response, sizeof(response),
                          "$b,%08X#", motorFaults);

            sendPacket(fd, response);
        }
        else if (packet == "$c#") {
            char response[64];

            std::snprintf(response, sizeof(response),
                          "$c,%.6f#", angle);

            sendPacket(fd, response);
        }
        else if (packet == "$d#") {
            char response[64];

            std::snprintf(response, sizeof(response),
                          "$d,%.6f#", speed);

            sendPacket(fd, response);
        }
        else if (packet == "$e#") {
            sendPacket(fd,
                "$e," + std::to_string(motorStatus) + "#");
        }
        else if (packet == "$g#") {
            speed = 0.0;
            motorStatus = 0;

            std::printf("Motor stopped\n");

            sendPacket(fd, "$g#");
        }
        else if (packet.compare(0, 3, "$i,") == 0) {
            double requestedSpeed = 0.0;

            if (!parseSpeed(packet, requestedSpeed)) {
                sendPacket(fd, "$X#");
                return;
            }

            if (deviceStatus != 2 || motorFaults != 0) {
                sendPacket(fd, "$X#");
                return;
            }

            speed = requestedSpeed;

            motorStatus =
                std::abs(speed) > 0.000001
                    ? 5   // Rotating
                    : 0;  // Idle

            std::printf("Rotation speed: %.3f deg/s\n",
                        speed);

            // Подтверждение заданной скорости.
            char response[64];

            std::snprintf(response, sizeof(response),
                          "$i,%.6f#", speed);

            sendPacket(fd, response);
        }
        else {
            std::printf("Unknown command\n");
            sendPacket(fd, "$X#");
        }
    }
};

} // namespace

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr,
                     "Usage: %s /dev/ttySx\n",
                     argv[0]);
        return EXIT_FAILURE;
    }

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    int fd = openSerial(argv[1]);

    if (fd < 0)
        return EXIT_FAILURE;

    std::printf("Serial: %s\n", argv[1]);

    Simulator simulator(fd);
    simulator.run();

    ::close(fd);

    std::printf("Simulator stopped\n");

    return EXIT_SUCCESS;
}
