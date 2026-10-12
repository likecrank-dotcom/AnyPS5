#ifndef CORE_LIBS_PRX_LIBSCESYSTEMSERVICE_LOADEXEC_HPP
#define CORE_LIBS_PRX_LIBSCESYSTEMSERVICE_LOADEXEC_HPP

namespace SystemService {

[[noreturn]] void ReplaceProcess(const char* path, const char* const* arguments);

}

#endif
