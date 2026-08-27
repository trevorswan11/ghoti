#include <sstream>
#include <string>

#include <catch2/catch_test_macros.hpp>
#include <stdx/types.hh>
#include <fmt/format.h>

#include "compiler/syntax/doc.hh"
#include "compiler/syntax/layout_engine.hh"
#include "helpers/common.hh"
#include "helpers/formatter.hh"

namespace ghoti::tests {

namespace docs = syntax::docs;

TEST_CASE("layout_engine renders plain text verbatim") {
    syntax::doc_manager m;
    CHECK(helpers::render_docs(m, m.add<docs::text>("hello")) == "hello");
}

TEST_CASE("layout_engine renders concat children in order") {
    syntax::doc_manager m;
    const auto          root{m.add<docs::concat>(
        helpers::make_vector<syntax::doc_id>(m.add<docs::text>("foo"), m.add<docs::text>("bar")))};
    CHECK(helpers::render_docs(m, root) == "foobar");
}

TEST_CASE("layout_engine indent only shifts lines that break after it") {
    syntax::doc_manager m;
    const auto          body{m.add<docs::concat>(
        helpers::make_vector<syntax::doc_id>(m.add<docs::hard_line>(), m.add<docs::text>("x")))};
    const auto          root{m.add<docs::indent>(body)};
    CHECK(helpers::render_docs(m, root) == "\n    x");
}

TEST_CASE("layout_engine indent width follows indent_spaces") {
    syntax::doc_manager m;
    const auto          body{m.add<docs::concat>(
        helpers::make_vector<syntax::doc_id>(m.add<docs::hard_line>(), m.add<docs::text>("x")))};
    const auto          root{m.add<docs::indent>(body)};
    CHECK(helpers::render_docs(m, root, 100, 2) == "\n  x");
}

TEST_CASE("layout_engine nested indents accumulate one level each") {
    syntax::doc_manager m;
    const auto          innermost{m.add<docs::concat>(
        helpers::make_vector<syntax::doc_id>(m.add<docs::hard_line>(), m.add<docs::text>("x")))};
    const auto          inner{m.add<docs::indent>(innermost)};
    const auto          root{m.add<docs::indent>(inner)};
    CHECK(helpers::render_docs(m, root) == "\n        x");
}

TEST_CASE("layout_engine writes no trailing whitespace on a blank line between indented content") {
    syntax::doc_manager m;
    const auto          body{
        m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(m.add<docs::hard_line>(),
                                                                 m.add<docs::text>("a"),
                                                                 m.add<docs::hard_line>(),
                                                                 m.add<docs::hard_line>(),
                                                                 m.add<docs::text>("b")))};
    const auto root{m.add<docs::indent>(body)};
    CHECK(helpers::render_docs(m, root) == "\n    a\n\n    b");
}

TEST_CASE("layout_engine leaves no dangling indentation after a trailing break") {
    syntax::doc_manager m;
    const auto          body{m.add<docs::concat>(
        helpers::make_vector<syntax::doc_id>(m.add<docs::text>("x"), m.add<docs::hard_line>()))};
    const auto          root{m.add<docs::indent>(body)};
    CHECK(helpers::render_docs(m, root) == "x\n");
}

TEST_CASE("layout_engine keeps a group flat when its content fits max_width") {
    syntax::doc_manager m;
    const auto          inner{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
        m.add<docs::text>("a"), m.add<docs::line_or_space>(", "), m.add<docs::text>("b")))};
    const auto          root{m.add<docs::group>(inner, false)};
    CHECK(helpers::render_docs(m, root, 100) == "a, b");
}

TEST_CASE("layout_engine breaks a group whose flat form overflows max_width") {
    syntax::doc_manager m;
    const auto          inner{
        m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(m.add<docs::text>("aaaaaaaaaa"),
                                                                 m.add<docs::line_or_space>(", "),
                                                                 m.add<docs::text>("bbbbbbbbbb")))};
    const auto root{m.add<docs::group>(inner, false)};
    CHECK(helpers::render_docs(m, root, 15) == "aaaaaaaaaa\nbbbbbbbbbb");
}

TEST_CASE("layout_engine force_break always breaks a group even if it would fit") {
    syntax::doc_manager m;
    const auto          inner{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
        m.add<docs::text>("a"), m.add<docs::line_or_space>(", "), m.add<docs::text>("b")))};
    const auto          root{m.add<docs::group>(inner, true)};
    CHECK(helpers::render_docs(m, root, 100) == "a\nb");
}

TEST_CASE("layout_engine soft_line disappears when its group stays flat") {
    syntax::doc_manager m;
    const auto          inner{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
        m.add<docs::text>("a"), m.add<docs::soft_line>(), m.add<docs::text>("b")))};
    const auto          root{m.add<docs::group>(inner, false)};
    CHECK(helpers::render_docs(m, root, 100) == "ab");
}

TEST_CASE("layout_engine soft_line becomes a break when its group breaks") {
    syntax::doc_manager m;
    const auto          inner{
        m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(m.add<docs::text>("aaaaaaaaaa"),
                                                                 m.add<docs::soft_line>(),
                                                                 m.add<docs::text>("bbbbbbbbbb")))};
    const auto root{m.add<docs::group>(inner, false)};
    CHECK(helpers::render_docs(m, root, 15) == "aaaaaaaaaa\nbbbbbbbbbb");
}

TEST_CASE("layout_engine hard_line always breaks, even inside a group that otherwise fits") {
    syntax::doc_manager m;
    const auto          inner{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
        m.add<docs::text>("a"), m.add<docs::hard_line>(), m.add<docs::text>("b")))};
    const auto          root{m.add<docs::group>(inner, false)};
    CHECK(helpers::render_docs(m, root, 100) == "a\nb");
}

TEST_CASE("layout_engine lets a nested group stay flat while its parent breaks") {
    syntax::doc_manager m;
    const auto          inner_group{m.add<docs::group>(
        m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
            m.add<docs::text>("x"), m.add<docs::line_or_space>(" "), m.add<docs::text>("y"))),
        false)};
    const auto          outer{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
        m.add<docs::text>("aaaaaaaaaa"), m.add<docs::line_or_space>(", "), inner_group))};
    const auto          root{m.add<docs::group>(outer, false)};

    CHECK(helpers::render_docs(m, root, 14) == "aaaaaaaaaa\nx y");
}

TEST_CASE("layout_engine align shifts the indent level of its child by the current column") {
    syntax::doc_manager m;
    const auto          label{m.add<docs::text>("key: ")};
    const auto          aligned_body{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(
        m.add<docs::hard_line>(), m.add<docs::text>("value")))};
    const auto          aligned{m.add<docs::align>(aligned_body, static_cast<u16>(0))};
    const auto root{m.add<docs::concat>(helpers::make_vector<syntax::doc_id>(label, aligned))};

    CHECK(helpers::render_docs(m, root) == fmt::format("key: \n{:{}}value", "", 5));
}

TEST_CASE("layout_engine renders all doc_manager roots in sequence") {
    syntax::doc_manager m;
    const auto          r1{m.add<docs::text>("hello ")};
    const auto          r2{m.add<docs::text>("world")};
    m.add_root(r1);
    m.add_root(r2);

    std::ostringstream os;
    syntax::layout_engine{m}.render(os);
    CHECK(os.str() == "hello world");
}

} // namespace ghoti::tests
