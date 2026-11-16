#include "socket_utils.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>

int create_server_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        std::cerr << "socket() failed: " << strerror(errno) << std::endl;
        return -1;
    }

    // SO_REUSEADDR: a restarted node can bind the same port immediately.
    // Without it, bind fails with "Address already in use" for about 60 s
    // while old connections are still in the TIME_WAIT state.
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind() on port " << port << " failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }
    if (listen(fd, 128) < 0) {
        std::cerr << "listen() failed: " << strerror(errno) << std::endl;
        close(fd);
        return -1;
    }
    return fd;
}

int accept_client(int server_fd) {
    while (true) {
        int client_fd = accept(server_fd, nullptr, nullptr);
        if (client_fd >= 0) {
            set_no_delay(client_fd);
            return client_fd;
        }
        // A signal interrupted accept(): just try again.
        if (errno == EINTR) {
            continue;
        }
        // Any other error, including the listening socket being shut down.
        return -1;
    }
}

int connect_to(const std::string& host, int port, int timeout_ms) {
    sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    // Only numeric IPv4 addresses (like 127.0.0.1) are supported, no DNS lookup.
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        return -1;
    }

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    // Set the timeout before connect(): on Linux SO_SNDTIMEO also limits how
    // long connect() waits, so a dead host can't block us forever.
    set_timeout(fd, timeout_ms);
    set_no_delay(fd);

    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void set_timeout(int fd, int ms) {
    // Timeouts stop a dead or slow peer from blocking a thread forever.
    timeval tv;
    tv.tv_sec = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

void set_no_delay(int fd) {
    // TCP_NODELAY turns off Nagle's algorithm. Otherwise the kernel may hold a
    // small request for up to ~40 ms hoping to batch it with more data.
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
}

bool send_all(int fd, const std::string& data) {
    size_t sent_total = 0;
    // send() may send only part of the data, so loop until everything is sent.
    while (sent_total < data.size()) {
        // MSG_NOSIGNAL: writing to a closed socket returns an error (EPIPE)
        // instead of raising SIGPIPE, which would kill the whole program.
        ssize_t n = send(fd, data.data() + sent_total, data.size() - sent_total, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;   // interrupted by a signal: retry
            }
            return false;
        }
        sent_total += (size_t)n;
    }
    return true;
}

void close_socket(int fd) {
    if (fd >= 0) {
        close(fd);
    }
}

void shutdown_socket(int fd) {
    // On Linux, close() alone does not reliably wake a thread blocked in
    // accept() or recv() on this socket. shutdown() does.
    if (fd >= 0) {
        shutdown(fd, SHUT_RDWR);
    }
}

bool send_request(const std::string& host, int port, const std::string& request,
                  int timeout_ms, std::string& reply) {
    int fd = connect_to(host, port, timeout_ms);
    if (fd < 0) {
        return false;
    }
    bool ok = send_all(fd, request + "\n");
    if (ok) {
        LineReader reader(fd);
        ok = reader.read_line(reply);
    }
    close_socket(fd);   // every socket we open is closed
    return ok;
}

LineReader::LineReader(int fd) : fd(fd), too_long(false) {
}

bool LineReader::read_line(std::string& line) {
    // TCP is a stream of bytes, not messages: one recv() can return half a
    // line or two lines at once. So keep a buffer and cut lines out of it.
    char chunk[4096];
    while (true) {
        size_t newline_pos = buffer.find('\n');
        if (newline_pos != std::string::npos) {
            line = buffer.substr(0, newline_pos);
            buffer.erase(0, newline_pos + 1);
            // Accept "\r\n" too, in case the client (e.g. some telnet) sends it.
            if (!line.empty() && line[line.size() - 1] == '\r') {
                line.erase(line.size() - 1);
            }
            return true;
        }
        if ((int)buffer.size() > MAX_LINE_LENGTH) {
            too_long = true;
            return false;
        }

        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            buffer.append(chunk, (size_t)n);
        } else if (n == 0) {
            return false;   // the other side closed the connection
        } else if (errno == EINTR) {
            continue;       // interrupted by a signal: retry
        } else {
            return false;   // EAGAIN/EWOULDBLOCK means timeout; anything else is an error
        }
    }
}

bool LineReader::was_too_long() {
    return too_long;
}

std::vector<std::string> split(const std::string& line, int max_parts) {
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos < line.size()) {
        // Skip spaces between parts
        while (pos < line.size() && line[pos] == ' ') {
            pos++;
        }
        if (pos >= line.size()) {
            break;
        }
        // The last part takes the rest of the line, spaces included
        if ((int)parts.size() == max_parts - 1) {
            parts.push_back(line.substr(pos));
            break;
        }
        size_t end = line.find(' ', pos);
        if (end == std::string::npos) {
            end = line.size();
        }
        parts.push_back(line.substr(pos, end - pos));
        pos = end;
    }
    return parts;
}
