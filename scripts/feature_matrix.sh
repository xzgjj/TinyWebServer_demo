#!/usr/bin/env bash
# C++ 标准特性矩阵探针：逐特性 -fsyntax-only 试编译，判定"该工具链是否真支持"。
# 用途：doc/cxx_standard_matrix.md 的白名单依据（门禁规则 STD-001）。
# 用法: bash scripts/feature_matrix.sh <编译器> <标准>
set -u
CXX="${1:-g++}"
STD="${2:-c++23}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

check() {
  local name="$1" code="$2"
  printf '%s\n' "$code" > "$TMP/t.cpp"
  if "$CXX" -std="$STD" -fsyntax-only "$TMP/t.cpp" >"$TMP/out.txt" 2>&1; then
    printf 'OK    %s\n' "$name"
  else
    printf 'FAIL  %s\n' "$name"
  fi
}

echo "## compiler: $("$CXX" --version | head -1)  std=$STD"
printf '%s\n' '#include <cstdio>
int main(){ std::printf("__cplusplus=%ld\n", (long)__cplusplus); }' > "$TMP/v.cpp"
"$CXX" -std="$STD" "$TMP/v.cpp" -o "$TMP/v" >/dev/null 2>&1 && "$TMP/v" || echo "__cplusplus=unavailable"

check "c++17:structured_bindings"   'int main(){int a[2]={1,2}; auto [x,y]=a; return x+y;}'
check "c++17:if_constexpr"          'int main(){ if constexpr(true) return 0; return 1; }'
check "c++17:optional"              '#include <optional>
int main(){ std::optional<int> o=1; return o.value(); }'
check "c++17:filesystem"            '#include <filesystem>
int main(){ std::filesystem::path p{"."}; return (int)p.string().size(); }'
check "c++17:string_view"           '#include <string_view>
int main(){ std::string_view v="x"; return (int)v.size(); }'
check "c++20:concepts"              '#include <concepts>
template<std::integral T> int f(T){return 0;}
int main(){ return f(1); }'
check "c++20:span"                  '#include <span>
int main(){ int a[2]{}; std::span<int> s{a}; return (int)s.size(); }'
check "c++20:jthread"               '#include <thread>
int main(){ std::jthread t{[]{}}; return 0; }'
check "c++20:source_location"       '#include <source_location>
int main(){ auto l=std::source_location::current(); return (int)l.line()>0?0:1; }'
check "c++20:format"                '#include <format>
int main(){ return (int)std::format("{}",1).size(); }'
check "c++20:designated_init"       'struct P{int x;int y;};
int main(){ P p{.x=1,.y=2}; return p.x+p.y; }'
check "c++20:bit_cast"              '#include <bit>
int main(){ float f=1.f; return (int)std::bit_cast<unsigned>(f); }'
check "c++20:starts_ends_with"      '#include <string>
int main(){ std::string s="ab"; return s.starts_with("a")?0:1; }'
check "c++21:to_underlying"         '#include <utility>
enum class E:int{A=1};
int main(){ return std::to_underlying(E::A); }'
check "c++23:expected"              '#include <expected>
int main(){ std::expected<int,int> e{1}; return e.value(); }'
check "c++23:string_contains"       '#include <string>
int main(){ std::string s="ab"; return s.contains("a")?0:1; }'
check "c++23:byteswap"              '#include <bit>
int main(){ return (int)std::byteswap(1u); }'
check "c++23:print"                 '#include <print>
int main(){ std::print("x"); return 0; }'
check "c++23:mdspan"                '#include <mdspan>
int main(){ int a[4]{}; std::mdspan<int,std::extents<size_t,2,2>> m{a}; return (int)m.rank(); }'
check "c++23:stacktrace"            '#include <stacktrace>
int main(){ auto s=std::stacktrace::current(); return (int)s.size(); }'
check "c++23:flat_map"              '#include <flat_map>
int main(){ std::flat_map<int,int> m; m[1]=1; return (int)m.size(); }'
check "c++23:unreachable"           '#include <utility>
int main(){ if(false) std::unreachable(); return 0; }'
check "c++23:multidim_subscript"    'struct M{int operator[](int,int){return 0;}};
int main(){ M m; return m[1,2]; }'
check "c++23:deducing_this"         'struct S{ template<class Self> int f(this Self&&){return 0;} };
int main(){ S s; return s.f(); }'
check "c++23:if_consteval"          'constexpr int f(){ if consteval { return 1;} else {return 2;} }
int main(){ return f()-1; }'
