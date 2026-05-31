#include "ego_runtime/version.hpp"

namespace ego_runtime {

std::string VersionString() {
#ifdef EGO_RUNTIME_GIT_HASH
    return std::string("1.0.0+") + EGO_RUNTIME_GIT_HASH;
#else
    return "1.0.0+dev";
#endif
}

}  // namespace ego_runtime
