#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>
#include <stdx/utility.hh>

#include "compiler/syntax/doc.hh"

namespace ghoti::tests {

namespace docs = syntax::docs;

TEST_CASE("doc_manager retrieves the exact data a doc was constructed with") {
    syntax::doc_manager m;
    const auto          id{m.add<docs::text>(std::string_view{"hello"})};

    REQUIRE(m[id].is<docs::text>());
    CHECK(m[id].as<docs::text>().text == "hello");
}

TEST_CASE("doc_manager assigns a distinct id to every added doc") {
    syntax::doc_manager m;
    const auto          first{m.add<docs::text>(std::string_view{"a"})};
    const auto          second{m.add<docs::text>(std::string_view{"b"})};

    CHECK(first != second);
    CHECK(m[first].as<docs::text>().text == "a");
    CHECK(m[second].as<docs::text>().text == "b");
}

TEST_CASE("doc_manager total_doc_count tracks every doc added, not just roots") {
    syntax::doc_manager m;
    CHECK(m.total_doc_count() == 0);

    DISCARD(m.add<docs::text>(std::string_view{"a"}));
    DISCARD(m.add<docs::text>(std::string_view{"b"}));
    CHECK(m.total_doc_count() == 2);
}

TEST_CASE("doc_manager roots only contains docs explicitly added as roots") {
    syntax::doc_manager m;
    const auto          child{m.add<docs::text>(std::string_view{"child"})};
    CHECK(m.empty());

    m.add_root(child);
    REQUIRE(m.size() == 1);
    CHECK(*m.begin() == child);
}

} // namespace ghoti::tests
