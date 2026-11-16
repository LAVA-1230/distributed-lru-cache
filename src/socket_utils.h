#ifndef SOCKET_UTILS_H
#define SOCKET_UTILS_H

#include <string>
#include <vector>

// All operating-system socket code lives in socket_utils.cpp.
// The rest of the project only calls these functions, so it stays plain C++.
// (Porting to Windows would only need this one .cpp file to change.)

// Maximum length of one protocol line (64 KB).
const int MAX_LINE_LENGTH = 64 * 1024;

int  create_server_socket(int port);        // socket + SO_REUSEADDR + bind 127.0.0.1 + listen; fd or -1
int  accept_client(int server_fd);          // client fd, or -1 on error/shutdown
int  connect_to(const std::string& host, int port, int timeout_ms);  // fd or -1
void set_timeout(int fd, int ms);           // receive + send timeout
void set_no_delay(int fd);                  // TCP_NODELAY
bool send_all(int fd, const std::string& data);
void close_socket(int fd);
void shutdown_socket(int fd);               // wakes a thread blocked in accept()/recv() on fd

// Opens a connection, sends one request line, reads one reply line, closes.
// Returns false if the node can't be reached or doesn't answer in time.
bool send_request(const std::string& host, int port, const std::string& request,
                  int timeout_ms, std::string& reply);

// Reads '\n'-terminated lines from a socket.
class LineReader {
public:
    explicit LineReader(int fd);
    // Returns false when the other side closed the connection, on timeout,
    // on error, or when a line is longer than MAX_LINE_LENGTH.
    bool read_line(std::string& line);
    bool was_too_long();

private:
    int fd;
    std::string buffer;   // bytes received but not returned yet
    bool too_long;
};

// Splits on spaces into at most max_parts parts. The last part keeps the rest
// of the line (including spaces), so values may contain spaces.
std::vector<std::string> split(const std::string& line, int max_parts);

#endif
