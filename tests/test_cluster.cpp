#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>

#include "client.h"
#include "config.h"
#include "hash_ring.h"
#include "server.h"
#include "socket_utils.h"

// Builds a config with node_count nodes on ports base_port, base_port+1, ...
static ClusterConfig make_config(int base_port, int node_count) {
    ClusterConfig config;
    config.replication = 2;
    config.virtual_nodes = 100;
    for (int i = 0; i < node_count; i++) {
        NodeInfo node;
        node.id = "n" + std::to_string(i + 1);
        node.host = "127.0.0.1";
        node.port = base_port + i;
        config.nodes.push_back(node);
    }
    return config;
}

static std::string ask(int port, const std::string& request) {
    std::string reply;
    if (!send_request("127.0.0.1", port, request, 1000, reply)) {
        return "<no reply>";
    }
    return reply;
}

static void sleep_ms(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

// ---------------- Phase 5: one server ----------------

TEST(ClusterTest, SingleServerPutGet) {
    ClusterConfig config = make_config(7101, 1);
    CacheServer server("n1", config, 100);
    ASSERT_TRUE(server.start());

    // Several requests on one connection, like a real client would do.
    int fd = connect_to("127.0.0.1", 7101, 1000);
    ASSERT_GE(fd, 0);
    LineReader reader(fd);
    std::string reply;

    send_all(fd, "PUT a 0 hello world\n");
    ASSERT_TRUE(reader.read_line(reply));
    EXPECT_EQ(reply, "OK");

    send_all(fd, "GET a\n");
    ASSERT_TRUE(reader.read_line(reply));
    EXPECT_EQ(reply, "VALUE hello world");

    send_all(fd, "GET missing\nPING\nBOGUS\n");   // three requests in one send
    ASSERT_TRUE(reader.read_line(reply));
    EXPECT_EQ(reply, "NOT_FOUND");
    ASSERT_TRUE(reader.read_line(reply));
    EXPECT_EQ(reply, "PONG n1");
    ASSERT_TRUE(reader.read_line(reply));
    EXPECT_EQ(reply, "ERROR unknown command");
    close_socket(fd);

    EXPECT_EQ(ask(7101, "DEL a"), "OK");
    EXPECT_EQ(ask(7101, "GET a"), "NOT_FOUND");
    EXPECT_EQ(ask(7101, "PUT a notanumber x"), "ERROR usage: PUT <key> <ttl_ms> <value>");
    server.stop();
}

TEST(ClusterTest, LineTooLongClosesConnection) {
    ClusterConfig config = make_config(7111, 1);
    CacheServer server("n1", config, 100);
    ASSERT_TRUE(server.start());

    int fd = connect_to("127.0.0.1", 7111, 2000);
    ASSERT_GE(fd, 0);
    send_all(fd, std::string(MAX_LINE_LENGTH + 100, 'x'));   // no newline, too long
    LineReader reader(fd);
    std::string reply;
    ASSERT_TRUE(reader.read_line(reply));
    EXPECT_EQ(reply, "ERROR line too long");
    EXPECT_FALSE(reader.read_line(reply));   // server closed the connection
    close_socket(fd);
    server.stop();
}

TEST(ClusterTest, StopIsFastWithIdleClientConnected) {
    ClusterConfig config = make_config(7121, 1);
    CacheServer server("n1", config, 100);
    ASSERT_TRUE(server.start());
    int fd = connect_to("127.0.0.1", 7121, 1000);   // idle: the handler waits in recv()
    sleep_ms(50);

    auto start = std::chrono::steady_clock::now();
    server.stop();
    long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::steady_clock::now() - start).count();
    EXPECT_LT(ms, 2000);   // not the 30 s idle timeout
    close_socket(fd);
}

// ---------------- Phase 7: replication ----------------

TEST(ClusterTest, ReplicationAndFailover) {
    ClusterConfig config = make_config(7201, 3);
    CacheServer s1("n1", config, 1000);
    CacheServer s2("n2", config, 1000);
    CacheServer s3("n3", config, 1000);
    ASSERT_TRUE(s1.start());
    ASSERT_TRUE(s2.start());
    ASSERT_TRUE(s3.start());

    CacheClient client(config);
    for (int i = 0; i < 50; i++) {
        ASSERT_TRUE(client.put("key" + std::to_string(i), "value" + std::to_string(i), 0));
    }
    sleep_ms(200);   // replication is asynchronous

    // Every key must be on exactly 2 of the 3 servers.
    for (int i = 0; i < 50; i++) {
        int copies = 0;
        for (int port = 7201; port <= 7203; port++) {
            if (ask(port, "LOCAL_GET key" + std::to_string(i)) == "VALUE value" + std::to_string(i)) {
                copies++;
            }
        }
        EXPECT_EQ(copies, 2) << "key" << i;
    }

    // Stop the primary of key0: the client must fail over to the replica.
    HashRing ring(config.virtual_nodes);
    ring.add_node("n1");
    ring.add_node("n2");
    ring.add_node("n3");
    std::vector<std::string> owners = ring.get_nodes("key0", 2);
    CacheServer* servers[3] = {&s1, &s2, &s3};
    servers[owners[0][1] - '1']->stop();

    std::string value;
    std::string served_by;
    EXPECT_TRUE(client.get("key0", value, served_by));
    EXPECT_EQ(value, "value0");
    EXPECT_EQ(served_by, owners[1]);

    s1.stop();
    s2.stop();
    s3.stop();
}

TEST(ClusterTest, ForwardingAndReplicatedDelete) {
    ClusterConfig config = make_config(7211, 3);
    CacheServer s1("n1", config, 1000);
    CacheServer s2("n2", config, 1000);
    CacheServer s3("n3", config, 1000);
    ASSERT_TRUE(s1.start());
    ASSERT_TRUE(s2.start());
    ASSERT_TRUE(s3.start());

    // Send every request to n1, even for keys n1 doesn't own: it forwards them.
    for (int i = 0; i < 20; i++) {
        EXPECT_EQ(ask(7211, "PUT k" + std::to_string(i) + " 0 v"), "OK");
    }
    for (int i = 0; i < 20; i++) {
        EXPECT_EQ(ask(7211, "GET k" + std::to_string(i)), "VALUE v");
        EXPECT_EQ(ask(7212, "GET k" + std::to_string(i)), "VALUE v");
    }

    EXPECT_EQ(ask(7213, "DEL k0"), "OK");
    sleep_ms(200);
    for (int port = 7211; port <= 7213; port++) {
        EXPECT_EQ(ask(port, "LOCAL_GET k0"), "NOT_FOUND");   // gone from both copies
    }

    // An old replicated write must not overwrite a newer value.
    EXPECT_EQ(ask(7211, "REPL_PUT x 200 0 new"), "OK");
    EXPECT_EQ(ask(7211, "REPL_PUT x 100 0 old"), "OK");
    EXPECT_EQ(ask(7211, "LOCAL_GET x"), "VALUE new");
}

// ---------------- Phase 8: failure detection ----------------

TEST(ClusterTest, FailureDetection) {
    ClusterConfig config = make_config(7301, 3);
    CacheServer s1("n1", config, 100);
    CacheServer s2("n2", config, 100);
    CacheServer s3("n3", config, 100);
    ASSERT_TRUE(s1.start());
    ASSERT_TRUE(s2.start());
    ASSERT_TRUE(s3.start());
    sleep_ms(1500);
    EXPECT_EQ(ask(7301, "NODES"), "NODES n1=ALIVE n2=ALIVE n3=ALIVE");

    s3.stop();
    sleep_ms(4000);   // 3 missed pings at 1 per second
    EXPECT_EQ(ask(7301, "NODES"), "NODES n1=ALIVE n2=ALIVE n3=DEAD");
    EXPECT_EQ(ask(7302, "NODES"), "NODES n1=ALIVE n2=ALIVE n3=DEAD");

    // Restart n3 (a new server object on the same port, thanks to SO_REUSEADDR).
    CacheServer s3_again("n3", config, 100);
    ASSERT_TRUE(s3_again.start());
    sleep_ms(2000);
    EXPECT_EQ(ask(7301, "NODES"), "NODES n1=ALIVE n2=ALIVE n3=ALIVE");
    EXPECT_EQ(ask(7302, "NODES"), "NODES n1=ALIVE n2=ALIVE n3=ALIVE");
}
