# gst-plugin-vfmcap

GStreamer source `vfmcapsrc` for the `vfm_cap` V4L2 device ([vim4-kvm/common_drivers](https://github.com/vim4-kvm/common_drivers) `drivers/media/vfm_cap`).

Frames are exported as DMA-buf straight from the vdin capture buffers and pushed downstream without copies, so a hardware encoder (`amlvenc`) can consume them directly.

## Build

```sh
sudo apt install libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev meson pkg-config
meson setup build --buildtype=release --prefix=/usr && ninja -C build && meson test -C build && sudo ninja -C build install
```

## Use

vdin0 must write 8-bit NV21/NV12/UYVY or YUV444 (the 10-bit packed default is rejected), and `vfm_cap` must be in the vfm `tvpath` after `vdin0`. See the `vim4-kvm` repo `tools/hdmi-setup.sh`.

YUV444 requires the driver's `VUY3` extension and `no_swap_en=1`. VDIN writes
Cr-Cb-Y bytes; the source reorders them into standard GStreamer `v308` (Y-Cb-Cr)
system-memory buffers, preserving every chroma sample. This path copies each
frame and uses ARM64 NEON when available; NV21/NV12/UYVY remain zero-copy.
Capture support does not imply that `amlvenc` can encode 4:4:4. Keep NV21 for
existing KVM applications until their complete encoding path supports 4:4:4.
On the VIM4 development board, a release build delivered 300 4K `v308` frames
to fakesink in 22.70 seconds (about 13.2 fps). The uncached DMA read and CPU
repacking path is functional, but is not a 4K60 application pipeline.

For an exclusive diagnostic pipeline, configure the board HDMI service with
`HDMI_CAPTURE_FORMAT=YUV444`, then use:

```sh
gst-launch-1.0 vfmcapsrc ! video/x-raw,format=v308 ! fakesink sync=false
```

The capture setting affects all consumers and requires restarting the HDMI
service. Restore `HDMI_CAPTURE_FORMAT=NV21` before restarting existing KVM apps.

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
