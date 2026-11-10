# Confirmed recurring lessons

Keep only verified, reusable lessons; consolidate stale entries and link supporting evidence.

## Build artifacts can outlive their source targets

A stale executable can pass after current source stops compiling. For branch baselines, configure/build in a fresh directory and compare registered tests with current CMake source before reporting results.

Evidence: [the onboarding audit](../docs/BRANCH_ONBOARDING_AUDIT.md) found a generated GPU-test target absent from source; rebuilding later caught a missing `StampFootprint` that an old executable would hide. [Earlier audit reports](../docs/VR_AUDIT.md) also record stale-binary testing. A later test removal did not establish that the missing behavior was fixed.
