// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <array>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <vector>

#include <mpi.h>

#if defined(LOOM_OPENMPI_BACKEND)
#    include <loom/openmpi/bridge.h>
#endif

namespace {
void require(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "Open MPI workload failed: %s\n", description);
        MPI_Abort(MPI_COMM_WORLD, 1);
        std::abort();
    }
}

void check(int status) {
    if (status != MPI_SUCCESS) {
        std::array<char, MPI_MAX_ERROR_STRING> message{};
        int length{};
        MPI_Error_string(status, message.data(), &length);
        std::fprintf(stderr, "MPI error: %.*s\n", length, message.data());
        MPI_Abort(MPI_COMM_WORLD, 1);
        std::abort();
    }
}

void large_transfer(int rank) {
    std::vector<int> sent(65536), received(sent.size(), -1);
    std::iota(sent.begin(), sent.end(), rank * 65536);
    MPI_Status status{};
    check(MPI_Sendrecv(sent.data(),
                       sent.size(),
                       MPI_INT,
                       1 - rank,
                       81,
                       received.data(),
                       received.size(),
                       MPI_INT,
                       1 - rank,
                       81,
                       MPI_COMM_WORLD,
                       &status));
    for (std::size_t i = 0; i < received.size(); ++i) {
        require(received[i] == (1 - rank) * 65536 + static_cast<int>(i), "large transfer payload");
    }
    int count{};
    check(MPI_Get_count(&status, MPI_INT, &count));
    require(count == static_cast<int>(received.size()), "large transfer length");
}

void matched_probe(int rank, int sender) {
    constexpr int message_tag = 41, ready_tag = 42, unrelated_tag = 43;
    const int receiver = 1 - sender;
    std::array<int, 13> expected{};
    std::iota(expected.begin(), expected.end(), 100);
    if (rank == sender) {
        int ready{};
        check(MPI_Recv(&ready, 1, MPI_INT, receiver, ready_tag, MPI_COMM_WORLD, MPI_STATUS_IGNORE));
        require(ready == 1, "probe readiness handshake");
        std::array<MPI_Request, 2> requests{};
        const int unrelated = 73;
        check(MPI_Isend(expected.data(),
                        expected.size(),
                        MPI_INT,
                        receiver,
                        message_tag,
                        MPI_COMM_WORLD,
                        &requests[0]));
        check(MPI_Isend(
            &unrelated, 1, MPI_INT, receiver, unrelated_tag, MPI_COMM_WORLD, &requests[1]));
        check(MPI_Waitall(requests.size(), requests.data(), MPI_STATUSES_IGNORE));
    } else {
        int matched{};
        MPI_Status status{};
        check(MPI_Iprobe(MPI_ANY_SOURCE, message_tag, MPI_COMM_WORLD, &matched, &status));
        require(matched == 0, "empty probe");
        const int ready = 1;
        check(MPI_Send(&ready, 1, MPI_INT, sender, ready_tag, MPI_COMM_WORLD));
        MPI_Message message = MPI_MESSAGE_NULL;
        while (!matched) {
            check(MPI_Improbe(
                MPI_ANY_SOURCE, message_tag, MPI_COMM_WORLD, &matched, &message, &status));
        }
        require(status.MPI_SOURCE == sender && status.MPI_TAG == message_tag, "probe attribution");
        int count{};
        check(MPI_Get_count(&status, MPI_INT, &count));
        require(count == static_cast<int>(expected.size()), "probed length");
        matched = 0;
        check(MPI_Iprobe(MPI_ANY_SOURCE, message_tag, MPI_COMM_WORLD, &matched, &status));
        require(matched == 0, "claimed message remains reserved");
        int unrelated{};
        check(MPI_Recv(&unrelated, 1, MPI_INT, sender, unrelated_tag, MPI_COMM_WORLD, &status));
        require(unrelated == 73, "progress while holding a claim");
        std::array<int, 13> received{};
        MPI_Request request = MPI_REQUEST_NULL;
        check(MPI_Imrecv(received.data(), received.size(), MPI_INT, &message, &request));
        require(message == MPI_MESSAGE_NULL, "matched receive consumes its handle");
        check(MPI_Wait(&request, &status));
        require(status.MPI_SOURCE == sender && status.MPI_TAG == message_tag,
                "claimed receive attribution");
        check(MPI_Get_count(&status, MPI_INT, &count));
        require(count == static_cast<int>(expected.size()), "claimed receive length");
        require(received == expected, "claimed receive payload");
    }
    check(MPI_Barrier(MPI_COMM_WORLD));
}

void synchronous_probe(int rank, int sender) {
    constexpr int tag = 51;
    const std::array<int, 4> expected{11, 17, 23, 29};
    if (rank == sender) {
        check(
            MPI_Ssend(expected.data(), expected.size(), MPI_INT, 1 - sender, tag, MPI_COMM_WORLD));
    } else {
        MPI_Message message = MPI_MESSAGE_NULL;
        MPI_Status status{};
        check(MPI_Mprobe(MPI_ANY_SOURCE, tag, MPI_COMM_WORLD, &message, &status));
        std::array<int, 4> received{};
        check(MPI_Mrecv(received.data(), received.size(), MPI_INT, &message, &status));
        require(status.MPI_SOURCE == sender && status.MPI_TAG == tag,
                "synchronous receive attribution");
        require(received == expected, "synchronous claimed receive");
    }
    check(MPI_Barrier(MPI_COMM_WORLD));
}

void datatype_transfer(int rank) {
    MPI_Datatype vector = MPI_DATATYPE_NULL;
    check(MPI_Type_vector(4, 1, 2, MPI_INT, &vector));
    check(MPI_Type_commit(&vector));
    std::array<int, 8> buffer{};
    if (rank == 0) {
        std::iota(buffer.begin(), buffer.end(), 200);
    } else {
        buffer.fill(-1);
    }
    MPI_Request request = MPI_REQUEST_NULL;
    if (rank == 0) {
        check(MPI_Issend(buffer.data(), 1, vector, 1, 61, MPI_COMM_WORLD, &request));
    } else {
        check(MPI_Irecv(buffer.data(), 1, vector, 0, 61, MPI_COMM_WORLD, &request));
    }
    MPI_Status status{};
    check(MPI_Wait(&request, &status));
    if (rank == 1) {
        for (std::size_t i = 0; i < buffer.size(); ++i) {
            require(buffer[i] == (i % 2 == 0 ? 200 + static_cast<int>(i) : -1),
                    "datatype unpack preserves untouched elements");
        }
        int count{};
        check(MPI_Get_count(&status, vector, &count));
        require(count == 1, "datatype receive count");
    }
    check(MPI_Type_free(&vector));
    check(MPI_Barrier(MPI_COMM_WORLD));
}

void errors_and_empty_messages(int rank) {
    std::array<int, 8> buffer{};
    if (rank == 0) {
        check(MPI_Send(buffer.data(), buffer.size(), MPI_INT, 1, 71, MPI_COMM_WORLD));
    } else {
        MPI_Status status{};
        const auto result = MPI_Recv(buffer.data(), 4, MPI_INT, 0, 71, MPI_COMM_WORLD, &status);
        int error{};
        check(MPI_Error_class(result, &error));
        require(error == MPI_ERR_TRUNCATE, "truncation remains an error");
        MPI_Request request = MPI_REQUEST_NULL;
        check(MPI_Irecv(buffer.data(), buffer.size(), MPI_INT, 0, 72, MPI_COMM_WORLD, &request));
        check(MPI_Cancel(&request));
        check(MPI_Wait(&request, &status));
        int canceled{};
        check(MPI_Test_cancelled(&status, &canceled));
        require(canceled != 0, "unmatched receive cancellation");
    }
    check(MPI_Barrier(MPI_COMM_WORLD));
    MPI_Status status{};
    check(MPI_Sendrecv(nullptr,
                       0,
                       MPI_BYTE,
                       1 - rank,
                       73,
                       nullptr,
                       0,
                       MPI_BYTE,
                       1 - rank,
                       73,
                       MPI_COMM_WORLD,
                       &status));
    int count{-1};
    check(MPI_Get_count(&status, MPI_BYTE, &count));
    require(count == 0, "zero-byte message matching");
}
}  // namespace

int main(int argc, char** argv) {
    int provided{};
    if (MPI_Init_thread(&argc, &argv, MPI_THREAD_FUNNELED, &provided) != MPI_SUCCESS) {
        return 1;
    }
#if defined(LOOM_OPENMPI_BACKEND)
    require(ompi_mtl_ofi_loom_active() == 1, "Loom must own the MPI transport");
#endif
    check(MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN));
    require(provided >= MPI_THREAD_FUNNELED, "thread support");
    int rank{}, size{};
    check(MPI_Comm_rank(MPI_COMM_WORLD, &rank));
    check(MPI_Comm_size(MPI_COMM_WORLD, &size));
    require(size == 2, "exactly two ranks are required");
    for (const int sender : std::array{0, 1}) {
        matched_probe(rank, sender);
        synchronous_probe(rank, sender);
    }
    datatype_transfer(rank);
    large_transfer(rank);
    errors_and_empty_messages(rank);
    const int local = rank + 1;
    int sum{};
    check(MPI_Allreduce(&local, &sum, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD));
    require(sum == 3, "collective progress after point-to-point traffic");
    check(MPI_Finalize());
#if defined(LOOM_OPENMPI_BACKEND)
    if (ompi_mtl_ofi_loom_active() != 0) {
        std::fputs("Loom transport remains alive after MPI_Finalize\n", stderr);
        return 1;
    }
#endif
    if (rank == 0) {
        std::puts("Open MPI matching workload passed");
    }
}
