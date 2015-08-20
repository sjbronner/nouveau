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
#include "channv04.h"
#include "regsnv04.h"

#include <core/client.h>
#include <core/ramht.h>
#include <subdev/instmem.h>

#include <nvif/class.h>
#include <nvif/unpack.h>

int
nv04_fifo_context_attach(struct nvkm_object *parent,
			 struct nvkm_object *object)
{
	nv_engctx(object)->addr = nvkm_fifo_chan(parent)->chid;
	return 0;
}

void
nv04_fifo_object_detach(struct nvkm_object *parent, int cookie)
{
	struct nv04_fifo *fifo = (void *)parent->engine;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	mutex_lock(&nv_subdev(fifo)->mutex);
	nvkm_ramht_remove(imem->ramht, cookie);
	mutex_unlock(&nv_subdev(fifo)->mutex);
}

int
nv04_fifo_object_attach(struct nvkm_object *parent,
			struct nvkm_object *object, u32 handle)
{
	struct nv04_fifo *fifo = (void *)parent->engine;
	struct nv04_fifo_chan *chan = (void *)parent;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	u32 context, chid = chan->base.chid;
	int ret;

	if (nv_iclass(object, NV_GPUOBJ_CLASS))
		context = nv_gpuobj(object)->addr >> 4;
	else
		context = 0x00000004; /* just non-zero */

	if (object->engine) {
		switch (nv_engidx(object->engine)) {
		case NVDEV_ENGINE_DMAOBJ:
		case NVDEV_ENGINE_SW:
			context |= 0x00000000;
			break;
		case NVDEV_ENGINE_GR:
			context |= 0x00010000;
			break;
		case NVDEV_ENGINE_MPEG:
			context |= 0x00020000;
			break;
		default:
			return -EINVAL;
		}
	}

	context |= 0x80000000; /* valid */
	context |= chid << 24;

	mutex_lock(&nv_subdev(fifo)->mutex);
	ret = nvkm_ramht_insert(imem->ramht, NULL, chid, 0, handle, context);
	mutex_unlock(&nv_subdev(fifo)->mutex);
	return ret;
}

int
nv04_fifo_chan_fini(struct nvkm_object *object, bool suspend)
{
	struct nv04_fifo *fifo = (void *)object->engine;
	struct nv04_fifo_chan *chan = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	struct nvkm_memory *fctx = device->imem->ramfc;
	struct ramfc_desc *c;
	unsigned long flags;
	u32 data = chan->ramfc;
	u32 chid;

	/* prevent fifo context switches */
	spin_lock_irqsave(&fifo->base.lock, flags);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 0);

	/* if this channel is active, replace it with a null context */
	chid = nvkm_rd32(device, NV03_PFIFO_CACHE1_PUSH1) & fifo->base.max;
	if (chid == chan->base.chid) {
		nvkm_mask(device, NV04_PFIFO_CACHE1_DMA_PUSH, 0x00000001, 0);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 0);
		nvkm_mask(device, NV04_PFIFO_CACHE1_PULL0, 0x00000001, 0);

		c = fifo->ramfc_desc;
		do {
			u32 rm = ((1ULL << c->bits) - 1) << c->regs;
			u32 cm = ((1ULL << c->bits) - 1) << c->ctxs;
			u32 rv = (nvkm_rd32(device, c->regp) &  rm) >> c->regs;
			u32 cv = (nvkm_ro32(fctx, c->ctxp + data) & ~cm);
			nvkm_wo32(fctx, c->ctxp + data, cv | (rv << c->ctxs));
		} while ((++c)->bits);

		c = fifo->ramfc_desc;
		do {
			nvkm_wr32(device, c->regp, 0x00000000);
		} while ((++c)->bits);

		nvkm_wr32(device, NV03_PFIFO_CACHE1_GET, 0);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUT, 0);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH1, fifo->base.max);
		nvkm_wr32(device, NV03_PFIFO_CACHE1_PUSH0, 1);
		nvkm_wr32(device, NV04_PFIFO_CACHE1_PULL0, 1);
	}

	/* restore normal operation, after disabling dma mode */
	nvkm_mask(device, NV04_PFIFO_MODE, 1 << chan->base.chid, 0);
	nvkm_wr32(device, NV03_PFIFO_CACHES, 1);
	spin_unlock_irqrestore(&fifo->base.lock, flags);

	return nvkm_fifo_channel_fini(&chan->base, suspend);
}

int
nv04_fifo_chan_init(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object->engine;
	struct nv04_fifo_chan *chan = (void *)object;
	struct nvkm_device *device = fifo->base.engine.subdev.device;
	u32 mask = 1 << chan->base.chid;
	unsigned long flags;
	int ret;

	ret = nvkm_fifo_channel_init(&chan->base);
	if (ret)
		return ret;

	spin_lock_irqsave(&fifo->base.lock, flags);
	nvkm_mask(device, NV04_PFIFO_MODE, mask, mask);
	spin_unlock_irqrestore(&fifo->base.lock, flags);
	return 0;
}

void
nv04_fifo_chan_dtor(struct nvkm_object *object)
{
	struct nv04_fifo *fifo = (void *)object->engine;
	struct nv04_fifo_chan *chan = (void *)object;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	struct ramfc_desc *c = fifo->ramfc_desc;

	nvkm_kmap(imem->ramfc);
	do {
		nvkm_wo32(imem->ramfc, chan->ramfc + c->ctxp, 0x00000000);
	} while ((++c)->bits);
	nvkm_done(imem->ramfc);

	nvkm_fifo_channel_destroy(&chan->base);
}

static int
nv04_fifo_chan_ctor(struct nvkm_object *parent,
		    struct nvkm_object *engine,
		    struct nvkm_oclass *oclass, void *data, u32 size,
		    struct nvkm_object **pobject)
{
	union {
		struct nv03_channel_dma_v0 v0;
	} *args = data;
	struct nv04_fifo *fifo = (void *)engine;
	struct nvkm_instmem *imem = fifo->base.engine.subdev.device->imem;
	struct nv04_fifo_chan *chan;
	int ret;

	nvif_ioctl(parent, "create channel dma size %d\n", size);
	if (nvif_unpack(args->v0, 0, 0, false)) {
		nvif_ioctl(parent, "create channel dma vers %d pushbuf %llx "
				   "offset %08x\n", args->v0.version,
			   args->v0.pushbuf, args->v0.offset);
	} else
		return ret;

	ret = nvkm_fifo_channel_create(parent, engine, oclass, 0, 0x800000,
				       0x10000, args->v0.pushbuf,
				       (1ULL << NVDEV_ENGINE_DMAOBJ) |
				       (1ULL << NVDEV_ENGINE_SW) |
				       (1ULL << NVDEV_ENGINE_GR), &chan);
	*pobject = nv_object(chan);
	if (ret)
		return ret;

	args->v0.chid = chan->base.chid;

	nv_parent(chan)->object_attach = nv04_fifo_object_attach;
	nv_parent(chan)->object_detach = nv04_fifo_object_detach;
	nv_parent(chan)->context_attach = nv04_fifo_context_attach;
	chan->ramfc = chan->base.chid * 32;

	nvkm_kmap(imem->ramfc);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x00, args->v0.offset);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x04, args->v0.offset);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x08, chan->base.pushgpu->addr >> 4);
	nvkm_wo32(imem->ramfc, chan->ramfc + 0x10,
			       NV_PFIFO_CACHE1_DMA_FETCH_TRIG_128_BYTES |
			       NV_PFIFO_CACHE1_DMA_FETCH_SIZE_128_BYTES |
#ifdef __BIG_ENDIAN
			       NV_PFIFO_CACHE1_BIG_ENDIAN |
#endif
			       NV_PFIFO_CACHE1_DMA_FETCH_MAX_REQS_8);
	nvkm_done(imem->ramfc);
	return 0;
}

static struct nvkm_ofuncs
nv04_fifo_ofuncs = {
	.ctor = nv04_fifo_chan_ctor,
	.dtor = nv04_fifo_chan_dtor,
	.init = nv04_fifo_chan_init,
	.fini = nv04_fifo_chan_fini,
	.map  = _nvkm_fifo_channel_map,
	.rd32 = _nvkm_fifo_channel_rd32,
	.wr32 = _nvkm_fifo_channel_wr32,
	.ntfy = _nvkm_fifo_channel_ntfy
};

struct nvkm_oclass
nv04_fifo_sclass[] = {
	{ NV03_CHANNEL_DMA, &nv04_fifo_ofuncs },
	{}
};

int
nv04_fifo_context_ctor(struct nvkm_object *parent,
		       struct nvkm_object *engine,
		       struct nvkm_oclass *oclass, void *data, u32 size,
		       struct nvkm_object **pobject)
{
	struct nv04_fifo_base *base;
	int ret;

	ret = nvkm_fifo_context_create(parent, engine, oclass, NULL, 0x1000,
				       0x1000, NVOBJ_FLAG_HEAP, &base);
	*pobject = nv_object(base);
	if (ret)
		return ret;

	return 0;
}

struct nvkm_oclass
nv04_fifo_cclass = {
	.handle = NV_ENGCTX(FIFO, 0x04),
	.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv04_fifo_context_ctor,
		.dtor = _nvkm_fifo_context_dtor,
		.init = _nvkm_fifo_context_init,
		.fini = _nvkm_fifo_context_fini,
		.rd32 = _nvkm_fifo_context_rd32,
		.wr32 = _nvkm_fifo_context_wr32,
	},
};
