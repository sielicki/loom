#!@shell@
# SPDX-License-Identifier: BSD-2-Clause OR GPL-2.0-only
set -euo pipefail

for provider in sockets 'tcp;ofi_rxm'; do
    for tag_mode in ofi_tag_full ofi_tag_1; do
        printf 'Checking Open MPI with %s, %s\n' "$provider" "$tag_mode"
        @timeout@ 90 @bindir@/mpirun --np 2 --oversubscribe --bind-to none \
            --mca pml cm --mca mtl ofi \
            --mca mtl_ofi_enable_sep 0 --mca mtl_ofi_disable_hmem 1 \
            --mca mtl_ofi_provider_include "$provider" \
            --mca mtl_ofi_tag_mode "$tag_mode" \
            @bindir@/openmpi-ofi-workload
    done
done
