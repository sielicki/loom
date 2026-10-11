/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#ifndef LOOM_OPENMPI_BRIDGE_H
#define LOOM_OPENMPI_BRIDGE_H

#include <rdma/fi_domain.h>
#include <rdma/fi_tagged.h>

#ifdef __cplusplus
extern "C" {
#endif

struct ompi_mtl_ofi_loom;

/* Borrowed handles for the consumer's address exchange and bookkeeping. */
struct ompi_mtl_ofi_loom_handles {
    struct fid_fabric* fabric;
    struct fid_domain* domain;
    struct fid_av* av;
    struct fid_cq* cq;
    struct fid_ep* ep;
};

int ompi_mtl_ofi_loom_open(const struct fi_info*,
                           enum fi_av_type,
                           size_t,
                           size_t,
                           struct ompi_mtl_ofi_loom**,
                           struct ompi_mtl_ofi_loom_handles*);
/* Failed close retains the entire graph. Callers must quiesce requests first. */
int ompi_mtl_ofi_loom_close(struct ompi_mtl_ofi_loom**);
int ompi_mtl_ofi_loom_active(void);
int ompi_mtl_ofi_loom_insert(struct ompi_mtl_ofi_loom*, const void*, size_t, fi_addr_t*);
int ompi_mtl_ofi_loom_remove(struct ompi_mtl_ofi_loom*, fi_addr_t);

/* Exactly one entry is non-null. Its storage is borrowed until callback return. */
typedef void (*ompi_mtl_ofi_loom_completion)(struct fi_cq_tagged_entry*, struct fi_cq_err_entry*);
int ompi_mtl_ofi_loom_progress(struct ompi_mtl_ofi_loom*, size_t, ompi_mtl_ofi_loom_completion);

int ompi_mtl_ofi_loom_send(struct ompi_mtl_ofi_loom*, const void*, size_t, void*, fi_addr_t, void*);
int ompi_mtl_ofi_loom_recv(struct ompi_mtl_ofi_loom*, void*, size_t, void*, fi_addr_t, void*);
int ompi_mtl_ofi_loom_senddata(
    struct ompi_mtl_ofi_loom*, const void*, size_t, void*, uint64_t, fi_addr_t, void*);
int ompi_mtl_ofi_loom_inject(struct ompi_mtl_ofi_loom*, const void*, size_t, fi_addr_t);
int ompi_mtl_ofi_loom_injectdata(
    struct ompi_mtl_ofi_loom*, const void*, size_t, uint64_t, fi_addr_t);
int ompi_mtl_ofi_loom_tsend(
    struct ompi_mtl_ofi_loom*, const void*, size_t, void*, fi_addr_t, uint64_t, void*);
int ompi_mtl_ofi_loom_trecv(
    struct ompi_mtl_ofi_loom*, void*, size_t, void*, fi_addr_t, uint64_t, uint64_t, void*);
int ompi_mtl_ofi_loom_tsenddata(
    struct ompi_mtl_ofi_loom*, const void*, size_t, void*, uint64_t, fi_addr_t, uint64_t, void*);
int ompi_mtl_ofi_loom_tinject(struct ompi_mtl_ofi_loom*, const void*, size_t, fi_addr_t, uint64_t);
int ompi_mtl_ofi_loom_tinjectdata(
    struct ompi_mtl_ofi_loom*, const void*, size_t, uint64_t, fi_addr_t, uint64_t);
int ompi_mtl_ofi_loom_tsendmsg(struct ompi_mtl_ofi_loom*, const struct fi_msg_tagged*, uint64_t);
int ompi_mtl_ofi_loom_trecvmsg(struct ompi_mtl_ofi_loom*, const struct fi_msg_tagged*, uint64_t);
int ompi_mtl_ofi_loom_cancel(struct ompi_mtl_ofi_loom*, void*);

#ifdef __cplusplus
}
#endif
#endif
