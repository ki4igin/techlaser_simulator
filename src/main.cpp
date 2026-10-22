#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <iomanip>
#include <linux/serial.h>
#include <ncurses.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace {

using Clock = std::chrono::steady_clock;


class TerminalUI {
public:
	bool initialize() {
		if (!::isatty(STDIN_FILENO) || !::isatty(STDOUT_FILENO)) {
			std::fprintf(stderr, "Run simulator in an interactive terminal.\n");
			return false;
		}

		if (initscr() == nullptr) return false;

		active = true;

		cbreak();
		noecho();
		curs_set(0);
		timeout(0);

		if (has_colors()) {
			start_color();
			use_default_colors();
			init_pair(1, COLOR_GREEN, -1);
			init_pair(2, COLOR_CYAN, -1);
			init_pair(3, COLOR_RED, -1);
			init_pair(4, COLOR_YELLOW, -1);
		}

		createWindows();
		return true;
	}

	void shutdown() {
		if (!active) return;

		if (statusWindow) delwin(statusWindow);

		if (logWindow) delwin(logWindow);

		statusWindow = nullptr;
		logWindow    = nullptr;

		endwin();
		active = false;
	}

	~TerminalUI() { shutdown(); }

	void log(const std::string & message) {
		if (!active) return;

		if (!logWindow) createWindows();

		if (!logWindow) return;


		logs.push_back(message);

		while (static_cast<int>(logs.size()) > 1000)
			logs.pop_front();

		drawLogs();
	}

	void drawStatus(const std::string & port, double angle, double speed, int deviceStatus, int motorStatus, unsigned faults) {
		if (!active) return;

		if (!statusWindow || !logWindow) createWindows();

		if (!statusWindow || !logWindow) return;

		werase(statusWindow);
		box(statusWindow, 0, 0);

		mvwprintw(statusWindow, 0, 2, " TECHLASER SIMULATOR ");

		mvwprintw(statusWindow, 1, 2, "Port: %s", port.c_str());

		mvwprintw(statusWindow, 1, 38, "115200 8N1");

		const char * device = deviceStatus == 2   ? "READY"
		                      : deviceStatus == 1 ? "SELF DIAGNOSTICS"
		                      : deviceStatus == 3 ? "HEATING"
		                      : deviceStatus == 0 ? "NOT READY"
		                                          : "INVALID";

		const char * motor  = motorStatus == 0   ? "IDLE"
		                      : motorStatus == 1 ? "HOLD"
		                      : motorStatus == 2 ? "STOPPING"
		                      : motorStatus == 3 ? "STARTING"
		                      : motorStatus == 4 ? "BRAKING"
		                      : motorStatus == 5 ? "ROTATING"
		                                         : "INVALID";

		if (has_colors()) wattron(statusWindow, COLOR_PAIR(1) | A_BOLD);

		mvwprintw(statusWindow, 2, 2, "Device: %s", device);

		if (has_colors()) wattroff(statusWindow, COLOR_PAIR(1) | A_BOLD);

		if (has_colors()) {
			wattron(statusWindow, COLOR_PAIR(motorStatus == 5 ? 1 : 4) | A_BOLD);
		}

		mvwprintw(statusWindow, 2, 38, "Motor: %s", motor);

		if (has_colors()) wattroff(statusWindow, COLOR_PAIR(motorStatus == 5 ? 1 : 4) | A_BOLD);

		mvwprintw(statusWindow, 3, 2, "Angle: %8.2f deg", angle);

		mvwprintw(statusWindow, 3, 38, "Speed: %8.2f deg/s", speed);

		if (has_colors() && faults != 0) wattron(statusWindow, COLOR_PAIR(3) | A_BOLD);

		mvwprintw(statusWindow, 4, 2, "Faults: 0x%08X", faults);

		if (has_colors() && faults != 0) wattroff(statusWindow, COLOR_PAIR(3) | A_BOLD);

		mvwprintw(statusWindow, 4, 38, "Ctrl+C: exit");

		wnoutrefresh(statusWindow);
		wnoutrefresh(logWindow);
		doupdate();
	}

private:
	bool active           = false;

	WINDOW * statusWindow = nullptr;
	WINDOW * logWindow    = nullptr;

	std::deque<std::string> logs;

	void createWindows() {
		if (!active) return;

		if (statusWindow) delwin(statusWindow);

		if (logWindow) delwin(logWindow);

		statusWindow   = nullptr;
		logWindow      = nullptr;

		const int rows = LINES;
		const int cols = COLS;

		if (rows < 12 || cols < 55) {
			erase();
			mvprintw(0, 0, "Enlarge terminal to at least 55x12.");
			refresh();
			return;
		}

		const int statusHeight = 7;

		statusWindow           = newwin(statusHeight, cols, 0, 0);
		logWindow              = newwin(rows - statusHeight, cols, statusHeight, 0);

		if (!statusWindow || !logWindow) {
			if (statusWindow) delwin(statusWindow);

			if (logWindow) delwin(logWindow);

			statusWindow = nullptr;
			logWindow    = nullptr;
			return;
		}

		scrollok(logWindow, FALSE);
		keypad(stdscr, TRUE);

		werase(logWindow);
		box(logWindow, 0, 0);
		mvwprintw(logWindow, 0, 2, " SERIAL EXCHANGE LOG ");
		wnoutrefresh(logWindow);

		drawLogs();
	}

	void drawLogs() {
		if (!logWindow) return;

		werase(logWindow);
		box(logWindow, 0, 0);
		mvwprintw(logWindow, 0, 2, " SERIAL EXCHANGE LOG ");

		const int height    = getmaxy(logWindow);
		const int width     = getmaxx(logWindow);
		const int available = height - 2;

		const int start     = std::max(0, static_cast<int>(logs.size()) - available);

		int row             = 1;

		for (int i = start; i < static_cast<int>(logs.size()) && row < height - 1; ++i, ++row) {
			const std::string & line = logs[i];

			if (has_colors()) {
				if (line.rfind("TX:", 0) == 0)
					wattron(logWindow, COLOR_PAIR(2));
				else if (line.rfind("ERROR:", 0) == 0)
					wattron(logWindow, COLOR_PAIR(3) | A_BOLD);
				else if (line.rfind("RX:", 0) == 0)
					wattron(logWindow, COLOR_PAIR(4));
			}

			mvwaddnstr(logWindow, row, 2, line.c_str(), std::max(0, width - 4));

			if (has_colors()) wattroff(logWindow, COLOR_PAIR(2) | COLOR_PAIR(3) | COLOR_PAIR(4) | A_BOLD);
		}

		wnoutrefresh(logWindow);
		doupdate();
	}
};

TerminalUI * terminalUI            = nullptr;


volatile std::sig_atomic_t running = 1;

void onSignal(int) {
	running = 0;
}

bool configureRs485(int fd) {
	serial_rs485 disabled{};

	if (::ioctl(fd, TIOCSRS485, &disabled) < 0) {
		std::perror("TIOCSRS485 disable");
		return false;
	}

	serial_rs485 config{};
	config.flags = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND;

	if (::ioctl(fd, TIOCSRS485, &config) < 0) {
		std::perror("TIOCSRS485");
		return false;
	}

	serial_rs485 actual{};

	if (::ioctl(fd, TIOCGRS485, &actual) < 0) {
		if (terminalUI) terminalUI->log("ERROR: TIOCGRS485");
		return false;
	}

	constexpr unsigned mask = SER_RS485_ENABLED | SER_RS485_RTS_ON_SEND | SER_RS485_RTS_AFTER_SEND;

	if ((actual.flags & mask) != config.flags) {
		std::fprintf(stderr, "Unexpected RS485 flags: 0x%x\n", actual.flags);
		return false;
	}

	return true;
}

int openSerial(const char * device) {
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

	tty.c_cc[VMIN]  = 0;
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

bool writeAll(int fd, const std::string & data) {
	std::size_t offset = 0;

	while (offset < data.size() && running) {
		ssize_t n = ::write(fd, data.data() + offset, data.size() - offset);

		if (n > 0) {
			offset += static_cast<std::size_t>(n);
			continue;
		}

		if (n < 0 && errno == EINTR) continue;

		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			pollfd pfd{};
			pfd.fd     = fd;
			pfd.events = POLLOUT;

			int ret    = ::poll(&pfd, 1, 100);

			if (ret < 0 && errno != EINTR) return false;

			if (ret == 0) {
				std::fprintf(stderr, "TX timeout\n");
				return false;
			}

			continue;
		}

		if (terminalUI) terminalUI->log("ERROR: write");
		return false;
	}

	if (!running) return false;

	// Дожидаемся передачи всех байтов UART.
	if (::tcdrain(fd) < 0) {
		if (terminalUI) terminalUI->log("ERROR: tcdrain");
		return false;
	}

	return true;
}

bool sendPacket(int fd, const std::string & packet) {
	::usleep(1000); // Временно: 5 мс для проверки смены направления.
	if (terminalUI) terminalUI->log("TX: " + packet);

	return writeAll(fd, packet);
}

double normalizeAngle(double angle) {
	angle = std::fmod(angle, 360.0);

	if (angle < 0.0) angle += 360.0;

	return angle;
}

bool parseSpeed(const std::string & packet, double & speed) {
	if (packet.size() < 5 || packet.compare(0, 3, "$i,") != 0 || packet.back() != '#') return false;

	std::string value = packet.substr(3, packet.size() - 4);

	if (value.empty()) return false;

	char * end    = nullptr;
	errno         = 0;

	double parsed = std::strtod(value.c_str(), &end);

	if (end == value.c_str() || *end != '\0' || errno == ERANGE || !std::isfinite(parsed)) return false;

	speed = parsed;
	return true;
}

class Simulator {
public:
	explicit Simulator(int serialFd, const std::string & serialPort): fd(serialFd), port(serialPort), lastUpdate(Clock::now()) {}

	void run() {
		auto lastDisplay = Clock::now();

		if (terminalUI) {
			terminalUI->drawStatus(port, angle, speed, deviceStatus, motorStatus, motorFaults);
		}

		while (running) {
			updateMotion();

			const auto now = Clock::now();

			if (now - lastDisplay >= std::chrono::milliseconds(100)) {
				if (terminalUI) {
					terminalUI->drawStatus(port, angle, speed, deviceStatus, motorStatus, motorFaults);
				}
				lastDisplay = now;
			}


			pollfd pfd{};
			pfd.fd     = fd;
			pfd.events = POLLIN;

			int result = ::poll(&pfd, 1, 10);

			if (result < 0) {
				if (errno == EINTR) continue;

				if (terminalUI) {
					terminalUI->log("ERROR: poll");
				}
				break;
			}

			if (result == 0) continue;

			if (pfd.revents & (POLLERR | POLLNVAL)) {
				if (terminalUI) {
					terminalUI->log("ERROR: Serial poll error");
				}
				break;
			}

			if (!(pfd.revents & POLLIN)) continue;

			char buffer[256];

			ssize_t n = ::read(fd, buffer, sizeof(buffer));

			if (n < 0) {
				if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;

				if (terminalUI) terminalUI->log("ERROR: read");
				break;
			}

			if (n == 0) continue;

			for (ssize_t i = 0; i < n; ++i)
				receiveByte(buffer[i]);
		}
	}

private:
	int fd;
	std::string port;

	double angle         = 0.0;
	double speed         = 180.0;

	int deviceStatus     = 2; // Ready
	int motorStatus      = 5; // Rotating

	unsigned motorFaults = 0;

	Clock::time_point lastUpdate;

	std::string input;

	void updateMotion() {
		auto now   = Clock::now();

		double dt  = std::chrono::duration<double>(now - lastUpdate).count();

		lastUpdate = now;

		angle      = normalizeAngle(angle + speed * dt);
	}

	void receiveByte(char c) {
		if (c == '$') {
			input.clear();
			input.push_back(c);
			return;
		}

		if (input.empty()) return;

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


	void processPacket(const std::string & packet) {
		if (terminalUI) terminalUI->log("RX: " + packet);

		if (packet == "$a#") {
			sendPacket(fd, "$a," + std::to_string(deviceStatus) + "#");

		} else if (packet == "$b#") {
			char response[32];
			std::snprintf(response, sizeof(response), "$b,%08X#", motorFaults);
			sendPacket(fd, response);

		} else if (packet == "$c#") {
			char response[64];
			std::snprintf(response, sizeof(response), "$c,%.6f#", angle);
			sendPacket(fd, response);

		} else if (packet == "$d#") {
			char response[64];
			std::snprintf(response, sizeof(response), "$d,%.6f#", speed);
			sendPacket(fd, response);

		} else if (packet == "$e#") {
			sendPacket(fd, "$e," + std::to_string(motorStatus) + "#");

		} else if (packet == "$g#") {
			speed       = 0.0;
			motorStatus = 0;

			if (terminalUI) terminalUI->log("Motor stopped");

			sendPacket(fd, "$g#");

		} else if (packet.compare(0, 3, "$i,") == 0) {
			double requestedSpeed = 0.0;

			if (!parseSpeed(packet, requestedSpeed)) {
				if (terminalUI) terminalUI->log("ERROR: Invalid speed command");

				sendPacket(fd, "$X#");
				return;
			}

			if (deviceStatus != 2 || motorFaults != 0) {
				if (terminalUI) terminalUI->log("ERROR: Device not ready or motor fault");

				sendPacket(fd, "$X#");
				return;
			}

			speed       = requestedSpeed;

			motorStatus = std::abs(speed) > 0.000001 ? 5 : 0;

			if (terminalUI) {
				char message[96];

				std::snprintf(message, sizeof(message), "Motor speed set to %.3f deg/s", speed);

				terminalUI->log(message);
			}

			char response[64];

			std::snprintf(response, sizeof(response), "$i,%.6f#", speed);

			sendPacket(fd, response);

		} else {
			if (terminalUI) terminalUI->log("ERROR: Unknown command " + packet);

			sendPacket(fd, "$X#");
		}
	}
};

} // namespace


int main(int argc, char ** argv) {
	if (argc != 2) {
		std::fprintf(stderr, "Usage: %s /dev/ttySx\n", argv[0]);
		return EXIT_FAILURE;
	}

	std::signal(SIGINT, onSignal);
	std::signal(SIGTERM, onSignal);

	int fd = openSerial(argv[1]);

	if (fd < 0) return EXIT_FAILURE;

	TerminalUI ui;

	if (!ui.initialize()) {
		::close(fd);
		return EXIT_FAILURE;
	}

	terminalUI = &ui;

	{
		Simulator simulator(fd, argv[1]);
		simulator.run();
	}

	terminalUI = nullptr;
	ui.shutdown();

	::close(fd);
	return EXIT_SUCCESS;
}
