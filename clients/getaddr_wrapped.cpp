/* standard C libraries */
#include <cstdlib>
#include <cstring>
#include <cassert>

/* standard C++ libraries */
#include <iostream>
#include <utility>
#include <iomanip>
#include <fstream>
#include <unordered_map>

/* standard unix libraries */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>

/* external libraries */
#include <boost/program_options.hpp>


#include "netwrap.hpp"
#include "wrapped_buffer.hpp"
#include "logger.hpp"
#include "config.hpp"
#include "connector.hpp"
#include "lib.hpp"

using namespace std;

#define TIMEOUT (2*60*60)

//#define FIND_CXN

uint64_t g_time;
set<sockaddr_in, sockaddr_cmp> g_to_connect;
unordered_map<sockaddr_in, uint64_t, sockaddr_hash, sockaddr_keyeq> g_last_fail;

inline void do_insert(const struct sockaddr_in &x) {
	if (!is_private(x.sin_addr.s_addr)) {
		auto it = g_last_fail.find(x);
		if (it != g_last_fail.cend()) {
			uint64_t last_fail = it->second;
			if (last_fail < (g_time - TIMEOUT)) {
				g_to_connect.insert(x);
			} 
		} else {
			g_to_connect.insert(x);
		}
	}
}

void handle_message(const struct log_format *log) {
	if (log->type == BITCOIN) {
		const struct bitcoin_log_format *blf = (const struct bitcoin_log_format *)log;
		uint32_t update_type = ntoh(blf->update_type);
		if ((update_type) & (CONNECT_SUCCESS | ACCEPT_SUCCESS)) {
			do_insert(blf->remote_addr);
		} else if (update_type & CONNECT_FAILURE) {
			uint64_t fail_ts = ntoh(blf->header.timestamp);
			g_last_fail[blf->remote_addr] = fail_ts;
			if (fail_ts > (g_time - TIMEOUT)) {
				g_to_connect.erase(blf->remote_addr);
			}
		}
	} else if (log->type == BITCOIN_MSG) {
		/* and is payload */
		const struct bitcoin_msg_log_format *blog = (const struct bitcoin_msg_log_format*)log;
		if (! blog->is_sender && strcmp(blog->msg.command, "addr") == 0) {
			uint8_t bits = 0;
			uint64_t entries = bitcoin::get_varint(blog->msg.payload, &bits);
			const struct bitcoin::full_packed_net_addr *addrs = (const struct bitcoin::full_packed_net_addr*) ((uint8_t*)blog->msg.payload + bits);
			struct sockaddr_in to_insert;
			bzero(&to_insert, sizeof(to_insert));
			for(size_t i = 0; i < entries; ++i) {
				if (!is_private(addrs[i].rest.addr.ipv4.as.number)) {
					memcpy(&to_insert.sin_addr, &addrs[i].rest.addr.ipv4.as.in_addr, sizeof(to_insert.sin_addr));
					to_insert.sin_port = addrs[i].rest.port;
					to_insert.sin_family = AF_INET;
					do_insert(to_insert);
				}
			}
		}
	}
}



volatile int lastpid = 0;
void sigchld(int) {
	int status = 0;
	pid_t ret;
	do {
		ret = waitpid(-1, &status, WNOHANG);
		if (ret > 0) {
			lastpid = ret;
		}
	} while (ret > 0);
}

int main(int argc, char *argv[]) {

	if (startup_setup(argc, argv) != 0) {
		return EXIT_FAILURE;
	}

	struct sigaction sigact;
	sigemptyset(&sigact.sa_mask);
	sigact.sa_handler = sigchld;
	sigact.sa_flags = 0;

	sigaction(SIGCHLD, &sigact, NULL);

	const libconfig::Config *cfg(get_config());
	const char *config_file = cfg->lookup("version").getSourceFile();

	string g_logpath = (const char*)cfg->lookup("verbatim.logpath") ;
	string filename = g_logpath + "/" + "verbatim.log";

	for(;;) {

		g_time = time(NULL);
		g_to_connect.clear();

		int in_fd = open(filename.c_str(), O_RDONLY);
		if (in_fd < 0) {
			cerr << "Could not open input file: " << strerror(errno) << endl;
			return EXIT_FAILURE;
		}

		cerr << "reading in log" << endl;
		bool reading_len(true);
		wrapped_buffer<uint8_t> buffer(sizeof(uint32_t));
		size_t remaining = sizeof(uint32_t);
		size_t cursor = 0;
		while(true) {
			buffer.realloc(cursor + remaining);
			ssize_t got = read(in_fd, buffer.ptr(), remaining);
			if (got > 0) {
				remaining -=  got;
			} else if (got == 0) {
				break;
			} else if (got < 0) {
				cerr << "Bad read: " << strerror(errno);
			}

			if (remaining == 0) {
				if (reading_len) {
					cursor = 0;
					remaining = ntoh(*(uint32_t*) (buffer.const_ptr()));
					reading_len = false;
				} else {
					const struct log_format *log = (const struct log_format*) buffer.const_ptr(); 
					/* endianness still in nbo order for log */
					handle_message(log);
					reading_len = true;
					remaining = 4;
				}
			}
		}

		close(in_fd);

		/* okay, now we know everyone we might want to connect to. */


		/* send a log message out */
		string root((const char*)cfg->lookup("logger.root"));
		string logpath = root + "servers";
		try {
		  g_log_buffer = new log_buffer(unix_sock_client(logpath, true));
		} catch (const network_error &e) {
		  cerr << "WARNING: Could not connect to log server! " << e.what() << endl;
		}

		time_t start_time = time(NULL);
		
		ev_now_update(ev_default_loop());
		g_log<DEBUG>("Initiating GETADDR probe");
		g_log_buffer->io_cb(g_log_buffer->io, 0);
		cerr << "Initiating GETADDR probe" << endl;


		// Killing any dupes (Oh noes, why dupes!)
		cerr << "attempting to kill dupes before run\n";
		pid_t child = fork();
		if (child == 0) {
		  const char *kill_dupes = "/home/litton/netmine/clients/kill_dupes";
		  execl(kill_dupes, kill_dupes, config_file, NULL);
		  cerr << "Failed to kill dupes!\n";
		} else if (child > 0) {
		  int count = 0;
		  sigaction(SIGCHLD, &sigact, NULL);
		  while(true) {
		    if (lastpid != child) {
		      if (count++ > 10) {
			cerr << "Manually killing kill_dupes" << endl;
			if (kill(child, SIGTERM) < 0) {
			  cerr << "Could not send SIGTERM to " << child << endl;
			}
		      }
		      sleep(1);
		    } else {
		      break;
		    }
		  }
		  
		}


		ev_now_update(ev_default_loop());
		g_log<DEBUG>("Cycling...");
		g_log_buffer->io_cb(g_log_buffer->io, 0);
		cerr << "Cycling..." << endl;
		/* call cycle and wait */
		child = fork();

		if (child == 0) {
			const char *cycle = "/home/litton/netmine/clients/cycle";
#ifndef FIND_CXN
			execl(cycle, cycle, config_file, NULL);
#endif
			cerr << "Child did not cycle!" << endl;
			exit(0);
		} else if (child > 0) {
			sigaction(SIGCHLD, &sigact, NULL);
			int count = 0;
			while(true) {
				if (lastpid != child) {
					if (count++ > 10) {
						cerr << "Manually killing cycle" << endl;
						if (kill(child, SIGTERM) < 0) {
							cerr << "Could not send SIGTERM to " << child << endl;
						}
					}
					sleep(1);
				} else {
					break;
				}
			}
		}


		/* remove existing nodes to connect to */
#ifndef FIND_CXN
		int sock = unix_sock_client((const char*)cfg->lookup("connector.control_path"), false);
	
		ev_now_update(ev_default_loop());
		g_log<DEBUG>("Fetching existing connections...");
		g_log_buffer->io_cb(g_log_buffer->io, 0);
		cerr << "Fetching existing connections..." << endl;
		get_all_cxn(sock, [&](struct ctrl::connection_info *info, size_t) {
				g_to_connect.erase(info->remote_addr);
			});
#else
		int sock(0);
#endif

		cerr << "want to connect to " << g_to_connect.size() << " people" << endl;

#ifdef FIND_CXN
		exit(0);
#endif

		/* initiate connections */
		struct sockaddr_in local_addr;
		bzero(&local_addr, sizeof(local_addr));
		local_addr.sin_family = AF_INET;
		if (inet_pton(AF_INET, "127.0.0.1", &local_addr.sin_addr) != 1) {
			perror("inet_pton source");
			return EXIT_FAILURE;
		}
		local_addr.sin_port = hton(static_cast<uint16_t>(0xdead));
		ev_now_update(ev_default_loop());
		g_log<DEBUG>("Initiating new connections...");
		g_log_buffer->io_cb(g_log_buffer->io, 0);
		cerr << "Initiating new connections..." << endl;
		int count = 0;
		for(auto it = g_to_connect.cbegin(); it != g_to_connect.cend(); ++it) {

			ctrl::easy::connect_msg message(&*it, &local_addr);
			pair<wrapped_buffer<uint8_t>, size_t> p = message.serialize();
			do_write(sock, p.first.const_ptr(), p.second);

			if ((count++ % 7001) == 0) { 
				sleep(1);
			}
		}

		cerr << "Giving connections two minutes" << endl;
		sleep(120);

		// Killing any dupes (Oh noes, why dupes!)
		cerr << "attempting to kill dupes after cycle\n";
		child = fork();
		if (child == 0) {
		  const char *kill_dupes = "/home/litton/netmine/clients/kill_dupes";
		  execl(kill_dupes, kill_dupes, config_file, NULL);
		  cerr << "Failed to kill dupes!\n";
		} else if (child > 0) {
		  int count = 0;
		  while(true) {
		    if (lastpid != child) {
		      if (count++ > 10) {
			cerr << "Manually killing kill_dupes" << endl;
			if (kill(child, SIGTERM) < 0) {
			  cerr << "Could not send SIGTERM to " << child << endl;
			}
		      }
		      sleep(1);
		    } else {
		      break;
		    }
		  }
		  
		}
	

		cerr << "Launching getaddr" << endl;
		ev_now_update(ev_default_loop());
		g_log<DEBUG>("launching getaddr program...");
		g_log_buffer->io_cb(g_log_buffer->io, 0);


		/* call getaddr and wait */
		child = fork();

		if (child == 0) {
			const char *getaddr = "/home/litton/netmine/clients/getaddr";
			execl(getaddr, getaddr, config_file, NULL);
			cerr << "Child did not getaddr!" << endl;
			exit(0);
		} else if (child > 0) {
			sigaction(SIGCHLD, &sigact, NULL);
			while(true) {
				if (lastpid != child) {
					time_t now = time(NULL);
					/* Rules for proceeding to next start time:
					 * Last must start have been 15 minutes ago
					 * and should repeat every 4 hours on the 
					 * 17th minute
					 */
					const time_t period = 240*60;
					const time_t offset = 17*60;
					const time_t minim = 10*60;
					time_t next = ((start_time+period-offset-1) / period)*period + offset;
					if (next - start_time <= minim) next += period;
					if (now >= next) {
						cerr << "Manually killing getaddr" << endl;
						if (kill(child, SIGTERM) < 0) {
							cerr << "Could not send SIGTERM to " << child << endl;
						}
					}
					cerr << "Sleeping for " << (next-now)/60 << " minutes" << endl;
					sleep(next - now);
				} else {
					break;
				}
			}
		}
		close(g_log_buffer->fd);
		delete g_log_buffer;

		filename = g_logpath + "/" + "verbatim.log";

	}

	return EXIT_SUCCESS;
}
