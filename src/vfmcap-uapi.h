/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* Userspace view of the vfm_cap custom ioctl (drivers/media/vfm_cap). */
#ifndef VFMCAP_UAPI_H
#define VFMCAP_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/* After VIDIOC_DQBUF: DMA-buf fd for the frame behind token @index. */
struct vfm_cap_dmabuf_req {
	__u32 index;
	__s32 fd;
	__u32 size;
	__u32 reserved;
};

#define VFM_CAP_IOC_GET_DMABUF _IOWR('V', 192, struct vfm_cap_dmabuf_req)

#endif
