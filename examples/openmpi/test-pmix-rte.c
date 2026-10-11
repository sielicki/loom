/* SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only */
#include "src/runtime/pmix_rte.h"

int main(void)
{
    pmix_status_t status = pmix_rte_init(PMIX_PROC_CLIENT, NULL, 0, NULL);
    if (PMIX_SUCCESS != status) {
        fprintf(stderr, "PMIx runtime initialization failed: %d\n", status);
        return 1;
    }
    pmix_rte_finalize();
    return 0;
}
