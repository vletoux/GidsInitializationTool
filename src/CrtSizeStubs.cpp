/*++

Copyright (C) MySmartLogon, All Rights Reserved.

Module Name:

    CrtSizeStubs.cpp

Abstract:

    Stub implementations of __std_type_info_* functions that prevent
    the linker from pulling in std_type_info.obj (libvcruntime) and
    its transitive dependency on undname.obj (~100 KB of C++ name
    demangling code that is never called at runtime).

    __std_type_info_compare uses strcmp on the decorated (mangled)
    names so that dynamic_cast continues to work correctly.

--*/

#include <stddef.h>
#include <string.h>

extern "C" {

struct __std_type_info_data {
    const char* _UndecoratedName;
    char        _DecoratedName[1];
};

int __cdecl __std_type_info_compare(
    const __std_type_info_data* lhs,
    const __std_type_info_data* rhs)
{
    if (lhs == rhs) return 0;
    return strcmp(lhs->_DecoratedName + 1, rhs->_DecoratedName + 1);
}

void __cdecl __std_type_info_destroy_list(void*) {}

const char* __cdecl __std_type_info_name(void*, void*) { return ""; }

size_t __cdecl __std_type_info_hash(const __std_type_info_data* d) {
    size_t h = 0;
    for (const char* p = d->_DecoratedName + 1; *p; ++p)
        h = h * 131 + (unsigned char)*p;
    return h;
}

} // extern "C"
