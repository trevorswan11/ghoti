#include <stdx/harness/hooks.hh>
#include <stdx/types.hh>

#include "compiler/codegen/llvm_scope.hh"

extern "C" {
auto harness_pre_main(i32, char**) -> void { ghoti::codegen::llvm_init_warmup(); }
auto harness_post_main(i32) -> void { ghoti::codegen::llvm_shutdown(); }
}
