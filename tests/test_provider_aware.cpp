// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/concepts/provider_traits.hpp>
#include <loom/core/memory.hpp>

#include <catch2/catch_test_macros.hpp>

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

TEST_CASE("provider_remote_memory default construction", "[provider_aware]") {
    loom::provider_remote_memory_test<loom::provider::verbs_tag> rm;

    REQUIRE(rm.addr == 0UL);
    REQUIRE(rm.key == 0UL);
    REQUIRE(rm.length == 0UL);
}

TEST_CASE("provider_remote_memory value construction", "[provider_aware]") {
    loom::provider_remote_memory_test<loom::provider::verbs_tag> rm{0x1000, 0xABCD, 4096};

    REQUIRE(rm.addr == 0x1000UL);
    REQUIRE(rm.key == 0xABCDUL);
    REQUIRE(rm.length == 4096UL);
}

TEST_CASE("provider_remote_memory from generic", "[provider_aware]") {
    loom::remote_memory generic{0x2000, 0x5678, 8192};
    loom::provider_remote_memory_test<loom::provider::efa_tag> rm{generic};

    REQUIRE(rm.addr == 0x2000UL);
    REQUIRE(rm.key == 0x5678UL);
    REQUIRE(rm.length == 8192UL);
}

TEST_CASE("provider_remote_memory to_generic", "[provider_aware]") {
    loom::provider_remote_memory_test<loom::provider::slingshot_tag> rm{0x3000, 0xDEAD, 16384};
    auto generic = rm.to_generic();

    REQUIRE(generic.addr == 0x3000UL);
    REQUIRE(generic.key == 0xDEADUL);
    REQUIRE(generic.length == 16384UL);
}

TEST_CASE("provider_traits static properties verbs", "[provider_aware]") {
    using traits = loom::provider_traits<loom::provider::verbs_tag>;

    REQUIRE(traits::max_inject_size > 0UL);
    REQUIRE(traits::supports_native_atomics);
    REQUIRE_FALSE(traits::uses_staged_atomics);

    REQUIRE(std::string_view{traits::provider_name()} == "verbs");
}

TEST_CASE("provider_traits static properties efa", "[provider_aware]") {
    using traits = loom::provider_traits<loom::provider::efa_tag>;

    REQUIRE(traits::max_inject_size > 0UL);
    REQUIRE_FALSE(traits::supports_native_atomics);
    REQUIRE(traits::uses_staged_atomics);

    REQUIRE(std::string_view{traits::provider_name()} == "efa");
}

TEST_CASE("provider_traits static properties slingshot", "[provider_aware]") {
    using traits = loom::provider_traits<loom::provider::slingshot_tag>;

    REQUIRE(traits::max_inject_size > 0UL);
    REQUIRE(traits::supports_native_atomics);

    REQUIRE(std::string_view{traits::provider_name()} == "cxi");
}

TEST_CASE("provider_traits static properties shm", "[provider_aware]") {
    using traits = loom::provider_traits<loom::provider::shm_tag>;

    REQUIRE(traits::max_inject_size > 0UL);
    REQUIRE(traits::supports_native_atomics);
    REQUIRE_FALSE(traits::uses_staged_atomics);

    REQUIRE(std::string_view{traits::provider_name()} == "shm");
}

TEST_CASE("provider_traits static properties tcp", "[provider_aware]") {
    using traits = loom::provider_traits<loom::provider::tcp_tag>;

    REQUIRE_FALSE(traits::supports_native_atomics);
    REQUIRE(traits::uses_staged_atomics);

    REQUIRE(std::string_view{traits::provider_name()} == "tcp");
}

TEST_CASE("can_inject helper", "[provider_aware]") {
    REQUIRE(loom::can_inject<loom::provider::verbs_tag>(64));
    REQUIRE(loom::can_inject<loom::provider::efa_tag>(32));
    REQUIRE_FALSE(loom::can_inject<loom::provider::efa_tag>(64));

    REQUIRE_FALSE(loom::can_inject<loom::provider::verbs_tag>(1000000));
}

TEST_CASE("provider_remote_memory roundtrip", "[provider_aware]") {
    loom::remote_memory original{0x12345678, 0xDEADBEEF, 65536};

    loom::provider_remote_memory_test<loom::provider::verbs_tag> typed{original};
    auto back = typed.to_generic();

    REQUIRE(back.addr == original.addr);
    REQUIRE(back.key == original.key);
    REQUIRE(back.length == original.length);
}

TEST_CASE("provider_tag concept", "[provider_aware]") {
    static_assert(loom::provider_tag<loom::provider::verbs_tag>);
    static_assert(loom::provider_tag<loom::provider::efa_tag>);
    static_assert(loom::provider_tag<loom::provider::slingshot_tag>);
    static_assert(loom::provider_tag<loom::provider::shm_tag>);
    static_assert(loom::provider_tag<loom::provider::tcp_tag>);

    static_assert(!loom::provider_tag<int>);
    static_assert(!loom::provider_tag<void>);
}
