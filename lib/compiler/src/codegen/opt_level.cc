#include "compiler/codegen/opt_level.hh"

#include <string_view>
#include <utility>

#include <stdx/option.hh>

#include "support/string_utils.hh"

namespace ghoti::codegen {

namespace {

constexpr auto OPT_LEVEL_PARSE_MAP{
    string_utils::make_constexpr_map<opt_level>(std::pair{"0", opt_level::O0},
                                                std::pair{"O0", opt_level::O0},
                                                std::pair{"-O0", opt_level::O0},
                                                std::pair{"o0", opt_level::O0},
                                                std::pair{"1", opt_level::O1},
                                                std::pair{"O1", opt_level::O1},
                                                std::pair{"-O1", opt_level::O1},
                                                std::pair{"o1", opt_level::O1},
                                                std::pair{"2", opt_level::O2},
                                                std::pair{"O2", opt_level::O2},
                                                std::pair{"-O2", opt_level::O2},
                                                std::pair{"o2", opt_level::O2},
                                                std::pair{"3", opt_level::O3},
                                                std::pair{"O3", opt_level::O3},
                                                std::pair{"-O3", opt_level::O3},
                                                std::pair{"o3", opt_level::O3},
                                                std::pair{"s", opt_level::Os},
                                                std::pair{"Os", opt_level::Os},
                                                std::pair{"-Os", opt_level::Os},
                                                std::pair{"os", opt_level::Os},
                                                std::pair{"z", opt_level::Oz},
                                                std::pair{"Oz", opt_level::Oz},
                                                std::pair{"-Oz", opt_level::Oz},
                                                std::pair{"oz", opt_level::Oz})};

} // namespace

auto parse_opt_level(std::string_view str) noexcept -> stdx::option<opt_level> {
    return OPT_LEVEL_PARSE_MAP.get_opt(str).materialize();
}

} // namespace ghoti::codegen
