# Third-party: ffnvcodec

`nvEncodeAPI.h` is vendored from [FFmpeg/nv-codec-headers][repo], which
redistributes NVIDIA's NVENC API header under an MIT-style permissive licence.
The full notice is at the top of the header itself.

Only the header is vendored. The implementation lives in `nvEncodeAPI64.dll`,
which ships with the NVIDIA display driver — nothing extra to install, and no
NVIDIA Video Codec SDK download is required to build this project.

Header version: NVENCAPI 13.1

[repo]: https://github.com/FFmpeg/nv-codec-headers
