#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

#include "hash_ring.h"

static std::string key_name(int i) {
    return "key:" + std::to_string(i);
}

TEST(HashRingTest, SameKeySameNode) {
    HashRing ring(100);
    ring.add_node("n1");
    ring.add_node("n2");
    ring.add_node("n3");
    for (int i = 0; i < 100; i++) {
        EXPECT_EQ(ring.get_node(key_name(i)), ring.get_node(key_name(i)));
    }
}

TEST(HashRingTest, EvenSpread) {
    HashRing ring(100);
    ring.add_node("n1");
    ring.add_node("n2");
    ring.add_node("n3");
    std::map<std::string, int> counts;
    for (int i = 0; i < 30000; i++) {
        counts[ring.get_node(key_name(i))]++;
    }
    EXPECT_EQ(counts.size(), 3u);
    for (auto it = counts.begin(); it != counts.end(); ++it) {
        EXPECT_GT(it->second, 7000) << it->first;
        EXPECT_LT(it->second, 13000) << it->first;
    }
}

TEST(HashRingTest, AddingNodeMovesFewKeysOnlyToNewNode) {
    HashRing ring(100);
    for (int i = 1; i <= 4; i++) {
        ring.add_node("n" + std::to_string(i));
    }
    std::vector<std::string> before;
    for (int i = 0; i < 30000; i++) {
        before.push_back(ring.get_node(key_name(i)));
    }

    ring.add_node("n5");
    int moved = 0;
    for (int i = 0; i < 30000; i++) {
        std::string now = ring.get_node(key_name(i));
        if (now != before[i]) {
            moved++;
            EXPECT_EQ(now, "n5");   // a key only ever moves to the new node
        }
    }
    EXPECT_LT(moved, 30000 * 30 / 100);
}

TEST(HashRingTest, GetNodesReturnsDistinctNodes) {
    HashRing ring(100);
    ring.add_node("n1");
    ring.add_node("n2");
    ring.add_node("n3");
    for (int i = 0; i < 1000; i++) {
        std::vector<std::string> nodes = ring.get_nodes(key_name(i), 2);
        ASSERT_EQ(nodes.size(), 2u);
        EXPECT_NE(nodes[0], nodes[1]);
        EXPECT_EQ(nodes[0], ring.get_node(key_name(i)));   // first one is the primary
    }
    // Asking for more nodes than exist returns each node once.
    EXPECT_EQ(ring.get_nodes("x", 5).size(), 3u);
}

TEST(HashRingTest, RemovingNodeMovesOnlyItsKeys) {
    HashRing ring(100);
    ring.add_node("n1");
    ring.add_node("n2");
    ring.add_node("n3");
    std::vector<std::string> before;
    for (int i = 0; i < 10000; i++) {
        before.push_back(ring.get_node(key_name(i)));
    }

    ring.remove_node("n2");
    for (int i = 0; i < 10000; i++) {
        std::string now = ring.get_node(key_name(i));
        EXPECT_NE(now, "n2");
        if (before[i] != "n2") {
            EXPECT_EQ(now, before[i]);   // keys of other nodes don't move
        }
    }
}

TEST(HashRingTest, EmptyRing) {
    HashRing ring(10);
    EXPECT_EQ(ring.get_node("a"), "");
    EXPECT_TRUE(ring.get_nodes("a", 2).empty());
}
