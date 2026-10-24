// SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
#include <rdma/fabric.h>
#include <rdma/fi_errno.h>

#include <assert.h>
#include <stdio.h>

struct mock_nic {
    struct fid_nic nic;
    struct mock_nic* copy;
    unsigned int closes;
    int copy_error;
};

static int close_nic(struct fid* fid) {
    struct mock_nic* nic = fid->context;
    ++nic->closes;
    return 0;
}

static int copy_nic(struct fid* fid, int command, void* output) {
    struct mock_nic* nic = fid->context;
    assert(command == FI_DUP);
    if (nic->copy_error)
        return nic->copy_error;
    *(struct fid_nic**)output = &nic->copy->nic;
    return 0;
}

static struct fi_ops nic_ops = {
    .size = sizeof(struct fi_ops),
    .close = close_nic,
    .control = copy_nic,
};

static void check_copy(int error) {
    struct mock_nic copy = {0};
    struct mock_nic source = {.copy = &copy, .copy_error = error};
    struct fi_info* info = fi_allocinfo();
    struct fi_info* duplicate;
    assert(info);
    source.nic.fid.context = &source;
    source.nic.fid.ops = &nic_ops;
    copy.nic.fid.context = &copy;
    copy.nic.fid.ops = &nic_ops;
    info->nic = &source.nic;

    duplicate = fi_dupinfo(info);
    assert(source.closes == 0);
    assert(copy.closes == 0);
    if (error) {
        assert(duplicate == NULL);
    } else {
        assert(duplicate);
        assert(duplicate->nic == &copy.nic);
        fi_freeinfo(duplicate);
        assert(copy.closes == 1);
        assert(source.closes == 0);
    }
    fi_freeinfo(info);
    assert(source.closes == 1);
}

int main(void) {
    check_copy(-FI_EIO);
    check_copy(-FI_ENOMEM);
    check_copy(0);
    puts("Discovery copies preserve source NIC ownership");
    return 0;
}
