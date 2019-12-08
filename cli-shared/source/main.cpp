/* Orchid - WebRTC P2P VPN Market (on Ethereum)
 * Copyright (C) 2017-2019  The Orchid Authors
*/

/* GNU Affero General Public License, Version 3 {{{ */
/*
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.

 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/
/* }}} */


#define OPENVPN_EXTERN extern

#include "log.hpp"
#define OPENVPN_LOG_STREAM orc::Log()
#include <openvpn/log/logsimple.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <cerrno>
#include <unistd.h>

#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#if 0
#elif defined(__APPLE__)
#include <net/if_utun.h>
#include <sys/sys_domain.h>
#include <sys/kern_control.h>
#elif defined(__linux__)
#include <linux/if_tun.h>
#endif

#include <boost/asio/generic/datagram_protocol.hpp>
#include <boost/filesystem/string_file.hpp>

#include <boost/program_options/parsers.hpp>
#include <boost/program_options/options_description.hpp>
#include <boost/program_options/variables_map.hpp>

#include <asio.hpp>
#include "capture.hpp"
#include "error.hpp"
#include "port.hpp"
#include "protect.hpp"
#include "sync.hpp"
#include "syscall.hpp"
#include "task.hpp"
#include "file.hpp"
#include "transport.hpp"

#if 0
#elif defined(__APPLE__)
#include "family.hpp"
#endif

namespace orc {

namespace po = boost::program_options;

std::string Group() {
    // UGH: error: 'current_path' is unavailable: introduced in macOS 10.15
    //return std::filesystem::current_path();
    return boost::filesystem::current_path().string();
}

int Main(int argc, const char *const argv[]) {
    po::variables_map args;

    po::options_description options("command-line (only)");
    options.add_options()
        ("help", "produce help message")
        ("capture", po::value<std::string>(), "single ip address to capture")
        ("config", po::value<std::string>(), "configuration file for client configuration")
    ;

    po::store(po::parse_command_line(argc, argv, po::options_description()
        .add(options)
    ), args);

    po::notify(args);

    if (args.count("help") != 0) {
        std::cout << po::options_description()
            .add(options)
        << std::endl;
        return 0;
    }


    Initialize();


    auto local(Host_);
    auto capture(Make<Sink<Capture>>(local));

#if 0
#elif defined(__APPLE__)
    auto family(capture->Wire<Sink<Family>>());
    auto sync(family->Wire<Sync<asio::generic::datagram_protocol::socket>>(Context(), asio::generic::datagram_protocol(PF_SYSTEM, SYSPROTO_CONTROL)));

    auto file((*sync)->native_handle());

    ctl_info info;
    memset(&info, 0, sizeof(info));
    orc_assert(strlcpy(info.ctl_name, UTUN_CONTROL_NAME, sizeof(info.ctl_name)) < sizeof(info.ctl_name));
    // XXX: is there a way I can do this with boost, to avoid the vararg call?
    // NOLINTNEXTLINE (cppcoreguidelines-pro-type-vararg)
    orc_syscall(ioctl(file, CTLIOCGINFO, &info));

    struct sockaddr_ctl address;
    address.sc_id = info.ctl_id;
    address.sc_len = sizeof(address);
    address.sc_family = AF_SYSTEM;
    address.ss_sysaddr = AF_SYS_CONTROL;
    address.sc_unit = 0;

    do ++address.sc_unit;
    while (orc_syscall(connect(file, reinterpret_cast<struct sockaddr *>(&address), sizeof(address)), EBUSY) != 0);

    auto utun("utun" + std::to_string(address.sc_unit - 1));
    orc_assert(system(("ifconfig " + utun + " inet " + local.String() + " " + local.String() + " mtu 1500 up").c_str()) == 0);
    if (args.count("capture") != 0)
        orc_assert(system(("route -n add " + args["capture"].as<std::string>() + " -interface " + utun).c_str()) == 0);
    else {
        orc_assert(system(("route -n add 0.0.0.0/1 -interface " + utun).c_str()) == 0);
        orc_assert(system(("route -n add 128.0.0.0/1 -interface " + utun).c_str()) == 0);
    }
    orc_assert(system(("route -n add 10.7.0.4 -interface " + utun).c_str()) == 0);
#elif defined(__linux__)
    int file = open("/dev/net/tun", O_RDWR);
    orc_assert(file >= 0);

    auto connection(std::make_unique<File<asio::posix::stream_descriptor>>(Context(), file));
    auto sync(capture->Wire<Inverted>(std::move(connection)));

    struct ifreq ifr = {.ifr_flags = IFF_TUN | IFF_NO_PI};
    orc_assert(ioctl(file, TUNSETIFF, (void*)&ifr) >= 0);
    char dev[IFNAMSIZ];
    strcpy(dev, ifr.ifr_name);
    std::string tun(dev);
    orc_assert(system(("ifconfig " + tun + " inet " + local.String() + " " + local.String() + " mtu 1500 up").c_str()) == 0);
    if (args.count("capture") != 0)
        orc_assert(system(("route -n add " + args["capture"].as<std::string>() + " -interface " + tun).c_str()) == 0);
    else {
        orc_assert(system(("route -n add 0.0.0.0/1 -interface " + tun).c_str()) == 0);
        orc_assert(system(("route -n add 128.0.0.0/1 -interface " + tun).c_str()) == 0);
    }
    orc_assert(system(("route -n add 10.7.0.4 -interface " + tun).c_str()) == 0);
#else
#error
#endif

    // XXX: rage against the cage, my friend :(
    (void) setgid(501);
    (void) setuid(501);

    Wait([&]() -> task<void> { try {
        co_await Schedule();
        co_await capture->Start(args["config"].as<std::string>());
        sync->Open();
    } catch (const std::exception &error) {
        std::cerr << error.what() << std::endl;
        throw;
    } }());

    Thread().join();
    return 0;
}

}

int main(int argc, const char *const argv[]) { try {
    return orc::Main(argc, argv);
} catch (const std::exception &error) {
    std::cerr << error.what() << std::endl;
    return 1;
} }
