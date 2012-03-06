#define BOOST_TEST_MODULE buffer test
#include <boost/test/unit_test.hpp>
#include "buffer.hh"

using namespace ten;

BOOST_AUTO_TEST_CASE(test1) {
    static std::vector<uint8_t> aa(500, 0xAA);
    static std::vector<uint8_t> bb(1000, 0xBB);
    BOOST_CHECK(std::is_pod<buffer2::head>::value);

    buffer2 b(100);
    b.reserve(1000); // force a realloc
    BOOST_CHECK(b.end() - b.back() >= 1000);
    std::fill(b.back(), b.end(1000), 0xAA);
    b.commit(1000);
    BOOST_CHECK_EQUAL(1000, b.size());
    b.remove(500);
    b.reserve(1000);
    BOOST_CHECK(b.end() - b.back() >= 1000);
    BOOST_CHECK(std::equal(b.front(), b.back(), aa.begin()));
    std::fill(b.front(), b.end(500), 0xBB);
    b.commit(500);
    BOOST_CHECK_EQUAL(1000, b.size());
    BOOST_CHECK(std::equal(b.front(), b.back(), bb.begin()));
    b.remove(500);
    b.reserve(1000);
    BOOST_CHECK(b.end() - b.back() >= 1000);
    BOOST_CHECK_THROW(b.remove(20000), std::runtime_error);
    BOOST_CHECK_THROW(b.commit(20000), std::runtime_error);
}

BOOST_AUTO_TEST_CASE(test2) {
    // this is just to see if realloc is returning new pointers
    {
        buffer2 b(100000);
        b.reserve(100001);
    }

    {
        buffer2 b(100000);
        b.reserve(100001);
    }
}
