# gst-plugin-vfmcap

GStreamer source `vfmcapsrc` for the `vfm_cap` V4L2 device ([vim4-kvm/common_drivers](https://github.com/vim4-kvm/common_drivers) `drivers/media/vfm_cap`).

Eight-bit frames are exported as DMA-buf straight from the vdin capture buffers
and pushed downstream without copies. Packed ten-bit input uses Mali Vulkan
conversion to contiguous P010 DMA buffers for the separate Main10 encoder,
with the verified CPU converter as a startup fallback.

The same plugin provides `vim4vdin1src` for the native-HDR 5.15 runtime:
VDIN0 retains the display's HDR stream while VDIN1 captures independent NV21 SDR
from the same HDMI receiver. The board launcher sets `VIM4_SECONDARY_HDMI_CAPTURE=1`
and selects the private **standard** `video4linux2` plugin; the system vendor
replacement must not be used because it closes the primary HDMI receiver.
`vim4vdin1src` reads HDMI geometry on startup, exports DMA buffers with VideoMeta,
and reserves enough buffers for concurrent encoder/JPEG branches. `height=0`
keeps the input size; a smaller height requests VDIN1 hardware scaling, preserving
aspect ratio. `num-buffers` supports one-frame snapshots. It refuses to start
without the explicit runtime flag: stock `/dev/video71` is display writeback.

The allocation query requests two downstream slots. The matched standard V4L2
plugin adds four, giving six physical buffers (about 71 MiB at 4K NV21).
Use bounded one-frame raw queues for encoder/JPEG branches. Asking for six slots
in the query instead allocates ten physical buffers and can prevent recovery
after HDMI pool reallocation, despite ample total free CMA.

On receivers exposing `Capture Frame Rate`, VDIN1 caps use that stable base
budget during VRR; the measured `Frame Rate` remains the fallback for older
kernels. An explicitly unknown base rate is rejected. This does not pace or
duplicate frames, advertise VRR through EDID, or qualify hardware VRR support.

With the Amlogic `libge2d` development library, this plugin also builds
`vim4scale`: a DMA-only NV12/NV21 → NV12 SDR downscaler. `height=0` preserves
native dimensions; smaller even heights preserve aspect ratio. The optional
`width-alignment=16` satisfies the vendor HEVC encoder; H.264 uses the default 2.
Alternatively, fix the output dimensions with raw-video caps. It never upscales.
Visible width and actual VideoMeta row pitch are separate GE2D parameters,
so a 3440-pixel image in 3456-byte rows is scaled without shearing or CPU copies.
Input must be one contiguous DMA allocation, with equal 64-byte-aligned Y/UV
pitches and UV immediately after the Y rows. The board launcher selects this
V4L2 layout on 5.15 (`GST_V4L2_PREFER_CONTIGUOUS=1`); 7.3 keeps separate plane views.
Native-size negotiation preserves the input format and DMA buffer without a blit
or output pool. Scaled output uses three bounded codecmm slots, a 64-byte pitch
and explicit VideoMeta.
Submission completes synchronously before releasing the input. Flush and stop
wake blocked output acquisitions; retained outputs own their DMA fds until release.

`sdr-scale-compare --exclusive` is an idle-board diagnostic, not an automatic
test: it checks poisonous input padding, visible luma, NV21→NV12 chroma, PTS and
repeated processing for 3440×1440→1720/1712×720. Its throughput is GE2D-only;
measure capture, encoding and browser playback separately.

## Build

```sh
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev meson pkg-config libvulkan-dev glslang-tools
meson setup build --buildtype=release --prefix=/usr && ninja -C build && meson test -C build && sudo ninja -C build install
```

## Use

vdin0 must write NV21/NV12/UYVY, YUV444 or packed ten-bit `AMLY`, and `vfm_cap`
must be in the vfm `tvpath` after `vdin0`. See the `vim4-kvm` repo `tools/hdmi-setup.sh`.

The native-HDR T7 configuration uses full-packed 4:2:2 and `no_swap_en=1`.
`AMLY` conversion unpacks little-endian Cb-Y-Cr-Y ten-bit slots (five bytes per pair),
then vertically averages chroma into P010. The Vulkan backend requires a real
Mali-G52, external-memory DMA imports and the matching userspace driver. Select
its private ICD before starting GStreamer; the board One-KVM launcher does so
on 5.15. No global graphics-library links are changed. Two fixed codecmm slots
are allocated before the encoder's workspace; there is no unbounded CMA growth.
Each GPU job completes and releases its temporary imports before returning the
captured frame. The encoder must retain the output until its SDK reports input
retirement. This two-slot budget requires the current low-delay encoder SDK,
which retires the submitted DMA input on each completed call. Pool flushing
wakes a source blocked waiting for an output slot.

For packed ten-bit input, `vfmcapsrc height=1700` requests a smaller even height
and derives an even width preserving the input aspect ratio (4K becomes
3022×1700). Zero or a height at least as large as the input keeps native size.
Scaling requires Mali and contiguous output buffers; it fails explicitly when
unavailable. Native-size conversion retains its CPU fallback.
The scaler unpacks into two fixed GPU-private R16F/RG16F textures, retaining
4:2:2 chroma until bilinear resizing and 4:2:0 subsampling. Binary16 represents
all ten-bit integer samples exactly. Texture interpolation has finite subtexel
precision; it does not apply a tone curve or change PQ/BT.2020. The final P010
surface uses the same bounded DMA pool and encoder retirement contract.

When Vulkan or contiguous allocation is unavailable at startup, CPU conversion
uses reusable system-memory buffers. `gpu-conversion=false` forces that path for
comparisons. A GPU error after submission fails the stream instead of returning
potentially incomplete pixels. ARM64 NEON processes complete 32-pixel blocks;
the scalar tail preserves padded rows. CPU access requires cacheable read-only
DMA mappings and DMA-BUF synchronization. Neither path applies a tone curve. Caps retain
PQ/BT.2020 from the driver's V4L2 colorimetry; use the runtime GStreamer color
parser because vendor and standard transfer-enum numeric values differ.
HLG, twelve-bit and non-full-packed input have not been qualified.

`amly-gpu-compare` is an exclusive hardware diagnostic, not an automatic test.
Its default suite compares synthetic extreme/random samples, workgroup/NEON
boundaries, unusual pitches, padding, 1440p/ultrawide and 4K byte-for-byte with
the CPU converter. For an actual captured frame, including optional page padding:

```sh
VK_ICD_FILENAMES=/usr/lib/vim4-kvm/gpu/mali.json build/amly-gpu-compare --raw capture.amly 3840 2160 9600 7680 3
VK_ICD_FILENAMES=/usr/lib/vim4-kvm/gpu/mali.json build/amly-gpu-compare --scale 3840 2160 3022 1700 120
```

This replays the same frame and checks visible samples and padding on the first
and final dispatch. Native conversion remains byte-exact. Scaled output is
compared with an independent double-precision bilinear reference (maximum six
ten-bit code values for the qualified Mali texture filter), with zero low P010
bits and unchanged padding. The default suite includes clamped edges, unusual
pitches, non-block-aligned dimensions, 4K downscales and ultrawide input. It does not
measure capture/encode/browser throughput. `VIM4_VIDEO_TIMING=1` also reports
GPU import, dispatch, fence wait, and import destruction/idle every120frames.

YUV444 requires the driver's `VUY3` extension and `no_swap_en=1`. VDIN writes
Cr-Cb-Y bytes; the source reorders them into standard GStreamer `v308` (Y-Cb-Cr)
system-memory buffers, preserving every chroma sample. This path copies each
frame and uses ARM64 NEON when available; NV21/NV12/UYVY remain zero-copy.
Use the driver with cacheable read-only DMA mappings and CPU access callbacks:
cache invalidation runs once per immutable frame export, and the source fuses
copying/reordering into reusable output buffers. The earlier write-combine
mapping is functionally compatible but too slow for full-frame CPU processing.
Capture support does not imply that `amlvenc` can encode 4:4:4. Keep NV21 for
existing KVM applications until their complete encoding path supports 4:4:4.

For an exclusive diagnostic pipeline, configure the board HDMI service with
`HDMI_CAPTURE_FORMAT=YUV444`, then use:

```sh
gst-launch-1.0 vfmcapsrc ! video/x-raw,format=v308 ! fakesink sync=false
```

The capture setting affects all consumers and requires restarting the HDMI
service. Restore `HDMI_CAPTURE_FORMAT=NV21` before restarting existing KVM apps.

## Hardware regression

Build `tests/capture-pattern.swift` with `swiftc` on the Mac supplying HDMI and
run it on a physical 3840×2160 display with 1920×1080 HiDPI scaling. It displays
dynamic frame IDs at three heights and single-pixel chroma stripes, then exits
after 240 seconds. With exclusive board access and YUV444 configured, run the
Meson-built `capture-integrity /dev/video_cap 3600 last-frame.raw`. After 120
warm-up frames it checks every output frame for checksum failures, tearing,
repeated/skipped IDs and horizontal/vertical chroma preservation. A failure
returns nonzero; source/display cadence can also cause repeated/skipped IDs.
This tests the complete capture/repack path to appsink, not encoding/network.

```sh
gst-launch-1.0 vfmcapsrc ! queue ! amlvenc bitrate=8000 ! video/x-h264 ! h264parse ! mp4mux ! filesink location=cap.mp4
```

| Property | Default | |
|---|---|---|
| `device` | `/dev/video_cap` | vfm_cap device node |
| `gpu-conversion` | `true` | Try Mali conversion for packed ten-bit; startup failures fall back to CPU |
| `height` | `0` | Maximum packed ten-bit output height; smaller values must be even and require GPU scaling |

On a source change (resolution/format) the element renegotiates caps. Timestamps are taken from the pipeline clock at dequeue time; caps carry `framerate=0/1` since the rate follows the HDMI source.

For temporary diagnostics, set `VIM4_VIDEO_TIMING=1 GST_DEBUG=vfmcapsrc:4`.
`delivery_to_dequeue_ms` compares the kernel's V4L2 delivery timestamp with
`CLOCK_MONOTONIC` immediately after DQBUF. It measures pending delivery, not HDMI
exposure or scanout latency. `sequence` is the consumer's delivery sequence.
Disabled by default; remove these variables after measuring to avoid logging overhead.

## License

[LGPL-2.1-or-later](LICENSE), matching the GStreamer plugin declaration.
