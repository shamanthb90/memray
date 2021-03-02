#include <cassert>
#include <cstdio>

#include "hooks.h"
#include "tracking_api.h"

namespace pensieve::hooks {

int
phdr_symfind_callback(dl_phdr_info* info, [[maybe_unused]] size_t size, void* data) noexcept
{
    auto result = reinterpret_cast<symbol_query*>(data);

    // From all maps without name, we only want to visit the executable (first map)
    if (result->maps_visited++ != 0 && !info->dlpi_name[0]) {
        return 0;
    }

    if (strstr(info->dlpi_name, "linux-vdso.so.1")) {
        // This is an evil place that don't have symbols
        return 0;
    }

    for (auto phdr = info->dlpi_phdr, end = phdr + info->dlpi_phnum; phdr != end; ++phdr) {
        if (phdr->p_type != PT_DYNAMIC) {
            continue;
        }

        const auto* dyn = reinterpret_cast<const Dyn*>(phdr->p_vaddr + info->dlpi_addr);
        SymbolTable symbols(info->dlpi_addr, dyn);

        const auto offset = symbols.getSymbolAddress(result->symbol_name);
        if (offset == 0) {
            continue;
        }

        result->address = reinterpret_cast<void*>(offset);
        return 1;
    }

    return 0;
}

AllocatorKind
allocatorKind(const Allocator& allocator)
{
    switch (allocator) {
        case Allocator::CALLOC:
        case Allocator::MALLOC:
        case Allocator::MEMALIGN:
        case Allocator::POSIX_MEMALIGN:
        case Allocator::PVALLOC:
        case Allocator::REALLOC:
        case Allocator::VALLOC: {
            return AllocatorKind::SIMPLE_ALLOCATOR;
        }
        case Allocator::FREE: {
            return AllocatorKind::SIMPLE_DEALLOCATOR;
        }
        case Allocator::MMAP: {
            return AllocatorKind::RANGED_ALLOCATOR;
        }
        case Allocator::MUNMAP: {
            return AllocatorKind::RANGED_DEALLOCATOR;
        }
    }
    __builtin_unreachable();
}

SymbolHook<decltype(&::malloc)> malloc("malloc", &::malloc);
SymbolHook<decltype(&::free)> free("free", &::free);
SymbolHook<decltype(&::calloc)> calloc("calloc", &::calloc);
SymbolHook<decltype(&::realloc)> realloc("realloc", &::realloc);
SymbolHook<decltype(&::posix_memalign)> posix_memalign("posix_memalign", &::posix_memalign);
SymbolHook<decltype(&::memalign)> memalign("memalign", &::memalign);
SymbolHook<decltype(&::valloc)> valloc("valloc", &::valloc);
SymbolHook<decltype(&::pvalloc)> pvalloc("pvalloc", &::pvalloc);
SymbolHook<decltype(&::dlopen)> dlopen("dlopen", &::dlopen);
SymbolHook<decltype(&::dlclose)> dlclose("dlclose", &::dlclose);
SymbolHook<decltype(&::mmap)> mmap("mmap", &::mmap);
SymbolHook<decltype(&::mmap64)> mmap64("mmap64", &::mmap64);
SymbolHook<decltype(&::munmap)> munmap("munmap", &::munmap);
SymbolHook<decltype(&::PyGILState_Ensure)> PyGILState_Ensure("PyGILState_Ensure", &::PyGILState_Ensure);

void
ensureAllHooksAreValid()
{
    malloc.ensureValidOriginalSymbol();
    free.ensureValidOriginalSymbol();
    calloc.ensureValidOriginalSymbol();
    realloc.ensureValidOriginalSymbol();
    posix_memalign.ensureValidOriginalSymbol();
    memalign.ensureValidOriginalSymbol();
    valloc.ensureValidOriginalSymbol();
    pvalloc.ensureValidOriginalSymbol();
    dlopen.ensureValidOriginalSymbol();
    dlclose.ensureValidOriginalSymbol();
    mmap.ensureValidOriginalSymbol();
    mmap64.ensureValidOriginalSymbol();
    munmap.ensureValidOriginalSymbol();
    PyGILState_Ensure.ensureValidOriginalSymbol();
}

}  // namespace pensieve::hooks

namespace pensieve::intercept {

void*
mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) noexcept
{
    assert(hooks::mmap);
    void* ptr = hooks::mmap(addr, length, prot, flags, fd, offset);
    tracking_api::Tracker::getTracker()->trackAllocation(ptr, length, hooks::Allocator::MMAP);
    return ptr;
}

void*
mmap64(void* addr, size_t length, int prot, int flags, int fd, off_t offset) noexcept
{
    assert(hooks::mmap64);
    void* ptr = hooks::mmap64(addr, length, prot, flags, fd, offset);
    tracking_api::Tracker::getTracker()->trackAllocation(ptr, length, hooks::Allocator::MMAP);
    return ptr;
}

int
munmap(void* addr, size_t length) noexcept
{
    assert(hooks::munmap);
    tracking_api::Tracker::getTracker()->trackDeallocation(addr, length, hooks::Allocator::MUNMAP);
    return hooks::munmap(addr, length);
}

void*
malloc(size_t size) noexcept
{
    assert(hooks::malloc);

    void* ptr = hooks::malloc(size);
    tracking_api::Tracker::getTracker()->trackAllocation(ptr, size, hooks::Allocator::MALLOC);
    return ptr;
}

void
free(void* ptr) noexcept
{
    assert(hooks::free);

    // We need to call our API before we call the real free implementation
    // to make sure that the pointer is not reused in-between.
    tracking_api::Tracker::getTracker()->trackDeallocation(ptr, 0, hooks::Allocator::FREE);

    hooks::free(ptr);
}

void*
realloc(void* ptr, size_t size) noexcept
{
    assert(hooks::realloc);

    void* ret = hooks::realloc(ptr, size);
    if (ret) {
        tracking_api::Tracker::getTracker()->trackDeallocation(ptr, 0, hooks::Allocator::FREE);
        tracking_api::Tracker::getTracker()->trackAllocation(ret, size, hooks::Allocator::REALLOC);
    }
    return ret;
}

void*
calloc(size_t num, size_t size) noexcept
{
    assert(hooks::calloc);

    void* ret = hooks::calloc(num, size);
    if (ret) {
        tracking_api::Tracker::getTracker()->trackAllocation(ret, num * size, hooks::Allocator::CALLOC);
    }
    return ret;
}

int
posix_memalign(void** memptr, size_t alignment, size_t size) noexcept
{
    assert(hooks::posix_memalign);

    int ret = hooks::posix_memalign(memptr, alignment, size);
    if (!ret) {
        tracking_api::Tracker::getTracker()->trackAllocation(
                *memptr,
                size,
                hooks::Allocator::POSIX_MEMALIGN);
    }
    return ret;
}

void*
memalign(size_t alignment, size_t size) noexcept
{
    assert(hooks::memalign);

    void* ret = hooks::memalign(alignment, size);
    if (ret) {
        tracking_api::Tracker::getTracker()->trackAllocation(ret, size, hooks::Allocator::MEMALIGN);
    }
    return ret;
}

void*
valloc(size_t size) noexcept
{
    assert(hooks::valloc);

    void* ret = hooks::valloc(size);
    if (ret) {
        tracking_api::Tracker::getTracker()->trackAllocation(ret, size, hooks::Allocator::VALLOC);
    }
    return ret;
}

void*
pvalloc(size_t size) noexcept
{
    assert(hooks::pvalloc);

    void* ret = hooks::pvalloc(size);
    if (ret) {
        tracking_api::Tracker::getTracker()->trackAllocation(ret, size, hooks::Allocator::PVALLOC);
    }
    return ret;
}

void*
dlopen(const char* filename, int flag) noexcept
{
    assert(hooks::dlopen);

    void* ret = hooks::dlopen(filename, flag);
    if (ret) tracking_api::Tracker::getTracker()->invalidate_module_cache();
    return ret;
}

int
dlclose(void* handle) noexcept
{
    assert(hooks::dlclose);

    int ret = hooks::dlclose(handle);
    tracking_api::NativeTrace::flushCache();
    if (!ret) tracking_api::Tracker::getTracker()->invalidate_module_cache();
    return ret;
}

PyGILState_STATE
PyGILState_Ensure() noexcept
{
    PyGILState_STATE ret = hooks::PyGILState_Ensure();
    tracking_api::install_trace_function();
    return ret;
}

}  // namespace pensieve::intercept
