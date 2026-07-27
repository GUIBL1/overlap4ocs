# csg-htsim upstream provenance

- Upstream repository: <https://github.com/Broadcom/csg-htsim.git>
- Upstream commit: `841d9e7be46bb968eece766aa4b6c044c7799f67`
- License: BSD-2-Clause (`LICENSE` in this directory)
- Import date: 2026-07-27
- Import method: verified local clone, followed by `git subtree add --prefix=third_party/csg-htsim <verified-local-clone> 841d9e7be46bb968eece766aa4b6c044c7799f67 --squash`
- Parent repository subtree import commit: `8711aff6133d618da73e44b2c2cc68c78b6dcab1`
- Subtree squash commit: `65953695c4219be84f155e5e6846f941cc7edbd6`

The imported directory is ordinary source tracked by the overlap4ocs parent
repository. It is not a nested repository, submodule, or gitlink.

## Reproducible update procedure

1. Create a dedicated update branch in the parent repository.
2. Clone the official repository into a temporary directory and verify its
   remote, license, clean status, and full target commit SHA.
3. Run `git subtree pull --prefix=third_party/csg-htsim <verified-clone> <full-sha> --squash`.
4. Reconcile each entry in `LOCAL_PATCHES.md`, then update this file and
   `LOCAL_PATCHSET.json`.
5. Run `make -C simulator/htsim_ocs clean all test check-vendor` in the
   supported development environment before merging the update branch.

Do not run `git pull` or create commits from inside this directory.
