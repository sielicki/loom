// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <loom/core/tagged_claim.hpp>
#include <loom/detail/impl/endpoint_impl.hpp>

#include <array>
#include <cstdlib>
#include <type_traits>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#if defined(__unix__) || defined(__APPLE__)
#    include <sys/wait.h>
#    include <unistd.h>
#endif

namespace {
using namespace loom;
using namespace loom::msg;

static_assert(!std::is_copy_constructible_v<tagged_claim>);
static_assert(!std::is_move_constructible_v<tagged_claim>);

struct claim_provider {
    int status{}, close_status{}, calls{};
    std::uint64_t flags{};
    const fi_msg_tagged* message{};
    bool preserved_context{true};
    std::array<int, std::extent_v<decltype(fi_context2::internal)>> scratch{};
    fi_ops ops{
        .size = sizeof(fi_ops),
        .close =
            [](fid* handle) { return static_cast<claim_provider*>(handle->context)->close_status; },
    };
    fi_ops_tagged tagged{
        .size = sizeof(fi_ops_tagged),
        .recvmsg = [](fid_ep* ep, const fi_msg_tagged* message, std::uint64_t flags) -> ssize_t {
            auto& self = *static_cast<claim_provider*>(ep->fid.context);
            ++self.calls;
            self.flags = flags;
            self.message = message;
            if (self.status) {
                return self.status;
            }
            auto& context = *static_cast<fi_context2*>(message->context);
            for (std::size_t i = 0; i < std::size(context.internal); ++i) {
                if (flags & FI_PEEK) {
                    context.internal[i] = &self.scratch[i];
                } else {
                    self.preserved_context &= context.internal[i] == &self.scratch[i];
                }
            }
            return 0;
        },
    };
    fid_ep handle{.fid = {.context = this, .ops = &ops}, .tagged = &tagged};

    auto endpoint() -> loom::endpoint {
        return loom::endpoint{
            loom::detail::make_pmr_unique<loom::detail::endpoint_impl>(loom::get_default_resource(),
                                                                       &handle,
                                                                       endpoint_types::rdm,
                                                                       address_format::inet,
                                                                       0UZ,
                                                                       0UZ,
                                                                       caps{},
                                                                       0ULL,
                                                                       FI_MULTI_RECV | FI_MORE)};
    }
};

struct claim_fixture {
    claim_provider native;
    loom::endpoint ep{native.endpoint()};
    tagged_claim claim{ep};

    ~claim_fixture() {
        // Also retire an active context when an assertion unwinds the fixture.
        native.close_status = 0;
        (void)ep.close();
    }

    auto event(std::error_code error = {}) -> completion_event {
        completion_event value{};
        value.context = claim.context();
        value.error = error;
        value.bytes_transferred = 7;
        value.flags = FI_TAGGED | FI_RECV | FI_REMOTE_CQ_DATA;
        value.tag = 42;
        value.data = 13;
        value.source = fabric_addr{19};
        value.error_info.prov_errno = 23;
        return value;
    }

    void reserve() {
        REQUIRE(claim.probe(42, 0x80, fabric_addr{19}));
        REQUIRE(claim.observe(event()) == tagged_claim_result::reserved);
    }
};
}  // namespace

TEST_CASE("tagged claim retains native storage through consumption", "[message][claim]") {
    const auto discard = GENERATE(false, true);
    claim_fixture fixture;
    auto& claim = fixture.claim;
    const auto context = claim.context();
    REQUIRE(claim.state() == tagged_claim_state::idle);
    REQUIRE_FALSE(claim.completion());
    REQUIRE(claim.receive({}).error() == errc::invalid_argument);
    REQUIRE(claim.discard().error() == errc::invalid_argument);
    REQUIRE(fixture.native.calls == 0);
    REQUIRE(claim.probe(42, 0x80, fabric_addr{19}));
    REQUIRE(fixture.native.flags == (FI_PEEK | FI_CLAIM | FI_COMPLETION));
    REQUIRE(fixture.native.message->context == context.get());
    REQUIRE(claim.state() == tagged_claim_state::probing);
    REQUIRE(claim.probe(99).error() == errc::busy);
    REQUIRE(claim.receive({}).error() == errc::invalid_argument);
    REQUIRE(claim.discard().error() == errc::invalid_argument);
    auto event = fixture.event();
    REQUIRE(claim.observe(event) == tagged_claim_result::reserved);
    REQUIRE(claim.state() == tagged_claim_state::reserved);
    REQUIRE(claim.completion()->bytes_transferred == 7);
    REQUIRE(claim.completion()->tag == 42);
    REQUIRE(claim.completion()->data == 13);
    REQUIRE(claim.completion()->source == fabric_addr{19});
    REQUIRE(claim.completion()->flags == event.flags);
    REQUIRE(claim.observe(event).error() == errc::invalid_argument);
    REQUIRE(claim.probe(99).error() == errc::busy);
    REQUIRE(fixture.native.calls == 1);

    std::array<std::byte, 7> buffer{};
    int descriptor{};
    REQUIRE((discard ? claim.discard() : claim.receive(buffer, mr_descriptor{&descriptor})));
    REQUIRE(claim.state() ==
            (discard ? tagged_claim_state::discarding : tagged_claim_state::receiving));
    REQUIRE(fixture.native.flags == (FI_CLAIM | FI_COMPLETION | (discard ? FI_DISCARD : 0)));
    REQUIRE(fixture.native.message->context == context.get());
    REQUIRE(fixture.native.preserved_context);
    REQUIRE(fixture.native.message->tag == 42);
    REQUIRE(fixture.native.message->ignore == 0x80);
    REQUIRE(fixture.native.message->addr == 19);
    // The provider can read the message, IOV and descriptor after submission.
    if (!discard) {
        REQUIRE(fixture.native.message->iov_count == 1);
        REQUIRE(fixture.native.message->msg_iov[0].iov_base == buffer.data());
        REQUIRE(fixture.native.message->msg_iov[0].iov_len == buffer.size());
        REQUIRE(fixture.native.message->desc[0] == &descriptor);
    } else {
        REQUIRE(fixture.native.message->iov_count == 0);
        REQUIRE(fixture.native.message->desc == nullptr);
    }
    REQUIRE(claim.receive(buffer).error() == errc::invalid_argument);
    REQUIRE(claim.discard().error() == errc::invalid_argument);
    REQUIRE(fixture.native.calls == 2);
    REQUIRE(claim.observe(event) ==
            (discard ? tagged_claim_result::discarded : tagged_claim_result::received));
    REQUIRE(claim.state() == tagged_claim_state::idle);
    REQUIRE(claim.observe(event).error() == errc::invalid_argument);
    REQUIRE(claim.discard().error() == errc::invalid_argument);
    REQUIRE(claim.probe(99));
    REQUIRE(claim.context().get() == context.get());
    REQUIRE(claim.observe(fixture.event(errc::no_message)) == tagged_claim_result::no_match);
}

TEST_CASE("tagged claim submission rejection preserves the reservation", "[message][claim]") {
    const auto status = GENERATE(-FI_EAGAIN, -FI_EOPNOTSUPP, -FI_EIO);
    const auto discard = GENERATE(false, true);
    claim_fixture fixture;
    auto& claim = fixture.claim;
    fixture.native.status = status;
    REQUIRE(claim.probe(42).error() == make_error_code_from_fi_errno(status));
    REQUIRE(claim.state() == tagged_claim_state::idle);
    fixture.native.status = 0;
    fixture.reserve();
    fixture.native.status = status;
    const auto rejected = discard ? claim.discard() : claim.receive({});
    REQUIRE(rejected.error() == make_error_code_from_fi_errno(status));
    REQUIRE(claim.state() == tagged_claim_state::reserved);
    REQUIRE(claim.completion()->tag == 42);
    fixture.native.status = 0;
    REQUIRE(claim.discard());
    REQUIRE(fixture.native.preserved_context);
    REQUIRE(claim.observe(fixture.event()) == tagged_claim_result::discarded);
}

TEST_CASE("tagged claim normalizes only probe no-match", "[message][claim]") {
    const auto phase = GENERATE(
        tagged_claim_state::probing, tagged_claim_state::receiving, tagged_claim_state::discarding);
    const auto error = GENERATE(FI_ENOMSG, FI_ETRUNC, FI_ECANCELED, FI_EIO);
    claim_fixture fixture;
    auto& claim = fixture.claim;
    REQUIRE(claim.probe(42));
    if (phase != tagged_claim_state::probing) {
        REQUIRE(claim.observe(fixture.event()) == tagged_claim_result::reserved);
        REQUIRE((phase == tagged_claim_state::receiving ? claim.receive({}) : claim.discard()));
    }
    auto event = fixture.event(make_error_code_from_fi_errno(error));
    event.error_info.err_data = std::make_shared<std::byte[]>(3);
    event.error_info.err_data_size = 3;
    const auto observed = claim.observe(event);
    if (phase == tagged_claim_state::probing && error == FI_ENOMSG) {
        REQUIRE(observed == tagged_claim_result::no_match);
    } else {
        REQUIRE_FALSE(observed);
        REQUIRE(observed.error() == event.error);
    }
    REQUIRE(claim.state() == tagged_claim_state::idle);
    REQUIRE(claim.completion()->error == event.error);
    REQUIRE(claim.completion()->error_info.prov_errno == 23);
    REQUIRE(claim.completion()->error_info.err_data == event.error_info.err_data);
    REQUIRE(claim.completion()->error_info.err_data_size == 3);
    REQUIRE(claim.probe(42));
    REQUIRE(claim.observe(fixture.event(errc::no_message)) == tagged_claim_result::no_match);
}

TEST_CASE("tagged claim rejects unrelated completions", "[message][claim]") {
    claim_fixture fixture;
    REQUIRE(fixture.claim.probe(42));
    auto foreign = fixture.event();
    foreign.context = context_ptr<void>{nullptr};
    REQUIRE(fixture.claim.observe(foreign).error() == errc::invalid_argument);
    REQUIRE(fixture.claim.state() == tagged_claim_state::probing);
    REQUIRE_FALSE(fixture.claim.completion());
    REQUIRE(fixture.claim.observe(fixture.event(errc::no_message)) ==
            tagged_claim_result::no_match);
}

TEST_CASE("tagged claim retires after successful endpoint close", "[message][claim][close]") {
    const auto phase = GENERATE(tagged_claim_state::probing,
                                tagged_claim_state::reserved,
                                tagged_claim_state::receiving,
                                tagged_claim_state::discarding);
    claim_fixture fixture;
    auto& claim = fixture.claim;
    REQUIRE(claim.probe(42));
    if (phase != tagged_claim_state::probing) {
        REQUIRE(claim.observe(fixture.event()) == tagged_claim_result::reserved);
    }
    if (phase == tagged_claim_state::receiving) {
        REQUIRE(claim.receive({}));
    } else if (phase == tagged_claim_state::discarding) {
        REQUIRE(claim.discard());
    }
    fixture.native.close_status = -FI_EBUSY;
    REQUIRE(fixture.ep.close().error() == errc::busy);
    REQUIRE(claim.retire_after_close().error() == errc::busy);
    REQUIRE(claim.state() == phase);
    fixture.native.close_status = 0;
    REQUIRE(fixture.ep.close());
    REQUIRE(claim.retire_after_close());
    REQUIRE(claim.state() == tagged_claim_state::idle);
    REQUIRE(claim.probe(42).error() == errc::invalid_argument);
    REQUIRE(claim.observe(fixture.event()).error() == errc::invalid_argument);
}

TEST_CASE("tagged claim rejects unavailable native operations", "[message][claim]") {
    claim_fixture fixture;
    SECTION("empty endpoint") {
        REQUIRE(fixture.ep.close());
        REQUIRE(fixture.claim.probe(42).error() == errc::invalid_argument);
    }
    SECTION("absent operation") {
        fixture.native.tagged.recvmsg = nullptr;
        REQUIRE(fixture.claim.probe(42).error() == errc::not_supported);
    }
    SECTION("absent table") {
        fixture.native.handle.tagged = nullptr;
        REQUIRE(fixture.claim.probe(42).error() == errc::not_supported);
    }
    REQUIRE(fixture.claim.state() == tagged_claim_state::idle);
    REQUIRE(fixture.native.calls == 0);
}

#if defined(__unix__) || defined(__APPLE__)
TEST_CASE("destroying an active tagged claim requires endpoint close", "[message][claim][close]") {
    const auto close = GENERATE(false, true);
    const auto reserved = GENERATE(false, true);
    const auto child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        std::set_terminate([] { std::_Exit(73); });
        claim_provider native;
        auto ep = native.endpoint();
        {
            tagged_claim claim{ep};
            if (!claim.probe(42)) {
                std::_Exit(1);
            }
            if (reserved) {
                completion_event event{};
                event.context = claim.context();
                if (!claim.observe(event)) {
                    std::_Exit(2);
                }
            }
            if (close && !ep.close()) {
                std::_Exit(3);
            }
        }
        std::_Exit(0);
    }
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    REQUIRE(WEXITSTATUS(status) == (close ? 0 : 73));
}
#endif
