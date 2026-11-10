# Qualcomm Xen branches

    master (upstream base)
      qcom/common       base + fixes shared by every Qualcomm platform
        qcom/eliza      + Eliza (SM7750) patches
        qcom/nord       + Nord (SA8797P) patches
        qcom/lemans     + Lemans (SA8775P) patches
      qcom/integration  qcom/common + every qcom/<platform> merged; TOPICS.md

## Where a fix goes

- qcom/common: generic Xen or Arm code, and Qualcomm code that is not tied
  to one SoC (GENI UART, SCM, PDC, SMMU-500 handling, FF-A). Gate any
  SoC-dependent behaviour on the device tree, so other platforms are
  unaffected at runtime.
- qcom/<platform>: code or data that names a SoC or board (compatible
  tables, SoC quirks, board docs).
- Never a copy of a common fix on a platform branch: a platform that
  needs a newer qcom/common merges it.
- Not in these branches: demo-only, debug-only (DIAG, EUD) and feature
  work. Feature branches (features/<platform>/<topic>) start from
  qcom/<platform>.

## Adding a fix

1. Commit on the right branch (kernel-style subject, Signed-off-by).
2. Build every new commit for arm64 with the platform config (for
   qcom/common: every platform config and plain defconfig); no warnings.
3. Boot the affected platforms with a fingerprinted build and read the
   fingerprint back before judging the result.
4. Fast-forward push of the branch.
5. Refresh qcom/integration: tag the current head
   qcom/integration-YYYYMMDD, merge qcom/common first if it moved, then
   the changed platform branches; write each conflict and its resolution
   in the merge commit message; update TOPICS.md (branch table, per-patch
   map, an "Added YYYY-MM-DD" line).

## Moving to a new upstream base

Rebase qcom/common onto the new base, rebase each platform branch onto
the new qcom/common, rebuild qcom/integration from scratch, keep the old
state as a dated tag, and record the redone resolutions under
"Conflicts (refresh YYYY-MM-DD)".
