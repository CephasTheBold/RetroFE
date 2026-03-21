#include "TOSGRSRestrictor.h"
#include "../../Utility/Log.h"
#include <algorithm>
#include <string>

static constexpr char COMPONENT[] = "TOSGRS";

TOSGRSRestrictor::TOSGRSRestrictor() : port_(nullptr) {}

TOSGRSRestrictor::~TOSGRSRestrictor() {
	if (port_) {
		sp_close(port_);
		sp_free_port(port_);
	}
}

bool TOSGRSRestrictor::initialize() {
	if (port_) {
		sp_close(port_);
		sp_free_port(port_);
		port_ = nullptr;
	}

	LOG_INFO(COMPONENT, "Searching for TOS GRS hardware...");
	port_ = findPort();

	if (!port_) {
		LOG_ERROR(COMPONENT, "GRS device not found or failed to initialize.");
		return false;
	}

	LOG_INFO(COMPONENT, "TOS GRS restrictor initialized successfully.");
	return true;
}

bool TOSGRSRestrictor::setWay(int way) {
	if (way != 4 && way != 8) return false;

	// Auto-reconnect if port was lost or never found
	if (!port_ && !initialize()) return false;

	auto current = getWay();
	if (current && *current == way) return true;

	return sendCmd("setway,all," + std::to_string(way)) != "err";
}

std::optional<int> TOSGRSRestrictor::getWay() {
	if (!port_ && !initialize()) return std::nullopt;

	auto r = sendCmd("getway,1");
	if (!r.empty() && (r[0] == '4' || r[0] == '8')) {
		return r[0] - '0';
	}
	return std::nullopt;
}

std::string TOSGRSRestrictor::sendCmd(const std::string& cmd) {
	if (!port_) return "err";

	sp_flush(port_, SP_BUF_INPUT);

	std::string command = cmd;
	if (command.empty() || command.back() != '\r') {
		command += '\r';
	}

	// Check for write failure (unplugged/hardware error)
	if (sp_blocking_write(port_, command.c_str(), command.size(), 500) < 0) {
		LOG_ERROR(COMPONENT, "Write failed. Device disconnected. Attempting recovery...");

		sp_close(port_);
		sp_free_port(port_);
		port_ = nullptr;

		if (initialize()) {
			return sendCmd(cmd); // Recursive retry once
		}
		return "err";
	}

	struct sp_event_set* eventSet = nullptr;
	if (sp_new_event_set(&eventSet) != SP_OK) return "err";
	sp_add_port_events(eventSet, port_, SP_EVENT_RX_READY);

	int n = 0;
	char buf[128];
	if (sp_wait(eventSet, 1000) == SP_OK) {
		n = sp_nonblocking_read(port_, buf, sizeof(buf));
	}
	sp_free_event_set(eventSet);

	if (n <= 0) {
		LOG_ERROR(COMPONENT, "No response from GRS device.");
		return "err";
	}

	std::string response(buf, n);
	while (!response.empty() && (response.back() == '\r' || response.back() == '\n')) {
		response.pop_back();
	}

	LOG_INFO(COMPONENT, "Received ASCII: \"" + response + "\"");
	return response.empty() ? "err" : response;
}

sp_port* TOSGRSRestrictor::findPort() {
	sp_port** ports = nullptr;
	if (sp_list_ports(&ports) != SP_OK) return nullptr;

	sp_port* result = nullptr;
	for (int i = 0; ports[i]; ++i) {
		if (sp_get_port_transport(ports[i]) != SP_TRANSPORT_USB) continue;

		int vid, pid;
		if (sp_get_port_usb_vid_pid(ports[i], &vid, &pid) != SP_OK) continue;

		// Match against GRS_VID/GRS_PID constants from header
		if (vid != GRS_VID || pid != GRS_PID) continue;

		LOG_INFO(COMPONENT, "Candidate found: " + std::string(sp_get_port_name(ports[i])));

		sp_port* testPort = nullptr;
		if (sp_copy_port(ports[i], &testPort) != SP_OK) continue;

		if (sp_open(testPort, SP_MODE_READ_WRITE) == SP_OK) {
			sp_set_baudrate(testPort, 115200);
			sp_set_bits(testPort, 8);
			sp_set_parity(testPort, SP_PARITY_NONE);
			sp_set_stopbits(testPort, 1);
			sp_set_flowcontrol(testPort, SP_FLOWCONTROL_NONE);

			const std::string probe = "getwelcome\r";
			sp_blocking_write(testPort, probe.c_str(), probe.size(), 200);

			char buf[128] = {};
			int n = sp_blocking_read(testPort, buf, sizeof(buf), 500);
			if (n > 0) {
				std::string response(buf, n);
				std::transform(response.begin(), response.end(), response.begin(),
							   [](unsigned char c){ return std::tolower(c); });

				if (response.find("tos428") != std::string::npos) {
					LOG_INFO(COMPONENT, "Handshake successful on " + std::string(sp_get_port_name(testPort)));
					result = testPort;
					break;
				}
			}
			sp_close(testPort);
		}
		sp_free_port(testPort);
	}
	sp_free_port_list(ports);
	return result;
}

bool TOSGRSRestrictor::isPresent() {
	TOSGRSRestrictor temp;
	sp_port* p = temp.findPort();
	if (p) {
		sp_close(p);
		sp_free_port(p);
		return true;
	}
	return false;
}
