// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/address.hpp>
#include <loom/core/concepts/capabilities.hpp>
#include <loom/core/concepts/protocol_concepts.hpp>
#include <loom/core/concepts/provider_traits.hpp>
#include <loom/core/concepts/strong_types.hpp>
#include <loom/core/mr_cache.hpp>
#include <loom/core/progress_policy.hpp>
#include <loom/core/result.hpp>
#include <loom/core/types.hpp>

#include <string_view>

#include <catch2/catch_test_macros.hpp>

using namespace std::literals;

namespace {

struct with_static_caps {
    static constexpr bool supports_rma = true;
    static constexpr bool supports_tagged = false;
    static constexpr bool supports_atomic = true;
    static constexpr bool supports_inject = true;
};

struct without_static_caps {};

struct mock_protocol {
    static constexpr bool supports_rma = true;
    static constexpr bool supports_tagged = false;
    static constexpr bool supports_atomic = true;
    static constexpr bool supports_inject = true;
};

struct rma_capable_proto {
    [[nodiscard]] auto name() const noexcept -> const char* { return "test"; }
    [[nodiscard]] auto send(std::span<const std::byte>, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
    [[nodiscard]] auto recv(std::span<std::byte>, loom::context_ptr<void>) -> loom::void_result {
        return loom::make_success();
    }
    [[nodiscard]] auto
    read(std::span<std::byte>, loom::rma_addr, loom::mr_key, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
    [[nodiscard]] auto
    write(std::span<const std::byte>, loom::rma_addr, loom::mr_key, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
};

}  // namespace

TEST_CASE("strong_type basics", "[concepts][strong_types]") {
    using loom::fabric_version;
    using loom::strong_type_concept;

    static_assert(strong_type_concept<fabric_version>);
    static_assert(strong_type_concept<loom::caps>);
    static_assert(strong_type_concept<loom::mr_access>);
    static_assert(strong_type_concept<loom::fabric_addr>);

    constexpr fabric_version v1{0x12345678U};
    constexpr fabric_version v2{0x12345678U};
    constexpr fabric_version v3{0x87654321U};

    REQUIRE(v1 == v2);
    REQUIRE(v1 != v3);
    REQUIRE(v1.get() == 0x12345678U);
}

TEST_CASE("bitwise_flags concept", "[concepts][strong_types]") {
    using loom::bitwise_flags;
    using loom::caps;

    static_assert(bitwise_flags<caps>);
    static_assert(bitwise_flags<loom::mr_access>);
    static_assert(bitwise_flags<loom::mode>);

    constexpr auto c1 = loom::capability::msg;
    constexpr auto c2 = loom::capability::rma;
    constexpr auto combined = c1 | c2;

    REQUIRE(combined.has(c1));
    REQUIRE(combined.has(c2));
    REQUIRE(combined.has_any(c1));
    REQUIRE_FALSE(c1.has(c2));
}

TEST_CASE("unsigned_strong_type concept", "[concepts][strong_types]") {
    using loom::unsigned_strong_type;

    static_assert(unsigned_strong_type<loom::caps>);
    static_assert(unsigned_strong_type<loom::mr_access>);
    static_assert(unsigned_strong_type<loom::fabric_version>);
}

TEST_CASE("underlying_type_t extraction", "[concepts][strong_types]") {
    using loom::underlying_type_t;

    static_assert(std::same_as<underlying_type_t<loom::caps>, std::uint64_t>);
    static_assert(std::same_as<underlying_type_t<loom::fabric_version>, std::uint32_t>);
}

TEST_CASE("provider_tag concept", "[concepts][provider_traits]") {
    using loom::provider_tag;

    static_assert(provider_tag<loom::provider::verbs_tag>);
    static_assert(provider_tag<loom::provider::efa_tag>);
    static_assert(provider_tag<loom::provider::slingshot_tag>);
    static_assert(provider_tag<loom::provider::shm_tag>);
    static_assert(provider_tag<loom::provider::tcp_tag>);
    static_assert(provider_tag<loom::provider::ucx_tag>);
}

TEST_CASE("verbs provider traits", "[concepts][provider_traits]") {
    using traits = loom::provider_traits<loom::provider::verbs_tag>;

    static_assert(traits::supports_native_atomics);
    static_assert(!traits::uses_staged_atomics);
    static_assert(traits::supports_inject);
    static_assert(traits::max_inject_size == 64);
    static_assert(traits::requires_local_key());

    REQUIRE(std::string_view{traits::provider_name()} == "verbs"sv);
}

TEST_CASE("efa provider traits", "[concepts][provider_traits]") {
    using traits = loom::provider_traits<loom::provider::efa_tag>;

    static_assert(!traits::supports_native_atomics);
    static_assert(traits::uses_staged_atomics);
    static_assert(traits::supports_inject);
    static_assert(traits::max_inject_size == 32);
    static_assert(!traits::requires_local_key());

    REQUIRE(std::string_view{traits::provider_name()} == "efa"sv);
}

TEST_CASE("slingshot provider traits", "[concepts][provider_traits]") {
    using traits = loom::provider_traits<loom::provider::slingshot_tag>;

    static_assert(traits::supports_native_atomics);
    static_assert(!traits::uses_staged_atomics);
    static_assert(traits::supports_auto_progress);
    static_assert(traits::default_data_progress == loom::progress_mode::auto_progress);

    REQUIRE(std::string_view{traits::provider_name()} == "cxi"sv);
}

TEST_CASE("provider concept refinements", "[concepts][provider_traits]") {
    using namespace loom;

    static_assert(native_atomic_provider<provider::verbs_tag>);
    static_assert(native_atomic_provider<provider::slingshot_tag>);
    static_assert(!native_atomic_provider<provider::efa_tag>);

    static_assert(staged_atomic_provider<provider::efa_tag>);
    static_assert(staged_atomic_provider<provider::tcp_tag>);
    static_assert(!staged_atomic_provider<provider::verbs_tag>);

    static_assert(inject_capable_provider<provider::verbs_tag>);
    static_assert(inject_capable_provider<provider::efa_tag>);

    static_assert(auto_progress_provider<provider::slingshot_tag>);
    static_assert(auto_progress_provider<provider::shm_tag>);
    static_assert(manual_progress_provider<provider::verbs_tag>);
    static_assert(manual_progress_provider<provider::efa_tag>);

    static_assert(local_key_required_provider<provider::verbs_tag>);
    static_assert(!local_key_required_provider<provider::efa_tag>);
}

TEST_CASE("can_inject helper", "[concepts][provider_traits]") {
    using namespace loom;

    static_assert(can_inject<provider::verbs_tag>(32));
    static_assert(can_inject<provider::verbs_tag>(64));
    static_assert(!can_inject<provider::verbs_tag>(65));

    static_assert(can_inject<provider::efa_tag>(32));
    static_assert(!can_inject<provider::efa_tag>(33));

    static_assert(can_inject<provider::shm_tag>(4096));
    static_assert(!can_inject<provider::shm_tag>(4097));
}

TEST_CASE("compute_rma_addr helper", "[concepts][provider_traits]") {
    using namespace loom;

    constexpr auto addr1 = compute_rma_addr<provider::verbs_tag>(0x1000, 0x100);
    static_assert(addr1 == 0x1100);

    constexpr auto addr2 = compute_rma_addr<provider::efa_tag>(0x2000, 0x50);
    static_assert(addr2 == 0x2050);
}

TEST_CASE("progress_policy concept", "[concepts][progress_policy]") {
    using loom::progress_policy;

    static_assert(progress_policy<loom::runtime_progress_policy>);
    static_assert(progress_policy<loom::auto_progress_policy>);
    static_assert(progress_policy<loom::manual_progress_policy>);
}

TEST_CASE("runtime_progress_policy", "[concepts][progress_policy]") {
    using loom::progress_mode;
    using loom::runtime_progress_policy;

    constexpr runtime_progress_policy manual_policy{progress_mode::manual, progress_mode::manual};
    static_assert(manual_policy.control_progress() == progress_mode::manual);
    static_assert(manual_policy.data_progress() == progress_mode::manual);
    static_assert(manual_policy.requires_manual_data_progress());
    static_assert(manual_policy.requires_manual_control_progress());
    static_assert(!manual_policy.supports_blocking_wait());

    constexpr runtime_progress_policy auto_policy{progress_mode::auto_progress,
                                                  progress_mode::auto_progress};
    static_assert(auto_policy.supports_blocking_wait());
    static_assert(!auto_policy.requires_manual_data_progress());
}

TEST_CASE("static_progress_policy", "[concepts][progress_policy]") {
    using loom::auto_progress_policy;
    using loom::manual_progress_policy;

    static_assert(auto_progress_policy::supports_blocking_wait());
    static_assert(!auto_progress_policy::requires_manual_data_progress());
    static_assert(!auto_progress_policy::requires_manual_control_progress());

    static_assert(!manual_progress_policy::supports_blocking_wait());
    static_assert(manual_progress_policy::requires_manual_data_progress());
    static_assert(manual_progress_policy::requires_manual_control_progress());
}

namespace {

struct mock_msg_endpoint {
    [[nodiscard]] auto send(std::span<const std::byte>, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
    [[nodiscard]] auto recv(std::span<std::byte>, loom::context_ptr<void>) -> loom::void_result {
        return loom::make_success();
    }
};

struct mock_tagged_endpoint {
    [[nodiscard]] auto tagged_send(std::span<const std::byte>,
                                   std::uint64_t,
                                   loom::context_ptr<void>) -> loom::void_result {
        return loom::make_success();
    }
    [[nodiscard]] auto
    tagged_recv(std::span<std::byte>, std::uint64_t, std::uint64_t, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
};

struct mock_inject_endpoint {
    [[nodiscard]] auto inject(std::span<const std::byte>, loom::fabric_addr) -> loom::void_result {
        return loom::make_success();
    }
};

struct mock_rma_endpoint {
    [[nodiscard]] auto
    read(std::span<std::byte>, loom::rma_addr, loom::mr_key, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
    [[nodiscard]] auto
    write(std::span<const std::byte>, loom::rma_addr, loom::mr_key, loom::context_ptr<void>)
        -> loom::void_result {
        return loom::make_success();
    }
};

struct mock_addressable_endpoint {
    [[nodiscard]] auto get_local_address() const -> loom::result<loom::address> {
        return loom::make_error_result<loom::address>(loom::errc::not_supported);
    }
};

struct mock_basic_endpoint : mock_msg_endpoint, mock_addressable_endpoint {};

struct not_an_endpoint {};

}  // namespace

TEST_CASE("msg_capable concept", "[concepts][capabilities]") {
    using namespace loom;

    static_assert(msg_capable<mock_msg_endpoint>);
    static_assert(!msg_capable<not_an_endpoint>);
}

TEST_CASE("tagged_capable concept", "[concepts][capabilities]") {
    using namespace loom;

    static_assert(tagged_capable<mock_tagged_endpoint>);
    static_assert(!tagged_capable<not_an_endpoint>);
}

TEST_CASE("inject_capable concept", "[concepts][capabilities]") {
    using namespace loom;

    static_assert(inject_capable<mock_inject_endpoint>);
    static_assert(!inject_capable<not_an_endpoint>);
}

TEST_CASE("rma_capable concept", "[concepts][capabilities]") {
    using namespace loom;

    static_assert(rma_capable<mock_rma_endpoint>);
    static_assert(rma_read_capable<mock_rma_endpoint>);
    static_assert(rma_write_capable<mock_rma_endpoint>);
    static_assert(!rma_capable<not_an_endpoint>);
}

TEST_CASE("addressable concept", "[concepts][capabilities]") {
    using namespace loom;

    static_assert(addressable<mock_addressable_endpoint>);
    static_assert(!addressable<not_an_endpoint>);
}

TEST_CASE("basic_endpoint composite concept", "[concepts][capabilities]") {
    using namespace loom;

    static_assert(basic_endpoint<mock_basic_endpoint>);
    static_assert(!basic_endpoint<mock_msg_endpoint>);
    static_assert(!basic_endpoint<mock_addressable_endpoint>);
    static_assert(!basic_endpoint<not_an_endpoint>);
}

TEST_CASE("has_static_capabilities concept", "[concepts][protocol]") {
    static_assert(loom::has_static_capabilities<with_static_caps>);
    static_assert(!loom::has_static_capabilities<without_static_caps>);
}

TEST_CASE("protocol_capabilities traits", "[concepts][protocol]") {
    using loom::protocol_capabilities;

    using caps = protocol_capabilities<mock_protocol>;
    static_assert(caps::static_rma == true);
    static_assert(caps::static_tagged == false);
    static_assert(caps::static_atomic == true);
    static_assert(caps::static_inject == true);
}

TEST_CASE("requires_* tag types", "[concepts][protocol]") {
    using namespace loom;

    static_assert(basic_protocol<rma_capable_proto>);
    static_assert(rma_protocol<rma_capable_proto>);
    static_assert(requires_rma::check<rma_capable_proto>);
}

TEST_CASE("mr_cache_traits alignment functions", "[concepts][mr_cache]") {
    using traits = loom::mr_cache_traits<loom::provider::verbs_tag>;

    static_assert(traits::page_size == 4096);

    static_assert(traits::align_down(0x1000) == 0x1000);
    static_assert(traits::align_down(0x1234) == 0x1000);
    static_assert(traits::align_down(0x1FFF) == 0x1000);
    static_assert(traits::align_down(0x2000) == 0x2000);

    static_assert(traits::align_up(0x1000) == 0x1000);
    static_assert(traits::align_up(0x1001) == 0x2000);
    static_assert(traits::align_up(0x1FFF) == 0x2000);
    static_assert(traits::align_up(0x2000) == 0x2000);

    static_assert(traits::aligned_length(0x1000, 100) == 4096);
    static_assert(traits::aligned_length(0x1234, 100) == 4096);
    static_assert(traits::aligned_length(0x1F00, 512) == 8192);
}

TEST_CASE("edge case alignment", "[concepts][mr_cache]") {
    using traits = loom::mr_cache_traits<loom::provider::verbs_tag>;

    static_assert(traits::align_down(0) == 0);
    static_assert(traits::align_up(0) == 0);
    static_assert(traits::align_down(1) == 0);
    static_assert(traits::align_up(1) == 4096);
    static_assert(traits::align_down(4095) == 0);
    static_assert(traits::align_up(4095) == 4096);
    static_assert(traits::align_down(4096) == 4096);
    static_assert(traits::align_up(4096) == 4096);
    static_assert(traits::align_down(4097) == 4096);
    static_assert(traits::align_up(4097) == 8192);
}

TEST_CASE("mr_cacheable concept", "[concepts][mr_cache]") {
    using namespace loom;

    static_assert(mr_cacheable<mr_cache<provider::verbs_tag>>);
    static_assert(mr_cacheable<mr_cache<provider::efa_tag>>);
    static_assert(mr_cacheable<mr_cache<provider::slingshot_tag>>);
    static_assert(mr_cacheable<mr_cache<provider::tcp_tag>>);
    static_assert(mr_cacheable<mr_cache<provider::shm_tag>>);
}

TEST_CASE("result success", "[concepts][result]") {
    auto success = loom::make_success(42);
    REQUIRE(success.has_value());
    REQUIRE(*success == 42);
}

TEST_CASE("result error", "[concepts][result]") {
    auto error = loom::make_error_result<int>(loom::errc::invalid_argument);
    REQUIRE_FALSE(error.has_value());
    REQUIRE(error.error().value() == static_cast<int>(loom::errc::invalid_argument));
}

TEST_CASE("void_result success", "[concepts][result]") {
    auto success = loom::make_success();
    REQUIRE(success.has_value());
}

TEST_CASE("void_result error", "[concepts][result]") {
    auto error = loom::make_error_result<void>(loom::errc::no_memory);
    REQUIRE_FALSE(error.has_value());
}

TEST_CASE("context_ptr void", "[concepts][context_ptr]") {
    loom::context_ptr<void> ptr;
    REQUIRE_FALSE(ptr);
    REQUIRE(ptr.get() == nullptr);

    int value = 42;
    loom::context_ptr<void> ptr2{&value};
    REQUIRE(!!ptr2);
    REQUIRE(ptr2.as<int>() == &value);
}

TEST_CASE("context_ptr typed", "[concepts][context_ptr]") {
    int value = 42;
    loom::context_ptr<int> ptr{&value};
    REQUIRE(!!ptr);
    REQUIRE(ptr.get() == &value);
    REQUIRE(*ptr == 42);
    REQUIRE(ptr.as() == &value);
}

namespace {

struct my_async_context : loom::async_context<my_async_context> {
    int request_id{0};
    void* user_data{nullptr};
};

struct invalid_context {
    int data;
    ::fi_context2 fi_ctx{};
};

}  // namespace

TEST_CASE("async_context layout", "[concepts][async_context]") {
    static_assert(sizeof(my_async_context) >= sizeof(::fi_context2));
}

TEST_CASE("async_context raw pointer", "[concepts][async_context]") {
    my_async_context ctx;
    ctx.request_id = 42;

    void* raw = ctx.raw();
    REQUIRE(raw == static_cast<void*>(&ctx));

    auto* recovered = my_async_context::from_raw(raw);
    REQUIRE(recovered == &ctx);
    REQUIRE(recovered->request_id == 42);
}

TEST_CASE("structured_context concept", "[concepts][async_context]") {
    static_assert(loom::structured_context<my_async_context>);
    static_assert(!loom::structured_context<invalid_context>);
    static_assert(!loom::structured_context<int>);
}

TEST_CASE("context_like with structured_context pointer", "[concepts][async_context]") {
    my_async_context ctx;
    ctx.request_id = 123;

    my_async_context* ptr = &ctx;
    static_assert(loom::context_like<my_async_context*>);

    void* raw = loom::to_raw_context(ptr);
    REQUIRE(raw == static_cast<void*>(&ctx));

    my_async_context* null_ptr = nullptr;
    REQUIRE(loom::to_raw_context(null_ptr) == nullptr);
}

TEST_CASE("context_like still accepts void*", "[concepts][async_context]") {
    int value = 42;
    void* ptr = &value;
    static_assert(loom::context_like<void*>);

    void* raw = loom::to_raw_context(ptr);
    REQUIRE(raw == &value);
}

TEST_CASE("context_like accepts nullptr", "[concepts][async_context]") {
    static_assert(loom::context_like<std::nullptr_t>);
    REQUIRE(loom::to_raw_context(nullptr) == nullptr);
}

namespace {

struct my_triggered_context : loom::triggered_context<my_triggered_context> {
    int operation_id{0};
    void* callback_data{nullptr};
};

struct invalid_triggered_context {
    int data;
    ::fi_triggered_context2 trig_ctx{};
};

}  // namespace

TEST_CASE("triggered_context layout", "[concepts][triggered_context]") {
    static_assert(sizeof(my_triggered_context) >= sizeof(::fi_triggered_context2));
}

TEST_CASE("triggered_context raw pointer", "[concepts][triggered_context]") {
    my_triggered_context ctx;
    ctx.operation_id = 42;

    void* raw = ctx.raw();
    REQUIRE(raw == static_cast<void*>(&ctx));

    auto* recovered = my_triggered_context::from_raw(raw);
    REQUIRE(recovered == &ctx);
    REQUIRE(recovered->operation_id == 42);
}

TEST_CASE("triggered_context fi_context_ptr", "[concepts][triggered_context]") {
    my_triggered_context ctx;

    auto* fi_ptr = ctx.fi_context_ptr();
    REQUIRE(fi_ptr == &ctx.trig_ctx);
}

TEST_CASE("structured_triggered_context concept", "[concepts][triggered_context]") {
    static_assert(loom::structured_triggered_context<my_triggered_context>);
    static_assert(!loom::structured_triggered_context<invalid_triggered_context>);
    static_assert(!loom::structured_triggered_context<int>);
    static_assert(!loom::structured_triggered_context<my_async_context>);
}

TEST_CASE("mr_access flag operations", "[concepts][mr_access]") {
    using namespace loom;

    constexpr auto read_write = mr_access_flags::read | mr_access_flags::write;
    static_assert(read_write.has(mr_access_flags::read));
    static_assert(read_write.has(mr_access_flags::write));
    static_assert(!read_write.has(mr_access_flags::remote_read));

    constexpr auto remote = mr_access_flags::remote_read | mr_access_flags::remote_write;
    static_assert(remote.has(mr_access_flags::remote_read));
    static_assert(remote.has(mr_access_flags::remote_write));

    constexpr auto all = read_write | remote;
    static_assert(all.has(mr_access_flags::read));
    static_assert(all.has(mr_access_flags::write));
    static_assert(all.has(mr_access_flags::remote_read));
    static_assert(all.has(mr_access_flags::remote_write));
}

TEST_CASE("capability flag operations", "[concepts][capability]") {
    using namespace loom;

    constexpr auto msg_rma = capability::msg | capability::rma;
    static_assert(msg_rma.has(capability::msg));
    static_assert(msg_rma.has(capability::rma));
    static_assert(!msg_rma.has(capability::tagged));

    constexpr auto full =
        capability::msg | capability::rma | capability::tagged | capability::atomic;
    static_assert(full.has(capability::msg));
    static_assert(full.has(capability::rma));
    static_assert(full.has(capability::tagged));
    static_assert(full.has(capability::atomic));
    static_assert(!full.has(capability::collective));

    static_assert(full.has_any(capability::msg));
    static_assert(full.has_any(capability::atomic));
}
