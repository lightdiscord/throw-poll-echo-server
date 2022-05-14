#include <stdexcept>
#include <sys/poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <poll.h>
#include <iostream>
#include <list>
#include <vector>
#include <memory.h>

struct Lock {
	int fd;
	short events;
	short revents;

	Lock() : fd(-1), events(0), revents(0) {
	}

	short wait(int _fd, short _events) {
		if (revents) {
			short save = revents;

			*this = Lock();
			return save;
		}

		fd = _fd;
		events = _events;
		throw this;
	}
};

struct Runnable {
	virtual ~Runnable() { }
	virtual void run() = 0;
};

struct Runner {
	std::list<Runnable*> runnables;

	void spawn(Runnable *runnable) {
		runnables.push_back(runnable);
	}

	void start() {
		while (true) {
			std::vector<Lock *> locks;

			auto it_runnable = runnables.begin();
			while (it_runnable != runnables.end()) {
				try {
					(*it_runnable)->run();
				} catch (Lock *lock) {
					it_runnable++;
					locks.push_back(lock);
					continue;
				}

				delete *it_runnable;
				it_runnable = runnables.erase(it_runnable);
			}

			std::vector<pollfd> pollfds(locks.size());

			auto it_lock = locks.begin();
			auto it_pollfd = pollfds.begin();
			while (it_lock != locks.end()) {
				it_pollfd->fd = (*it_lock)->fd;
				it_pollfd->events = (*it_lock)->events;
				it_pollfd->revents = 0;
				it_lock++;
				it_pollfd++;
			}

			if (poll(pollfds.data(), pollfds.size(), 1000) == -1) {
				throw std::runtime_error("error during poll");
			}

			it_lock = locks.begin();
			it_pollfd = pollfds.begin();
			while (it_lock != locks.end()) {
				(*it_lock)->revents = it_pollfd->revents;
				it_lock++;
				it_pollfd++;
			}
		}
	}
};

Runner runner;

struct Client: Runnable {
	sockaddr_in address;
	int fd;

	Lock lock;

	std::string buffer;

	Client(sockaddr_in address, int fd) : address(address), fd(fd) {
	}

	~Client() {
		close(fd);
	}

	void run() {
		while (true) {
			short revents = lock.wait(fd, POLLIN | POLLOUT);

			if (revents & POLLERR) {
				std::cout << "An error occured, closing the socket" << std::endl;
				return;
			}

			if (revents & POLLHUP) {
				std::cout << "The client disconnected, closing the socket" << std::endl;
				return;
			}

			if (revents & POLLIN) {
				char bytes[0x100];
				ssize_t nread = read(fd, bytes, 0x100);

				if (nread == -1) {
					std::cout << "An error occured during read, closing the socket" << std::endl;
					return;
				}

				// Nothing more to read means the client disconnected.
				if (nread == 0) {
					std::cout << "The client disconnected, closing the socket" << std::endl;
					return;
				}

				buffer.append(bytes, nread);
			}

			if (revents & POLLOUT) {
				ssize_t nwrite = write(fd, buffer.data(), buffer.size());

				if (nwrite == -1) {
					std::cout << "An error occured during write, closing the socket" << std::endl;
					return;
				}

				buffer.erase(0, nwrite);
			}
		}
	}
};

struct Server: Runnable {
	sockaddr_in address;
	int fd;

	Lock lock;

	Server(sockaddr_in address) : address(address) {
		if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
			throw std::runtime_error("error during socket creation");
		}
	}

	~Server() {
		close(fd);
	}

	void listen() {
		if (bind(fd, (const struct sockaddr*)&address, sizeof(sockaddr_in)) == -1) {
			throw std::runtime_error("error during bind");
		}

		if (::listen(fd, 5) == -1) {
			throw std::runtime_error("error during listen");
		}

		std::cout << "Listening for incoming connections" << std::endl;
	}

	void run() {
		while (true) {
			short revents = lock.wait(fd, POLLIN);

			if (revents & POLLIN) {
				std::cout << "A new connection was received!" << std::endl;

				sockaddr_in client_address;
				socklen_t client_address_len = sizeof(client_address);
				int client_fd;
				
				if ((client_fd = accept(fd, (struct sockaddr*)&client_address, &client_address_len)) == -1) {
					throw std::runtime_error("error during accept");
				}

				Client* client = new Client(client_address, client_fd);

				runner.spawn(client);
			}
		}
	}
};


int main() {
	sockaddr_in address;

	bzero(&address, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = inet_addr("127.0.0.1");
	address.sin_port = htons(8001);

	Server* server = new Server(address);

	server->listen();

	runner.spawn(server);
	runner.start();

	return 0;
}
