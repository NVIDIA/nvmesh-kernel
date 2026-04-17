# PET project

The long version - [Per Entity Traces](https://docs.google.com/document/d/1uhycWNthNgmcrqcWm5jpjMWomBT8L3neSLXbX33OENM/edit?tab=t.0#heading=h.7tpa1l5ykyh6) document

The short version: an I/O operation accumulates, in a private buffer, the execution timeline. On error, the buffer will be written to the disk. The idea sounds, but the **optimal** implementation would take too much time ~ 2 months.

One of the obstacles is the traces preprocessing. It supports multiple platforms:

+ Kernel
+ User space

  + simulator
  + TOMA
  + UM

Since the block code is shared between all of them, we need from the first day to implement all of them. It will take time. So we decided to implement slightly different idea.

## The idea

1. The messages may stay embedded within the executable, probably sitting in some dedicated section.
2. At runtime, it is possible to calculate the string offset from the beginning of that section. This allows us to introduce "unique" message identifier and write it, instead of the whole string.
3. The message arguments may be stored in 2, 3, 5 or 9 bytes. The first byte describes the argument type, while the remaining contain the value.
4. Using relatively simple C & Python code, we may:

   1. Continue to use and compile time validate message format and arguments
   2. Store a copy of all the arguments in some dedicated buffer
   3. Extract all messages from the binary file to some file
   4. Construction the final message, using {Python, ctypes, libc.printf} should be easy enough.

The idea should cut the bootstrap phase from few weeks to probably one week or even less. The next step would be to introduce "mcs" like file, where those message will be copied to.

## Python dependencies

- **pyelftools** -- ELF/DWARF parsing
- **pydantic** -- Type hints enforcing
- **kaitaistruct** -- Kaitai Struct runtime

Exact versions of all dependencies (including transitive) are pinned in
`requirements.txt`. Packages are installed from the internal NVIDIA Artifactory
PyPI mirror (`nv-shared-pypi`).

### Development Environment Usage

Use the wrapper script `pet_messages.sh` to run `nvmeib_pet_messages.py`. It
automatically creates a Python virtual environment on first use and installs
the pinned dependencies:

```bash
common/pet/pet_messages.sh save-dictionary MODULE SECTION OUTPUT
```

By default the venv is created at `common/pet/.venv`. To place it elsewhere
(for example in a build directory), set the `PET_VENV` environment variable:

```bash
PET_VENV=/path/to/build/pet-venv common/pet/pet_messages.sh save-dictionary ...
```
### CI/CD Environment

The build variable `CREATE_PYTHON_TOOLS_EXEC` controls how PET dependencies are packaged. When `CREATE_PYTHON_TOOLS_EXEC=no` (e.g., in the `install_only` suite), PET dependencies are symlinked to `dist/pytools/` at compile time but are not copied to the target node (see `py_to_symlink.sh` for details). In this mode, a wrapper script (`common/pet/pet_messages.sh`) serves as the `nvmesh_pet_messages` entry point and installs PET dependencies into a virtual environment on first invocation.

### Production Environment

When `CREATE_PYTHON_TOOLS_EXEC=yes`, all PET dependencies are compiled into a self-contained `nvmesh_pet_messages` binary (~20 MB). No external runtime dependencies are required.

### Updating dependencies

To regenerate `requirements.txt` after changing the dependency versions in
`pyproject.toml`, see the instructions at the top of `requirements.txt`.

> **Note:** The Kaitai Struct *compiler* (`ksc`) is not needed at runtime. It
> is only required to regenerate `nvmeib_pet_archive.py` from the `.ksy` spec.
> See the [Kaitai download page](https://kaitai.io/#download) for installation
> instructions.


## Build Artifacts

PET dictionary generation is **integrated into the kernel build process**.
During `make all`, `collect_dictionaries.sh` runs whenever `PET_MODULE` is set
(typically to the built client module, e.g., `clnt/nvmeibc.ko`). That step uses
**Poetry** to install the [Python dependencies](#python-dependencies), runs
`nvmeib_pet_messages.py save-dictionary`, and the result is packaged in
`dictionaries.tar.gz` for installation. Once installed, PET dictionaries reside in
 the `/var/log/nvmesh/trace_daemon/pet_dictionaries` directory.

**Successful PET dictionary build** therefore assumes the **same Python
availability as the rest of the NVMesh build**: A suitable
**`python${PY_TO_EXEC_VER}`** (the Python version used for the NVMesh build) or higher must be on the host so Poetry can create the venv and install wheels for the PET tool.

The customer-facing PET entry point is **`nvmesh_pet_messages`**. It is
installed under **`/opt/nvmesh/bin`** and also exposed under
**`/var/log/nvmesh/trace_daemon/nvmesh_pet_messages`** (symlink to the same
binary).

## View PET Traces 
To view PET traces in human-readable text format on a NVMesh node:

```bash
cd /var/log/nvmesh/trace_daemon
./nvmesh_pet_messages view --no-sort pet_dictionaries/ nvmeibc_io_pet_*
```

## Kaitai

[Kaitai](https://kaitai.io/) is a nice project, which allows to describe a binary data using YAML and generate parsers for it.

+ nvmeib_pet_specification.ksy - describes the binary data
+ nvmeib_pet_archive.py - the generated binary data parser
