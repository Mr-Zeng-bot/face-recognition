// 仅供 clangd 代码模型使用（由 .clangd 强制包含），不参与 MSVC 编译，也不进产物。
//
// 背景：Qt Creator 的 clangd 集成会抹掉 __clang__（-U__clang__），MSVC 14.5x 的
// __msvc_int128.hpp 于是走 MSVC 分支去调用 _udiv128 / _div128。这两个是 MSVC 专有
// 内置函数，声明在 MSVC 的 <intrin0.inl.h> 里；而 clang 编译时 #include <intrin.h>
// 命中的是 clang 自带的那份，里面没有它们，代码模型因此报
// "use of undeclared identifier '_udiv128'"。这里按 MSVC 的声明形式补上，签名一致。
#if defined(_M_X64) && !defined(_UDIV128_DECLARED_BY_SHIM)
#define _UDIV128_DECLARED_BY_SHIM

extern "C" unsigned __int64 __cdecl _udiv128(unsigned __int64 _HighDividend,
                                             unsigned __int64 _LowDividend,
                                             unsigned __int64 _Divisor,
                                             unsigned __int64* _Remainder);
extern "C" __int64 __cdecl _div128(__int64 _HighDividend,
                                   __int64 _LowDividend,
                                   __int64 _Divisor,
                                   __int64* _Remainder);

#endif // _M_X64
