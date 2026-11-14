#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gsl/span>
#include <stdx/memory.hh>
#include <stdx/option.hh>
#include <stdx/utility.hh>

#include "compiler/module/memory_loader.hh"
#include "compiler/module/module.hh"
#include "compiler/sema/cfg_evaluator.hh"
#include "compiler/sema/error.hh"
#include "compiler/uir/astgen.hh"
#include "helpers/sema.hh"

namespace ghoti::tests::helpers {

// A fresh module manager with the root and its mock imports loaded, ready to lower
struct uir_test_context {
    stdx::box<mod::memory_loader> loader;
    mod::module_manager           manager;
    sema::cfg_context             cfg;
    mod::module_t&                root;

    uir_test_context(const std::vector<mock_file>& imports,
                     const std::filesystem::path&  root_path,
                     std::string_view              input,
                     bool                          testing = false);
    ~uir_test_context() = default;
    MAKE_PINNED(uir_test_context);

    [[nodiscard]] auto lower(const mod::module_t& mod) -> uir::lowered;
};

[[nodiscard]] auto lower_uir(std::string_view              input,
                             const std::vector<mock_file>& imports = {},
                             bool                          testing = false) -> uir::lowered;

// The `--emit-uir` text of a program that must lower without errors
[[nodiscard]] auto dump_uir(std::string_view input, const std::vector<mock_file>& imports = {})
    -> std::string;

// Checks AstGen's diagnostics against `expected`, in any order
auto check_astgen_against(std::string_view                  input,
                          const std::vector<mock_file>&     imports,
                          gsl::span<const sema::diagnostic> expected) -> void;

} // namespace ghoti::tests::helpers
