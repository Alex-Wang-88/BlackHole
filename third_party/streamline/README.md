# Streamline runtime

The headers in `include/` are the public NVIDIA Streamline integration headers.
The runtime DLLs are intentionally not committed to this repository. Obtain a
matching x64 Streamline release from NVIDIA and place these files in `bin/`:

- `sl.interposer.dll`
- `sl.common.dll`
- `sl.dlss.dll`
- `nvngx_dlss.dll`

The CMake option `BLACKHOLE_STREAMLINE_RUNTIME_DIR` can point at another
directory instead. `run.ps1` also accepts the environment variable with the
same name.
