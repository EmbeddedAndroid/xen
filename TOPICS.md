# TOPICS: Xen qcom/integration

Base: xen staging c4a09c112c ("x86/time: fix typo s/interruopt/interrupt/",
4.23-unstable), the EmbeddedAndroid/xen master branch.
qcom/common is the base plus every fix shared by the Qualcomm platforms,
each carried once. Every qcom/<platform> branch is qcom/common plus that
platform's own patches. This branch merges every platform branch onto
qcom/common; each merge commit message records its conflict resolutions.
Snapshots before each refresh: tags qcom/integration-YYYYMMDD.
How to add a fix: README.qcom.md.

Created 2026-11-10 from eliza/xen 2154146601, nord/xen c6b1d655a7,
features/lemans/drtm 01d9c6c174 and the xen-cil RRD queue; eliza/xen and
nord/xen are frozen and superseded by qcom/eliza and qcom/nord.

## Branches

| branch | on | carries | upstream status |
|---|---|---|---|
| qcom/common | base | GENI UART driver and fixes, SCM probe, PDC routing (incl. GPIO wakeup SPIs), extended SPIs, direct-mapped hwdom memory (low bank kept for DMA, modules in the first bank that fits, lowest-first allocation, memory holes, CPU capacities), hwdom DT (bus-local reg, #interrupt-cells, Xen's IOMMUs as disabled nodes, SMMUs Xen does not drive), SMMU (unprotected masters of untaken SMMUs, dma-coherent at stage 2, secure firmware slice), FF-A (direct requests to the SPMC, lent memory taken from the lender), SA8797P docs | not submitted |
| qcom/eliza | qcom/common | Eliza SoC match | not submitted |
| qcom/nord | qcom/common | nothing beyond common (SA8797P docs are in common's history) | not submitted |
| qcom/lemans | qcom/common | Lemans (SA8775P) SoC match | not submitted |

Not in integration: eliza/xen-demo (dom0-iommu=nested, demo only),
features/lemans/drtm (DRTM feature work, rebases onto qcom/lemans), any
DIAG/EUD/debug patch.

## Per-patch map

### qcom/common (on the base)

| commit | subject | author | origin | note |
|---|---|---|---|---|
| 4707d408239a | xen/arm: gic: translate extended SPI device tree interrupts | Tyler Baker | shared history (eliza/xen, nord/xen) |  |
| a8e5bf63d73c | xen/arm: skip zero-sized regions when looking for extended regions | Tyler Baker | shared history (eliza/xen, nord/xen) |  |
| 5ee85c4d4f99 | xen/arm: check that a direct-mapped hwdom's bank 0 can hold its modules | Tyler Baker | shared history (eliza/xen, nord/xen) |  |
| 1388d7d2f10c | xen/char: add Qualcomm GENI UART driver | Volodymyr Babchuk | shared history (eliza/xen, nord/xen) |  |
| ce66c3cef655 | xen/arm: platforms: answer the Qualcomm SCM convention probe | Volodymyr Babchuk | shared history (eliza/xen, nord/xen) |  |
| 87ebfb08df5f | xen/arm: platforms: qcom: route PDC interrupts to the hardware domain | Tyler Baker | shared history (eliza/xen, nord/xen) |  |
| 6b42ec9c6aa7 | CHANGELOG.md: mention the Qualcomm GENI UART and SA8797P support | Tyler Baker | shared history (eliza/xen, nord/xen) |  |
| 5189e31046ad | docs/arm: add SA8797P (Nord) build and test notes | Tyler Baker | shared history (eliza/xen, nord/xen) |  |
| a927814a874d | xen/arm: do not map bus-local "reg" addresses for the hardware domain | Tyler Baker | ea/eliza/xen e31db59f9f |  |
| 474c6d744eff | xen/arm: smmu: leave masters of an SMMU the driver did not take unprotected | Tyler Baker | ea/eliza/xen 33f406994b |  |
| e1237c8ece1c | xen/char: geni-uart: empty the RX FIFO on each interrupt | Tyler Baker | ea/eliza/xen ec2ae39a29 |  |
| dedc4210191d | xen/arm: keep the host's #interrupt-cells in the hwdom GIC node | Tyler Baker | ea/eliza/xen 6c2aac3c76 |  |
| 3e3ce4c859c6 | xen/arm: load a 64-bit kernel into the first bank that can hold it | Tyler Baker | ea/eliza/xen b753834301 |  |
| 4aa75579e134 | xen/arm: keep the low bank of a direct-mapped hwdom for 32-bit DMA | Tyler Baker | ea/eliza/xen 0a4f68b9ff | supersedes xen-cil 0003 |
| 8da854a15b22 | xen/arm: smmu: keep dma-coherent masters coherent at stage 2 | Tyler Baker | ea/eliza/xen 7eb9a2704a |  |
| 2a0cdf0f6b29 | xen/arm: platforms: qcom: forward the hwdom's memory assign call | Tyler Baker | ea/eliza/xen 090bcd32d1 | context only: no Eliza match in common |
| 664b06dcbef2 | xen/arm: remove the host RAM from a direct-mapped hwdom's memory holes | Tyler Baker | ea/eliza/xen 19313e24c8 |  |
| 13ba4a16138a | xen/arm: describe Xen's IOMMUs to the hwdom as disabled nodes | Tyler Baker | ea/eliza/xen 85d841e67d |  |
| 7180f6fbc757 | xen/arm: give a direct-mapped hwdom the SMMUs Xen does not drive | Tyler Baker | ea/eliza/xen 2c6fc7a789 |  |
| e482ce57d60f | xen/arm: ffa: let Xen send direct requests to the SPMC | Tyler Baker | ea/eliza/xen 1e9e12765e | nord/xen 7ab791257c (same patch) |
| 613128114376 | xen/arm: smmu: keep out of the secure firmware's slice of an SMMU | Tyler Baker | ea/eliza/xen a6164610b9 | nord/xen a790719ca1 (without the dt_init hunk) |
| c5a25b52231f | xen/arm: hide firmware-owned streams from the hwdom's iommus | Tyler Baker | ea/eliza/xen a30f3105b5 |  |
| 75036de4b837 | xen/arm: ffa: take lent memory away from the lender | Tyler Baker | ea/eliza/xen 35aa76bd50 | nord/xen c6b1d655a7 (same patch) |
| bf91e0acf256 | xen/arm: describe a pinned dom0's CPU capacities | Tyler Baker | ea/eliza/xen 2154146601 |  |
| 8427906bcb10 | xen/arm: platforms: qcom: route the PDC's GPIO wakeup SPIs | Tyler Baker | ea/features/lemans/drtm 019cbac754 | also xen-iommu/kiosk e3b6e746bc (same patch) |
| 6cc2dbab55b6 | xen/char: geni-uart: set up RX and restart it when it stops | Tyler Baker | xen-cil local queue | xen-cil 0001, rebased onto the RX drain fix |
| 1f48ea62dca3 | xen/arm: allocate a direct-mapped hwdom as low as possible | Tyler Baker | xen-cil local queue | xen-cil 0002 |

### qcom/eliza (on qcom/common)

| commit | subject | author | origin | note |
|---|---|---|---|---|
| adb448e6ebef | xen/arm: platforms: qcom: match the Eliza SoC | Tyler Baker | ea/eliza/xen ee07078859 |  |

### qcom/lemans (on qcom/common)

| commit | subject | author | origin | note |
|---|---|---|---|---|
| 41089b61fd06 | xen/arm: platforms: qcom: match the Lemans SoC | Tyler Baker | ea/features/lemans/drtm 01d9c6c174 | context only: no Eliza match on qcom/lemans |
