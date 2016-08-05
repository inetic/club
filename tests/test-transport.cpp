// Copyright 2016 Peter Jankuliak
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#define BOOST_TEST_DYN_LINK
#include <boost/test/unit_test.hpp>

#include <boost/asio.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/functional/hash.hpp>
#include <transport/transport.h>
#include <debug/string_tools.h>
#include "when_all.h"

//------------------------------------------------------------------------------
// The transport tests that have the prefix 'test_transport_reliable*' should
// be tested with packet dropping enabled (needs root permissions):
//
// Insert a packet dropping rule:
// iptables -I INPUT 1 -m statistic -p udp --mode random --probability 0.5 -j DROP
//
// Delete the rule:
// iptables -D INPUT 1
//
// List rules:
// iptables -L INPUT
//------------------------------------------------------------------------------

using std::cout;
using std::endl;
using std::move;
using std::shared_ptr;
using std::make_shared;
using std::set;
using std::vector;
using boost::system::error_code;
using boost::asio::const_buffer;

using uuid             = club::uuid;
using UnreliableId     = uint32_t;
using TransmitQueue    = club::transport::TransmitQueue<UnreliableId>;
using Core             = club::transport::Core<UnreliableId>;
using Transport        = club::transport::Transport<UnreliableId>;
using udp              = boost::asio::ip::udp;

using TransportPtr = std::unique_ptr<Transport>;

namespace asio = boost::asio;

// -------------------------------------------------------------------
class Debug {
public:
  Debug() : next_map_id(0) {}

  template<class... Nodes> Debug(const Nodes&... nodes)
    : next_map_id(0) {
    map(nodes...);
  }

  template<class Node> void map(const Node& n) {
    cout << "Map(" << n.id << ")-><" << next_map_id++ << ">" << endl;
  }

  template<class Node, class... Nodes>
  void map(const Node& n, const Nodes&... rest) {
    map(n);
    map(rest...);
  }

private:
  size_t next_map_id;
};

//------------------------------------------------------------------------------
namespace std {
std::ostream& operator<<(std::ostream& os, const vector<uint8_t>& v) {
  return os << str(v);
}
} // std namespace

//------------------------------------------------------------------------------
vector<uint8_t> buf_to_vector(const_buffer buf) {
  auto p = boost::asio::buffer_cast<const uint8_t*>(buf);
  auto s = boost::asio::buffer_size(buf);
  return vector<uint8_t>(p, p + s);
}

//------------------------------------------------------------------------------
struct Node {
  uuid                                    id;
  std::map<uuid, TransportPtr>            transports;
  shared_ptr<Core>                        transport_core;
  std::function<void(uuid, const_buffer)> on_recv;

  void add_transport(uuid other_id, udp::socket s, udp::endpoint e) {
    auto t = std::make_unique<Transport>(id, move(s), e, transport_core);
    transports.emplace(std::make_pair(other_id, move(t)));
    transports[other_id]->add_target(other_id);
  }

  void broadcast_unreliable(vector<uint8_t> data) {
    auto data_id = boost::hash_value(data);

    transport_core->broadcast_unreliable(data_id, move(data));
  }

  void broadcast_reliable(vector<uint8_t> data) {
    transport_core->broadcast_reliable(move(data));
  }

  template<class OnFlush> void flush(OnFlush on_flush) {
    transport_core->flush(move(on_flush));
  }

  Node()
    : id(boost::uuids::random_generator()())
    , transport_core(make_shared<Core>( id
                                      , [this](auto s, auto b) {
                                          this->on_recv(s,b);
                                        }))
  {}
};

//------------------------------------------------------------------------------
void connect_nodes(asio::io_service& ios, Node& n1, Node& n2) {
  udp::socket s1(ios, udp::endpoint(udp::v4(), 0));
  udp::socket s2(ios, udp::endpoint(udp::v4(), 0));

  auto ep1 = s1.local_endpoint();
  auto ep2 = s2.local_endpoint();

  n1.add_transport(n2.id, move(s1), move(ep2));
  n2.add_transport(n1.id, move(s2), move(ep1));
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_one_message) {
  asio::io_service ios;

  Node n1, n2;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    c();
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_one_big_message) {
  asio::io_service ios;

  Node n1, n2;

  WhenAll when_all;

  size_t N = 5000;
  vector<uint8_t> big_message(N);

  for (size_t i = 0; i < N; i++) {
    big_message[i] = i;
  }

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), big_message);
    c();
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_unreliable(big_message);

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_two_messages) {
  asio::io_service ios;

  Node n1, n2;

  size_t counter = 0;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);

    if (++counter == 1) {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    }
    else {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({4,5,6,7}));
      c();
    }
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});
  n1.broadcast_unreliable(std::vector<uint8_t>{4,5,6,7});

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_many_messages) {
  asio::io_service ios;

  Node n1, n2;

  size_t counter = 0;

  WhenAll when_all;

  const uint8_t N = 64;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({counter++}));
    if (counter == N) c();
  });

  connect_nodes(ios, n1, n2);

  for (uint8_t i = 0; i < N; ++i) {
    n1.broadcast_unreliable(std::vector<uint8_t>{i});
  }

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_two_messages_causal) {
  asio::io_service ios;

  Node n1, n2;

  size_t counter = 0;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);

    if (counter++ == 0) {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
      n1.broadcast_unreliable(std::vector<uint8_t>{4,5,6,7});
    }
    else {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({4,5,6,7}));
      c();
    }
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_exchange) {
  asio::io_service ios;

  Node n1, n2;

  connect_nodes(ios, n1, n2);

  WhenAll when_all;

  n1.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n2.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({2,3,4,5}));
    c();
  });

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    c();
  });

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});
  n2.broadcast_unreliable(std::vector<uint8_t>{2,3,4,5});

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_one_hop) {
  asio::io_service ios;

  // n1 -> n2 -> n3
  Node n1, n2, n3;

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);

  // Setup routing tables
  n1.transports[n2.id]->add_target(n3.id);
  n3.transports[n2.id]->add_target(n1.id);

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    c();
  });

  n3.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    c();
  });

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());
  n3.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_one_hop_many_messages) {
  asio::io_service ios;

  // n1 -> n2 -> n3
  Node n1, n2, n3;

  //Debug d(n1, n2, n3);

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);

  // Setup routing tables
  n1.transports[n2.id]->add_target(n3.id);
  n3.transports[n2.id]->add_target(n1.id);

  WhenAll when_all;

  const uint8_t N = 64;
  size_t counter_n2 = 0;
  size_t counter_n3 = 0;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({counter_n2++}));
    if (counter_n2 == N) c();
  });

  n3.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({counter_n3++}));
    if (counter_n3 == N) c();
  });

  for (uint8_t i = 0; i < N; ++i) {
    n1.broadcast_unreliable(std::vector<uint8_t>{i});
  }

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());
  n3.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_two_hops) {
  asio::io_service ios;

  // n1 -> n2 -> n3 -> n4
  Node n1, n2, n3, n4;

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);
  connect_nodes(ios, n3, n4);

  // Setup routing tables
  n1.transports[n2.id]->add_target(n3.id);
  n1.transports[n2.id]->add_target(n4.id);
  n2.transports[n3.id]->add_target(n4.id);
  n3.transports[n2.id]->add_target(n1.id);
  n4.transports[n3.id]->add_target(n1.id);
  n4.transports[n3.id]->add_target(n2.id);

  WhenAll when_all;

  size_t counter = 0;

  auto on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    if (++counter == 3) c();
  });

  n2.on_recv = on_recv;
  n3.on_recv = on_recv;
  n4.on_recv = on_recv;

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());
  n3.flush(when_all.make_continuation());
  n4.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
      n4.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_two_targets) {
  asio::io_service ios;

  // n3
  // ^
  // |
  // n1 -> n2
  Node n1, n2, n3;

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n1, n3);

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    n2.flush(c);
  });

  n3.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    n3.flush(c);
  });

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_one_hop_two_targets) {
  asio::io_service ios;

  //        n3
  //        ^
  //        |
  //  n1 -> n2 -> n4
  Node n1, n2, n3, n4;

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);
  connect_nodes(ios, n2, n4);

  // Setup routing tables
  n1.transports[n2.id]->add_target(n3.id);
  n1.transports[n2.id]->add_target(n4.id);
  n3.transports[n2.id]->add_target(n1.id);
  n3.transports[n2.id]->add_target(n4.id);
  n4.transports[n2.id]->add_target(n1.id);
  n4.transports[n2.id]->add_target(n3.id);

  size_t counter = 0;

  WhenAll when_all;

  auto on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    if (++counter == 3) c();
  });

  n2.on_recv = on_recv;
  n3.on_recv = on_recv;
  n4.on_recv = on_recv;

  n1.broadcast_unreliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
      n4.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_reliable_one_message) {
  asio::io_service ios;

  Node n1, n2;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    c();
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_reliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_reliable_two_messages) {
  asio::io_service ios;

  Node n1, n2;

  size_t counter = 0;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);

    if (counter++ == 0) {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    }
    else {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({4,5,6,7}));
      c();
    }
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_reliable(std::vector<uint8_t>{0,1,2,3});
  n1.broadcast_reliable(std::vector<uint8_t>{4,5,6,7});

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
// TODO: This test fails when packet dropping is enabled.
BOOST_AUTO_TEST_CASE(test_transport_reliable_many_messages) {
  asio::io_service ios;

  Node n1, n2;

  size_t N = 100;
  size_t counter = 0;

  WhenAll when_all;

  vector<uint8_t> message(1000);

  for (size_t i = 0; i < message.size(); ++i) {
    message[i] = i;
  }

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), message);

    if (++counter == N) c();
  });

  connect_nodes(ios, n1, n2);

  for (size_t i = 0; i < N; ++i) {
    n1.broadcast_reliable(message);
  }

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_reliable_two_messages_causal) {
  asio::io_service ios;

  Node n1, n2;

  size_t counter = 0;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);

    if (counter++ == 0) {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));

      n1.broadcast_reliable(std::vector<uint8_t>{4,5,6,7});
      n1.flush(when_all.make_continuation());
    }
    else {
      BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({4,5,6,7}));
      c();
    }
  });

  connect_nodes(ios, n1, n2);

  n1.broadcast_reliable(std::vector<uint8_t>{0,1,2,3});

  n2.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_reliable_broadcast_3) {
  asio::io_service ios;

  // n1 -> n2 -> n3
  Node n1, n2, n3;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    n2.flush(c);
  });

  n3.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    n3.flush(c);
  });

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);

  // Setup routing tables
  n1.transports[n2.id]->add_target(n3.id);
  n3.transports[n2.id]->add_target(n1.id);

  n1.broadcast_reliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_reliable_broadcast_4) {
  asio::io_service ios;

  //        n3
  //        ^
  //        |
  //  n1 -> n2 -> n4

  Node n1, n2, n3, n4;

  size_t count = 0;

  WhenAll when_all;

  auto on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({0,1,2,3}));
    if (++count == 3) c();
  });

  n2.on_recv = on_recv;
  n3.on_recv = on_recv;
  n4.on_recv = on_recv;

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);
  connect_nodes(ios, n2, n4);

  // Setup routing tables
  n1.transports[n2.id]->add_target(n3.id);
  n1.transports[n2.id]->add_target(n4.id);
  n4.transports[n2.id]->add_target(n1.id);
  n4.transports[n2.id]->add_target(n3.id);
  n3.transports[n2.id]->add_target(n1.id);
  n3.transports[n2.id]->add_target(n4.id);

  n1.broadcast_reliable(std::vector<uint8_t>{0,1,2,3});

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());
  n3.flush(when_all.make_continuation());
  n4.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
      n4.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_and_reliable) {
  asio::io_service ios;

  std::srand(std::time(0));

  //  n1 -> n2

  Node n1, n2;

  size_t count = 0;

  const uint8_t N = 64;

  WhenAll when_all;

  n2.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({count++}));
    if (count == N) c();
  });

  connect_nodes(ios, n1, n2);

  for (uint8_t i = 0; i < N; ++i) {
    if (std::rand() % 2) {
      n1.broadcast_reliable(std::vector<uint8_t>{i});
    }
    else {
      n1.broadcast_unreliable(std::vector<uint8_t>{i});
    }
  }

  n1.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
    });

  ios.run();
}

//------------------------------------------------------------------------------
BOOST_AUTO_TEST_CASE(test_transport_unreliable_and_reliable_one_hop) {
  asio::io_service ios;

  std::srand(std::time(0));

  //  n1 -> n2 -> n3

  Node n1, n2, n3;

  size_t count = 0;

  const uint8_t N = 64;

  WhenAll when_all;

  n2.on_recv = [&](auto s, auto b) {};

  n3.on_recv = when_all.make_continuation([&](auto c, auto s, auto b) {
    BOOST_REQUIRE_EQUAL(s, n1.id);
    BOOST_REQUIRE_EQUAL(buf_to_vector(b), vector<uint8_t>({count++}));
    if (count == N) c();
  });

  connect_nodes(ios, n1, n2);
  connect_nodes(ios, n2, n3);

  n1.transports[n2.id]->add_target(n3.id);
  n3.transports[n2.id]->add_target(n1.id);

  for (uint8_t i = 0; i < N; ++i) {
    if (std::rand() % 2) {
      n1.broadcast_reliable(std::vector<uint8_t>{i});
    }
    else {
      n1.broadcast_unreliable(std::vector<uint8_t>{i});
    }
  }

  n1.flush(when_all.make_continuation());
  n2.flush(when_all.make_continuation());

  when_all.on_complete([&]() {
      n1.transports.clear();
      n2.transports.clear();
      n3.transports.clear();
    });

  ios.run();

  BOOST_REQUIRE_EQUAL(count, N);
}
