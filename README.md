# gst-plugin-vfmcap

GStreamer source `vfmcapsrc` for the `vfm_cap` V4L2 device ([vim4-kvm/common_drivers](https://github.com/vim4-kvm/common_drivers) `drivers/media/vfm_cap`).

Eight-bit frames are exported as DMA-buf straight from the vdin capture buffers
and pushed downstream without copies. Packed ten-bit input uses CPU conversion
to P010 for the separate Main10 encoder.

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

On receivers exposing `Capture Frame Rate`, VDIN1 caps use that stable base
budget during VRR; the measured `Frame Rate` remains the fallback for older
kernels. An explicitly unknown base rate is rejected. This does not pace or
duplicate frames, advertise VRR through EDID, or qualify hardware VRR support.

## Build

```sh
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev meson pkg-config
meson setup build --buildtype=release --prefix=/usr && ninja -C build && meson test -C build && sudo ninja -C build install
```

## Use

vdin0 must write NV21/NV12/UYVY, YUV444 or packed ten-bit `AMLY`, and `vfm_cap`
must be in the vfm `tvpath` after `vdin0`. See the `vim4-kvm` repo `tools/hdmi-setup.sh`.

The native-HDR T7 configuration uses full-packed 4:2:2 and `no_swap_en=1`.
`AMLY` conversion reverses each 128-bit word, unpacks Y-Cr-Y-Cb ten-bit samples,
then vertically averages chroma into reusable P010 system-memory buffers.
ARM64 NEON processes complete 32-pixel blocks; the scalar tail preserves padded
rows. This path copies raw frames and requires cacheable read-only DMA mappings
and DMA-BUF CPU synchronization. It does not apply a tone curve. Caps retain
PQ/BT.2020 from the driver's V4L2 colorimetry; use the runtime GStreamer color
parser because vendor and standard transfer-enum numeric values differ.
HLG, twelve-bit and non-full-packed input have not been qualified.

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

On a source change (resolution/format) the element renegotiates caps. Timestamps are taken from the pipeline clock at dequeue time; caps carry `framerate=0/1` since the rate follows the HDMI source.

For temporary diagnostics, set `VIM4_VIDEO_TIMING=1 GST_DEBUG=vfmcapsrc:4`.
`delivery_to_dequeue_ms` compares the kernel's V4L2 delivery timestamp with
`CLOCK_MONOTONIC` immediately after DQBUF. It measures pending delivery, not HDMI
exposure or scanout latency. `sequence` is the consumer's delivery sequence.
Disabled by default; remove these variables after measuring to avoid logging overhead.

## License

[LGPL-2.1-or-later](LICENSE), matching the GStreamer plugin declaration.
