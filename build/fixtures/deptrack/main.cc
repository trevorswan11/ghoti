#include <cstdio>

auto header_value() -> int;
auto inc_value() -> int;
auto plain_value() -> int;

auto main() -> int {
    std::printf("%d %d %d\n", header_value(), inc_value(), plain_value());
    return 0;
}
