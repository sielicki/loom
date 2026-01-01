// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/concepts/provider_traits.hpp>
#include <loom/core/memory.hpp>

#include "ut.hpp"

using namespace boost::ut;

namespace loom {

template <provider_tag Provider>
struct provider_remote_memory_test {
    std::uint64_t addr{0};
    std::uint64_t key{0};
    std::size_t length{0};

    constexpr provider_remote_memory_test() noexcept = default;

    constexpr provider_remote_memory_test(std::uint64_t addr_,
                                          std::uint64_t key_,
                                          std::size_t len_) noexcept
        : addr(addr_), key(key_), length(len_) {}

    explicit provider_remote_memory_test(const remote_memory& rm) noexcept
        : addr(rm.addr), key(rm.key), length(rm.length) {}

    [[nodiscard]] constexpr auto to_generic() const noexcept -> remote_memory {
        return remote_memory{addr, key, length};
    }
};

}  // namespace loom

suite provider_aware_suite = [] {
    "provider_remote_memory default construction"_test = [] {
        loom::provider_remote_memory_test<loom::provider::verbs_tag> rm;

        expect(rm.addr == 0UL);
        expect(rm.key == 0UL);
        expect(rm.length == 0UL);
    };

    "provider_remote_memory value construction"_test = [] {
        loom::provider_remote_memory_test<loom::provider::verbs_tag> rm{0x1000, 0xABCD, 4096};

        expect(rm.addr == 0x1000UL);
        expect(rm.key == 0xABCDUL);
        expect(rm.length == 4096UL);
    };

    "provider_remote_memory from generic"_test = [] {
        loom::remote_memory generic{0x2000, 0x5678, 8192};
        loom::provider_remote_memory_test<loom::provider::efa_tag> rm{generic};

        expect(rm.addr == 0x2000UL);
        expect(rm.key == 0x5678UL);
        expect(rm.length == 8192UL);
    };

    "provider_remote_memory to_generic"_test = [] {
        loom::provider_remote_memory_test<loom::provider::slingshot_tag> rm{0x3000, 0xDEAD, 16384};
        auto generic = rm.to_generic();

        expect(generic.addr == 0x3000UL);
        expect(generic.key == 0xDEADUL);
        expect(generic.length == 16384UL);
    };

    "provider_traits static properties verbs"_test = [] {
        using traits = loom::provider_traits<loom::provider::verbs_tag>;

        expect(traits::max_inject_size > 0UL) << "verbs should have inject size";
        expect(traits::supports_native_atomics) << "verbs should support native atomics";
        expect(!traits::uses_staged_atomics) << "verbs should not require staged atomics";

        expect(std::string_view{traits::provider_name()} == "verbs");
    };

    "provider_traits static properties efa"_test = [] {
        using traits = loom::provider_traits<loom::provider::efa_tag>;

        expect(traits::max_inject_size > 0UL) << "efa should have inject size";
        expect(!traits::supports_native_atomics) << "efa should not support native atomics";
        expect(traits::uses_staged_atomics) << "efa should use staged atomics";

        expect(std::string_view{traits::provider_name()} == "efa");
    };

    "provider_traits static properties slingshot"_test = [] {
        using traits = loom::provider_traits<loom::provider::slingshot_tag>;

        expect(traits::max_inject_size > 0UL) << "slingshot should have inject size";
        expect(traits::supports_native_atomics) << "slingshot should support native atomics";

        expect(std::string_view{traits::provider_name()} == "cxi");
    };

    "provider_traits static properties shm"_test = [] {
        using traits = loom::provider_traits<loom::provider::shm_tag>;

        expect(traits::max_inject_size > 0UL) << "shm should have inject size";
        expect(traits::supports_native_atomics) << "shm should support native atomics";
        expect(!traits::uses_staged_atomics) << "shm should not require staged atomics";

        expect(std::string_view{traits::provider_name()} == "shm");
    };

    "provider_traits static properties tcp"_test = [] {
        using traits = loom::provider_traits<loom::provider::tcp_tag>;

        expect(!traits::supports_native_atomics) << "tcp should not support native atomics";
        expect(traits::uses_staged_atomics) << "tcp should require staged atomics";

        expect(std::string_view{traits::provider_name()} == "tcp");
    };

    "can_inject helper"_test = [] {
        expect(loom::can_inject<loom::provider::verbs_tag>(64))
            << "verbs should be able to inject 64 bytes";
        expect(loom::can_inject<loom::provider::efa_tag>(32))
            << "efa should be able to inject 32 bytes";
        expect(!loom::can_inject<loom::provider::efa_tag>(64))
            << "efa should not inject 64 bytes (max is 32)";

        expect(!loom::can_inject<loom::provider::verbs_tag>(1000000)) << "should not inject 1MB";
    };

    "provider_remote_memory roundtrip"_test = [] {
        loom::remote_memory original{0x12345678, 0xDEADBEEF, 65536};

        loom::provider_remote_memory_test<loom::provider::verbs_tag> typed{original};
        auto back = typed.to_generic();

        expect(back.addr == original.addr);
        expect(back.key == original.key);
        expect(back.length == original.length);
    };

    "provider_tag concept"_test = [] {
        static_assert(loom::provider_tag<loom::provider::verbs_tag>);
        static_assert(loom::provider_tag<loom::provider::efa_tag>);
        static_assert(loom::provider_tag<loom::provider::slingshot_tag>);
        static_assert(loom::provider_tag<loom::provider::shm_tag>);
        static_assert(loom::provider_tag<loom::provider::tcp_tag>);

        static_assert(!loom::provider_tag<int>);
        static_assert(!loom::provider_tag<void>);
    };
};

auto main() -> int {}
