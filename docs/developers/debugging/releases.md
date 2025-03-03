# Debugging releases playbook

## Tools and Locations

* `.github/workflows/build_package.yml`: Release packaging jobs
* `build_tools/github_actions/build_dist.py`: Main script to build various
  release packages (for all platforms). We usually use this when reproing to
  approximate exactly what the CI does. Assumes a subdirectory of `main_checkout`
  and writes builds to `iree-build` and `iree-install` as a peer of it. To use
  locally, just symlink your source dir as `main_checkout` in an empty
  directory (versus checking out).

## Manylinux releases

The Linux releases are done in a manylinux2014 docker container. At the time of
this writing, it has gcc 9.3.1 and Python versions 3.5 - 3.9 under `/opt/python`.
Note that this docker image approximates a 2014 era RHEL distro, patched with
backported (newer) dev packages. It builds with gcc and BFD linker unless if
you arrange otherwise. `yum` can be used to get some packages.

Get a docker shell (see exact docker image in build_package.yml workflow):

```shell
docker run --rm -it -v $(pwd):/work/main_checkout stellaraccident/manylinux2014_x86_64-bazel-4.2.2:latest /bin/bash
```

Remember that docker runs as root unless if you take steps otherwise. Don't
touch write files in the `/work/main_checkout` directory to avoid scattering
root owned files on your workstation.

The default system Python is 2.x, so you must select one of the more modern
ones:

```shell
export PATH=/opt/python/cp39-cp39/bin:$PATH
```


Build core installation:

```shell
# (from within docker)
cd /work
python ./main_checkout/build_tools/github_actions/build_dist.py main-dist

# Also supports:
#   main-dist
#   py-runtime-pkg
#   py-xla-compiler-tools-pkg
#   py-tflite-compiler-tools-pkg
#   py-tf-compiler-tools-pkg
```

You can `git bisect` on the host and keep running the above in the docker
container. Note that every time you run `build_dist.py`, it deletes the cmake
cache but otherwise leaves the build directory (so it pays the configure cost
but is otherwise incremental). You can just `cd iree-build` and run `ninja`
for faster iteration (after the first build or if changing cmake flags).
Example:

Extended debugging in the manylinux container:

```shell
cd /work/iree-build
# If doing extended debugging in the container, these may make you happier.
yum install ccache devtoolset-9-libasan-devel gdb

# Get an LLVM symbolizer.
yum install llvm9.0
ln -s /usr/bin/llvm-symbolizer-9.0 /usr/bin/llvm-symbolizer

# You can manipulate cmake flags. These may get you a better debug experience.
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo -DIREE_ENABLE_ASAN=ON -DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=gold -DIREE_ENABLE_CCACHE=ON .

ninja

# Or you may need this if buggy LLVM tools (like mlir-tblgen) are leaking :(
ASAN_OPTIONS="detect_leaks=0" ninja
```

Other tips:

* If debugging the runtime, you may have a better time just building the
  Release mode `main-dist` package above once, which will drop binaries in the
  `iree-install` directory. Then build the `py-runtime-pkg` or equiv and
  iterate further in the build directory. Ditto for TF/XLA/etc.

## Testing releases on your fork

To avoid interrupting the regular releases published on the IREE github, you
can test any changes to the release process on your own fork.  Some setup is
required before these github actions will work on your fork and development
branch.

To run
[`schedule_snapshot_release.yml`](https://github.com/google/iree/blob/main/.github/workflows/schedule_snapshot_release.yml),
comment out
[this line](https://github.com/google/iree/blob/392449e986493bf710e3da637ebf807715da9ffe/.github/workflows/schedule_snapshot_release.yml#L14):
```yaml
# Don't run this in everyone's forks.
if: github.repository == 'google/iree'
```

And change the branch from 'main' to the branch you are developing on
[here](https://github.com/google/iree/blob/392449e986493bf710e3da637ebf807715da9ffe/.github/workflows/schedule_snapshot_release.yml#L37):
```yaml
- name: Pushing changes
  uses: ad-m/github-push-action@40bf560936a8022e68a3c00e7d2abefaf01305a6  # v0.6.0
  with:
    github_token: ${{ secrets.WRITE_ACCESS_TOKEN }}
    branch: main
    tags: true
```

To speed up
[`build_package.yml`](https://github.com/google/iree/blob/main/.github/workflows/build_package.yml),
you may want to comment out some of the builds
[here](https://github.com/google/iree/blob/392449e986493bf710e3da637ebf807715da9ffe/.github/workflows/build_package.yml#L34-L87).
The
[`py-pure-pkgs`](https://github.com/google/iree/blob/392449e986493bf710e3da637ebf807715da9ffe/.github/workflows/build_package.yml#L52)
build takes only ~2 minutes and the
[`py-runtime-pkg`](https://github.com/google/iree/blob/392449e986493bf710e3da637ebf807715da9ffe/.github/workflows/build_package.yml#L39)
build takes ~5, while the others can take several hours.

From your development branch, you can manually run the
[Schedule Snapshot Release](https://github.com/google/iree/actions/workflows/schedule_snapshot_release.yml)
action, which invokes the
[Build Native Release Packages](https://github.com/google/iree/actions/workflows/build_package.yml)
action, which finally invokes the
[Validate and Publish Release](https://github.com/google/iree/actions/workflows/validate_and_publish_release.yml)
action.  If you already have a draft release and know the release id, package
version, and run ID from a previous Build Native Release Packages run, you can
also manually run just the Validate and Publish Release action.
