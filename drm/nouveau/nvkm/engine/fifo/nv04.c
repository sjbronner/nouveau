/*
 * Copyright 2012 Red Hat Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: Ben Skeggs
 */
#include "nv04.h"
#include "channv04.h"
#include "regsnv04.h"

#include <core/handle.h>
#include <core/ramht.h>
#include <subdev/instmem.h>
#include <subdev/timer.h>
#include <engine/sw.h>

static struct ramfc_desc
nv04_ramfc[] = {
	{ 32,  0, 0x00,  0, NV04_PFIFO_CACHE1_DMA_PUT },
	{ 32,  0, 0x04,  0, NV04_PFIFO_CACHE1_DMA_GET },
	{ 16,  0, 0x08,  0, NV04_PFIFO_CACHE1_DMA_INSTANCE },
	{ 16, 16, 0x08,  0, NV04_PFIFO_CACHE1_DMA_DCOUNT },
	{ 32,  0, 0x0c,  0, NV04_PFIFO_CACHE1_DMA_STATE },
	{ 32,  0, 0x10,  0, NV04_PFIFO_CACHE1_DMA_FETCH },
	{ 32,  0, 0x14,  0, NV04_PFIFO_CACHE1_ENGINE },
	{ 32,  0, 0x18,  0, NV04_PFIFO_CACHE1_PULL1 },
	{}
};

void
nv04_fifo_pause(struct nvkm_fifo *obj, unsigned long *pflags)
__acquires(fifo->base.lock)
{
	struct nv04_fifo *fifo = container_of(obj, typeof(*fifo), base);
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	unsigned long flags;

	spin_lock_irqsave(&fifo->base.lock, flags);
	*pflags = flags;

	nvkm_wr32(device, NV03_PFIFO_CACHES, 0x00000000);
	nvkm_mask(device, NV04_PFIFO_CACHE1_PULL0, 0x00000001, 0x00000000);

	/* in some cases the puller may be left in an inconsistent state
	 * if you try to stop it while it's busy translating handles.
	 * sometimes you get a CACHE_ERROR, sometimes it just fails
	 * silently; sending incorrect instance offsets to PGRAPH after
	 * it's started up again.
	 *
	 * to avoid this, we invalidate the most recently calculated
	 * instance.
	 */
	nvkm_msec(device, 2000,
		u32 tmp = nvkm_rd32(device, NV04_PFIFO_CACHE1_PULL0);
		if (!(tmp & NV04_PFIFO_CACHE1_PULL0_HASH_BUSY))
			break;
	);

	if (nvkm_rd32(device, NV04_PFIFO_CACHE1_PULL0) &
			  NV04_PFIFO_CACHE1_PULL0_HASH_FAILED)
		nvkm_wr32(device, NV03_PFIFO_INTR_0, NV_PFIFO_INTR_CACHE_ERROR);

	nvkm_wr32(device, NV04_PFIFO_CACHE1_HASH, 0x00000000);
}

void
nv04_fifo_start(struct nvkm_fifo *obj, unsigned long *pflags)
__releases(fifo->base.lock)
{
	struct nv04_fifo *fifo = container_of(obj, typeof(*fifo), base);
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	unsigned long flags = *pflags;

	nvkm_mask(device, NV04_PFIFO_CACHE1_PULL0, 0x00000001, 0x00000001);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 0x00000001);

	spin_unlock_irqrestore(&fifo->base.lock, flags);
}

static const char *
nv_dma_state_err(u32 state)
{
	static const char * const desc[] = {
		"NONE", "CALL_SUBR_ACTIVE", "INVALID_MTHD", "RET_SUBR_INACTIVE",
		"INVALID_CMD", "IB_EMPTY"/* NV50+ */, "MEM_FAULT", "UNK"
	};
	return desc[(state >> 29) & 0x7];
}

static bool
nv04_fifo_swmthd(struct nvkm_device *device, u32 chid, u32 addr, u32 data)
{
	struct nvkm_sw *sw = device->sw;
	const int subc = (addr & 0x0000e000) >> 13;
	const int mthd = (addr & 0x00001ffc);
	const u32 mask = 0x0000000f << (subc * 4);
	u32 engine = nvkm_rd32(device, 0x003280);
	bool handled = false;

	switch (mthd) {
	case 0x0000 ... 0x0000: /* subchannel's engine -> software */
		nvkm_wr32(device, 0x003280, (engine &= ~mask));
	case 0x0180 ... 0x01fc: /* handle -> instance */
		data = nvkm_rd32(device, 0x003258) & 0x0000ffff;
	case 0x0100 ... 0x017c:
	case 0x0200 ... 0x1ffc: /* pass method down to sw */
		if (!(engine & mask) && sw)
			handled = nvkm_sw_mthd(sw, chid, subc, mthd, data);
		break;
	default:
		break;
	}

	return handled;
}

static void
nv04_fifo_cache_error(struct nv04_fifo *fifo, u32 chid, u32 get)
{
	struct nvkm_subdev *subdev = &fifo->base.engine.subdev;
	struct nvkm_device *device = subdev->device;
	u32 pull0 = nvkm_rd32(device, 0x003250);
	u32 mthd, data;
	int ptr;

	/* NV_PFIFO_CACHE1_GET actually goes to 0xffc before wrapping on my
	 * G80 chips, but CACHE1 isn't big enough for this much data.. Tests
	 * show that it wraps around to the start at GET=0x800.. No clue as to
	 * why..
	 */
	ptr = (get & 0x7ff) >> 2;

	if (device->card_type < NV_40) {
		mthd = nvkm_rd32(device, NV04_PFIFO_CACHE1_METHOD(ptr));
		data = nvkm_rd32(device, NV04_PFIFO_CACHE1_DATA(ptr));
	} else {
		mthd = nvkm_rd32(device, NV40_PFIFO_CACHE1_METHOD(ptr));
		data = nvkm_rd32(device, NV40_PFIFO_CACHE1_DATA(ptr));
	}

	if (!(pull0 & 0x00000100) ||
	    !nv04_fifo_swmthd(device, chid, mthd, data)) {
		const char *client_name =
			nvkm_client_name_for_fifo_chid(&fifo->base, chid);
		nvkm_error(subdev, "CACHE_ERROR - "
			   "ch %d [%s] subc %d mthd %04x data %08x\n",
			   chid, client_name, (mthd >> 13) & 7, mthd & 0x1ffc,
			   data);
	}

	nvkm_wr32(device, NV04_PFIFO_CACHE1_DMA_PUSH, 0);
	nvkm_wr32(device, NV03_PFIFO_INTR_0, NV_PFIFO_INTR_CACHE_ERROR);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0,
		nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH0) & ~1);
	nvkm_wr32(device, NV03_PFIFO_CACHE1_GET, get + 4);
	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0,
		nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH0) | 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_HASH, 0);

	nvkm_wr32(device, NV04_PFIFO_CACHE1_DMA_PUSH,
		nvkm_rd32(device, NV04_PFIFO_CACHE1_DMA_PUSH) | 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
}

static void
nv04_fifo_dma_pusher(struct nv04_fifo *fifo, u32 chid)
{
	struct nvkm_subdev *subdev = &fifo->base.engine.subdev;
	struct nvkm_device *device = subdev->device;
	u32 dma_get = nvkm_rd32(device, 0x003244);
	u32 dma_put = nvkm_rd32(device, 0x003240);
	u32 push = nvkm_rd32(device, 0x003220);
	u32 state = nvkm_rd32(device, 0x003228);
	const char *client_name;

	client_name = nvkm_client_name_for_fifo_chid(&fifo->base, chid);

	if (device->card_type == NV_50) {
		u32 ho_get = nvkm_rd32(device, 0x003328);
		u32 ho_put = nvkm_rd32(device, 0x003320);
		u32 ib_get = nvkm_rd32(device, 0x003334);
		u32 ib_put = nvkm_rd32(device, 0x003330);

		nvkm_error(subdev, "DMA_PUSHER - "
			   "ch %d [%s] get %02x%08x put %02x%08x ib_get %08x "
			   "ib_put %08x state %08x (err: %s) push %08x\n",
			   chid, client_name, ho_get, dma_get, ho_put, dma_put,
			   ib_get, ib_put, state, nv_dma_state_err(state),
			   push);

		/* METHOD_COUNT, in DMA_STATE on earlier chipsets */
		nvkm_wr32(device, 0x003364, 0x00000000);
		if (dma_get != dma_put || ho_get != ho_put) {
			nvkm_wr32(device, 0x003244, dma_put);
			nvkm_wr32(device, 0x003328, ho_put);
		} else
		if (ib_get != ib_put)
			nvkm_wr32(device, 0x003334, ib_put);
	} else {
		nvkm_error(subdev, "DMA_PUSHER - ch %d [%s] get %08x put %08x "
				   "state %08x (err: %s) push %08x\n",
			   chid, client_name, dma_get, dma_put, state,
			   nv_dma_state_err(state), push);

		if (dma_get != dma_put)
			nvkm_wr32(device, 0x003244, dma_put);
	}

	nvkm_wr32(device, 0x003228, 0x00000000);
	nvkm_wr32(device, 0x003220, 0x00000001);
	nvkm_wr32(device, 0x002100, NV_PFIFO_INTR_DMA_PUSHER);
}

void
nv04_fifo_intr(struct nvkm_subdev *subdev)
{
	struct nvkm_device *device = subdev->device;
	struct nv04_fifo *fifo = (void *)subdev;
	u32 mask = nvkm_rd32(device, NV03_PFIFO_INTR_EN_0);
	u32 stat = nvkm_rd32(device, NV03_PFIFO_INTR_0) & mask;
	u32 reassign, chid, get, sem;

	reassign = nvkm_rd32(device, NV03_PFIFO_CACHES) & 1;
	nvkm_wr32(device, NV03_PFIFO_CACHES, 0);

	chid = nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH1) & fifo->base.max;
	get  = nvkm_rd32(device, NV03_PFIFO_CACHE1_GET);

	if (stat & NV_PFIFO_INTR_CACHE_ERROR) {
		nv04_fifo_cache_error(fifo, chid, get);
		stat &= ~NV_PFIFO_INTR_CACHE_ERROR;
	}

	if (stat & NV_PFIFO_INTR_DMA_PUSHER) {
		nv04_fifo_dma_pusher(fifo, chid);
		stat &= ~NV_PFIFO_INTR_DMA_PUSHER;
	}

	if (stat & NV_PFIFO_INTR_SEMAPHORE) {
		stat &= ~NV_PFIFO_INTR_SEMAPHORE;
		nvkm_wr32(device, NV03_PFIFO_INTR_0, NV_PFIFO_INTR_SEMAPHORE);

		sem = nvkm_rd32(device, NV10_PFIFO_CACHE1_SEMAPHORE);
		nvkm_wr32(device, NV10_PFIFO_CACHE1_SEMAPHORE, sem | 0x1);

		nvkm_wr32(device, NV03_PFIFO_CACHE1_GET, get + 4);
		nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	}

	if (device->card_type == NV_50) {
		if (stat & 0x00000010) {
			stat &= ~0x00000010;
			nvkm_wr32(device, 0x002100, 0x00000010);
		}

		if (stat & 0x40000000) {
			nvkm_wr32(device, 0x002100, 0x40000000);
			nvkm_fifo_uevent(&fifo->base);
			stat &= ~0x40000000;
		}
	}

	if (stat) {
		nvkm_warn(subdev, "intr %08x\n", stat);
		nvkm_mask(device, NV03_PFIFO_INTR_EN_0, stat, 0x00000000);
		nvkm_wr32(device, NV03_PFIFO_INTR_0, stat);
	}

	nvkm_wr32(device, NV03_PFIFO_CACHES, reassign);
}

int
nv04_fifo_init(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	struct nvkm_instmem *imem = device->imem;
	struct nvkm_ramht *ramht = imem->ramht;
	struct nvkm_memory *ramro = imem->ramro;
	struct nvkm_memory *ramfc = imem->ramfc;
	int ret;

	ret = nvkm_fifo_init(&fifo->base);
	if (ret)
		return ret;

	nvkm_wr32(device, NV04_PFIFO_DELAY_0, 0x000000ff);
	nvkm_wr32(device, NV04_PFIFO_DMA_TIMESLICE, 0x0101ffff);

	nvkm_wr32(device, NV03_PFIFO_RAMHT, (0x03 << 24) /* search 128 */ |
					    ((ramht->bits - 9) << 16) |
					    (ramht->gpuobj->addr >> 8));
	nvkm_wr32(device, NV03_PFIFO_RAMRO, nvkm_memory_addr(ramro) >> 8);
	nvkm_wr32(device, NV03_PFIFO_RAMFC, nvkm_memory_addr(ramfc) >> 8);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH1, fifo->base.max);

	nvkm_wr32(device, NV03_PFIFO_INTR_0, 0xffffffff);
	nvkm_wr32(device, NV03_PFIFO_INTR_EN_0, 0xffffffff);

	nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 1);
	nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 1);
	return 0;
}

void
nv04_fifo_dtor(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object;
	nvkm_fifo_destroy(&fifo->base);
}

static int
nv04_fifo_ctor(struct nvkm_object *parent, struct nvkm_object *engine,
	       struct nvkm_oclass *oclass, void *data, u32 size,
	       struct nvkm_object **pobject)
{
	struct nv04_fifo *fifo;
	int ret;

	ret = nvkm_fifo_create(parent, engine, oclass, 0, 15, &fifo);
	*pobject = nv_object(fifo);
	if (ret)
		return ret;

	nv_subdev(fifo)->unit = 0x00000100;
	nv_subdev(fifo)->intr = nv04_fifo_intr;
	nv_engine(fifo)->cclass = &nv04_fifo_cclass;
	nv_engine(fifo)->sclass = nv04_fifo_sclass;
	fifo->base.pause = nv04_fifo_pause;
	fifo->base.start = nv04_fifo_start;
	fifo->ramfc_desc = nv04_ramfc;
	return 0;
}

struct nvkm_oclass *
nv04_fifo_oclass = &(struct nvkm_oclass) {
	.handle = NV_ENGINE(FIFO, 0x04),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv04_fifo_ctor,
		.dtor = nv04_fifo_dtor,
		.init = nv04_fifo_init,
		.fini = _nvkm_fifo_fini,
	},
};
