#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_message.hpp>
#include <catch2/catch_test_macros.hpp>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Object/Binary.h>
#include <llvm/Object/ObjectFile.h>
#include <llvm/Support/Casting.h>
#include <llvm/Support/Error.h>

#include "compiler/codegen/llvm_scope.hh"
#include "compiler/codegen/opt_level.hh"
#include "compiler/codegen/target.hh"
#include "helpers/codegen.hh"
#include "helpers/sema.hh"
#include "support/tempfile.hh"

namespace ghoti::tests {

namespace {

// Symbols an object file references but does not define; linker-synthesized ones are fine
[[nodiscard]] auto runtime_dependencies(const std::filesystem::path& object_path)
    -> std::vector<std::string> {
    constexpr std::string_view linker_provided[]{"_GLOBAL_OFFSET_TABLE_",
                                                 "__memory_base",
                                                 "__stack_pointer",
                                                 "__table_base",
                                                 "__indirect_function_table"};

    auto binary{llvm::object::createBinary(object_path.string())};
    if (!binary) { return {"<unreadable object>"}; }
    const auto* object{llvm::dyn_cast<llvm::object::ObjectFile>(binary->getBinary())};
    if (!object) { return {"<not an object>"}; }

    std::vector<std::string> names;
    for (const auto& symbol : object->symbols()) {
        auto flags{symbol.getFlags()};
        auto name{symbol.getName()};
        if (!flags || !name) { continue; }
        if ((*flags & llvm::object::SymbolRef::SF_Undefined) == 0 || name->empty()) { continue; }
        if (std::ranges::contains(linker_provided, std::string_view{*name})) { continue; }
        names.emplace_back(name->str());
    }
    return names;
}

} // namespace

TEST_CASE("objects reference no C runtime, even after the optimizer forms mem* calls") {
    // At -O2 both loops become `memset`/`memcpy` calls, which ghoti must define itself
    constexpr std::string_view source{R"(
        pub const fill := fn(buf: []mut u8, src: []u8): usize {
            var i: usize = 0;
            while (i < buf.len) : (i += 1) { buf[i] = 0; }
            i = 0;
            while (i < buf.len and i < src.len) : (i += 1) { buf[i] = src[i]; }
            return buf[1..].len;
        };
    )"};

    for (const std::string_view triple : {"x86_64-unknown-linux-gnu",
                                          "aarch64-unknown-linux-gnu",
                                          "x86_64-pc-windows-msvc",
                                          "x86_64-apple-macos",
                                          // 32-bit `usize`; also must not need EHABI unwinding
                                          "arm-unknown-linux-gnueabihf",
                                          "riscv32-unknown-linux-gnu",
                                          "wasm32-unknown-unknown"}) {
        for (const auto level : {codegen::opt_level::O0, codegen::opt_level::O2}) {
            INFO(triple << (level == codegen::opt_level::O0 ? " -O0" : " -O2"));
            codegen::llvm_scope scope;
            llvm::LLVMContext   context;
            auto [ctx, idx]{helpers::resolve_and_check(source)};

            tempfile                obj_file{"test_freestanding.o"};
            codegen::target_options target_opts{.triple_str = std::string{triple}, .level = level};
            codegen::optimizer_options opt_opts{.level = level};
            REQUIRE(helpers::emit_object(*ctx, context, obj_file, target_opts, opt_opts));
            CHECK(runtime_dependencies(obj_file.path).empty());
        }
    }
}

} // namespace ghoti::tests
