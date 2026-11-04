# gst-plugin-vfmcap

This page covers VIM4 capture ownership, DMA conversion and hardware diagnostic limits.
Current rule: share one capture owner, preserve VideoMeta, and deploy matched driver/plugin/runtime components.
Read [build](#build), [vendor capture](#existing-515-capture-and-scaling),
[packed HDR](#packed-hdr-conversion), [mainline HDR](#native-hdr-capture-on-the-experimental-v4l2-backend),
[SDR branch](#sdr-branch-candidate), or [validation](#hardware-regression).
Properties, layouts and pool counts are authoritative in the linked source/class_init, not repeated here.

## Build

Dependencies, optional UAPI/Vulkan/GE2D support and offline targets live in [meson.build](meson.build).
Configure a release build, then build, run Meson tests and install.
For the experimental source, export only the companion AMLY UAPI header into a private include tree;
pass that tree to the compiler. Without the header the factory is not registered.
The private standard V4L2 plugin needs the workspace mapping patches; the vendor replacement closes the primary RX.
Production dependencies and installation belong to the workspace
[package recipe](https://github.com/vim4-kvm/vim4-kvm/blob/main/packaging/ubuntu/package.py).
Runtime properties can be inspected with gst-inspect-1.0 after selecting the candidate plugin.

## Existing 5.15 capture and scaling

[vfmcapsrc](src/gstvfmcapsrc.c) exports eight-bit frames as DMA-buf without copying.
The board [HDMI setup](https://github.com/vim4-kvm/vim4-kvm/blob/main/tools/hdmi-setup.sh)
owns capture format and vfm routing. Capture support alone does not imply encoder support.

[VDIN1 source](src/gstvim4vdin1src.c) supplies independent SDR while VDIN0 retains HDR display.
It requires the explicit runtime enable flag: stock VDIN1 is display writeback, not isolated HDMI capture.
Use the matched standard V4L2 plugin and VideoMeta. Allocation-query slots are added to the plugin's own slots;
copying a physical pool count into that query previously over-allocated CMA and prevented input recovery.
Keep raw branch queues bounded. Stable capture base rate is used where available; unknown is rejected,
older kernels retain measured-rate fallback. This does not pace frames, advertise EDID VRR or qualify hardware VRR.

[GE2D scaler](src/gstvim4scale.c) handles SDR DMA downscaling.
Visible width and actual row pitch are independent; conflating them shears padded ultrawide input.
The validated contiguous-plane contract is mandatory, with native-size passthrough avoiding a blit/pool.
The common launcher selects the matching vendor layout; mainline retains separate plane views.
GPU/GE2D work must finish before input release. Flush/stop wakes blocked acquisitions,
and outstanding output owns its fd until release. Do not upscale.

YUV444 capture uses the driver's extension and byte ordering, then
[VUY repacking](src/vuy24.h) preserves full chroma in standard system-memory output.
This copies every frame. Cacheable read-only mapping plus DMA-BUF synchronization is required;
the older write-combine mapping is functionally valid but too slow for full-frame processing.
Fuse copy/reorder and reuse output instead of adding repeated full-frame reads.
Runtime color parsing is necessary because vendor and standard enum numbers differ.
Keep existing applications on their supported format until the complete encoder path supports full chroma.
A diagnostic format change affects every consumer and needs HDMI service restart;
restore the normal capture configuration before restarting KVM applications.

## Packed HDR conversion

[AMLY unpacking](src/amly.h), [GPU conversion](src/amly-gpu.c) and
[GPU scaling](src/amly-gpu-scale.c) preserve ten-bit PQ samples.
Use the board launcher's private matching Mali ICD, without global graphics-library symlinks.
Full-packed input and byte-order configuration must match the driver; HLG, twelve-bit and other packing are unqualified.
Scaling preserves aspect ratio and requires Mali with contiguous output; missing support fails explicitly.
Native-size startup can fall back to reusable CPU buffers, but a GPU error after submission must fail the stream,
never return potentially incomplete pixels. CPU SIMD handles complete blocks with a scalar padded-row tail.

Bounded output pools are allocated before encoder workspace and retained until SDK input retirement.
This budget depends on the low-delay SDK retiring input on completed calls; an output packet alone is not a general
retirement signal. Jobs finish and release temporary imports before capture requeue, and flushing wakes pool waits.
Scaling keeps full chroma until interpolation/subsampling; it applies no tone curve.
Binary16 represents ten-bit samples exactly, but texture filtering has finite subtexel precision.
Keep pitch, padding and colorimetry distinct from visible dimensions.

## Native HDR capture on the experimental V4L2 backend

[vim4amlysrc](src/gstvim4amlysrc.c) negotiates packed AMLY, then publishes P010 only after GPU completion.
It never maps pixels or falls back to CPU. Capture DMA is requeued only after its fence;
separate bounded output storage remains owned downstream. Flush wakes pool waits and capture polling.
VideoMeta carries padded pitch, UV offset and storage height, independent of visible geometry.
Height-only downscaling preserves aspect ratio; explicit pairs use the exact geometry contract.

The shared shaders are [unpack](src/amly-to-p010.comp),
[texture input](src/amly-to-texture.comp) and [scaled output](src/texture-to-p010.comp).
Real external-memory DMA capabilities are required; software devices are rejected.
Core/extension availability checks establish API requirements, not acceptance on kbase and Panfrost.
Consumers must read fresh standard HDR10 controls and insert metadata.
Never treat PQ capture as an SDR JPEG source.

## SDR branch candidate

[vim4pq2sdr](src/gstvim4pq2sdr.c) uses the same fence/external-memory ownership to produce separate NV12 DMA.
It accepts its validated padded P010 layout, passes compatible SDR through and rejects other PQ formats/colorimetry.
There is no CPU pixel conversion/upload fallback. Global curve preferences can be overridden by explicit properties;
shader arithmetic is in [p010-to-sdr.comp](src/p010-to-sdr.comp).
Tone mapping preserves midtones while converting gamut/range for SDR.

Main10 encoding keeps the original P010. Screenshots open a valve for one fresh shared frame,
tone map, then use ordinary SDR scaling/JPEG; continuous JPEG uses a leaky bounded raw queue.
Neither opens VDIN again. GPU failure terminates the affected pipeline.
CPU JPEG maps only the final SDR DMA with standard GStreamer CPU-access synchronization.

The offline [comparison](tests/pq-sdr-compare.py) runs actual SPIR-V on an explicitly selected software ICD,
checks padded offsets/poisoned rows against a double-precision reference, and saves results.
Meson covers negotiation, policy changes and failed-start cleanup.
These checks do not qualify board DMA import, cache ownership or either GPU stack's throughput.

## Hardware regression

All hardware diagnostics require exclusive idle-board ownership; they are not automatic tests.
Options and fixture geometry live in the named test source or its help.

- [AMLY comparison](tests/amly-gpu-compare.c): synthetic extremes/random data, boundaries, unusual pitches and real captured input.
  Native conversion is byte-exact; scaled output uses a separate bilinear reference and checks low bits/padding.
  Replaying one frame does not measure capture/encode/browser throughput.
- [SDR scale comparison](tests/sdr-scale-compare.c): poisoned padding, visible luma/chroma, timestamps and reuse.
  Its throughput is GE2D-only.
- [Capture fixture](tests/capture-pattern.swift) and [integrity checker](tests/capture-integrity.c):
  use a physical source display with the fixture's required scaling. Dynamic IDs and single-pixel chroma test
  checksum, tearing, repeats/skips and chroma preservation. Source/display cadence can also cause repeats/skips.
  This covers capture/repack to appsink, not encoding or network.

For standard raw 444, select the node using QUERYCAP and use the checker's V4L2 mode with the private YUV24 patch.
It requires real SDR YCbCr444 input; unsupported input/kernel fails explicitly.
That mode samples mapped complete frames and copies only an optional final frame, so timing is not a full CPU copy.
Vendor mode retains full repack coverage.

Timestamp, variable-rate caps and renegotiation behavior live in [vfmcapsrc](src/gstvfmcapsrc.c).
Delivery timing compares kernel delivery with dequeue, not HDMI exposure or display scanout;
the delivery sequence is not proof of a source frame counter.
Temporary timing/debug variables add overhead and must be removed after measurement.
Historical evidence is in the workspace
[bringup index](https://github.com/vim4-kvm/vim4-kvm/blob/main/docs/bringup/README.md).
Both GPU stacks, native HDR viewers and hardware VRR require their own board acceptance.

## License

[LGPL-2.1-or-later](LICENSE), matching the plugin declaration.
