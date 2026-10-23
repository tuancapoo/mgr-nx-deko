#pragma once

namespace rex::runtime {
class FunctionDispatcher;
}

namespace mgr {

// Registers the guest functions the code generator did not find (mgr_missing_functions.cpp).
void RegisterMissingFunctions(rex::runtime::FunctionDispatcher* dispatcher);

}  // namespace mgr
