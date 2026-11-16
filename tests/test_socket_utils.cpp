#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "socket_utils.h"

// Accepts one client and reads up to two lines from it.
static void read_two_lines(int server_fd, std::vector<std::string>* lines) {
    int client_fd = accept_client(server_fd);
    if (client_fd < 0) {
        return;
    }
    LineReader reader(client_fd);
    std::string line;
    for (int i = 0; i < 2 && reader.read_line(line); i++) {
        lines->push_back(line);
    }
    close_socket(client_fd);
}

TEST(SocketUtilsTest, LineSplitAcrossTwoSends) {
    int server_fd = create_server_socket(7401);
    ASSERT_GE(server_fd, 0);
    std::vector<std::string> lines;
    std::thread server(read_two_lines, server_fd, &lines);

    int fd = connect_to("127.0.0.1", 7401, 1000);
    ASSERT_GE(fd, 0);
    // One line arrives in two pieces: read_line must join them.
    send_all(fd, "PI");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    send_all(fd, "NG\n");
    // Two lines in one send: read_line must split them. Also strips "\r".
    send_all(fd, "PING\r\n");
    close_socket(fd);

    server.join();
    close_socket(server_fd);
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], "PING");
    EXPECT_EQ(lines[1], "PING");
}

TEST(SocketUtilsTest, SplitKeepsRestOfLine) {
    std::vector<std::string> parts = split("PUT k 0 hello world", 4);
    ASSERT_EQ(parts.size(), 4u);
    EXPECT_EQ(parts[0], "PUT");
    EXPECT_EQ(parts[1], "k");
    EXPECT_EQ(parts[2], "0");
    EXPECT_EQ(parts[3], "hello world");

    EXPECT_EQ(split("GET key", 3).size(), 2u);
    EXPECT_EQ(split("", 3).size(), 0u);
}

TEST(SocketUtilsTest, ConnectToClosedPortFails) {
    // Nothing listens on this port, so connect must fail quickly, not hang.
    EXPECT_LT(connect_to("127.0.0.1", 7499, 300), 0);
    std::string reply;
    EXPECT_FALSE(send_request("127.0.0.1", 7499, "PING", 300, reply));
}

TEST(SocketUtilsTest, ShutdownWakesBlockedAccept) {
    int server_fd = create_server_socket(7402);
    ASSERT_GE(server_fd, 0);
    std::vector<std::string> lines;
    std::thread server(read_two_lines, server_fd, &lines);   // blocks in accept()

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    shutdown_socket(server_fd);   // must wake accept(), or join() below hangs
    server.join();
    close_socket(server_fd);
    EXPECT_TRUE(lines.empty());
}
