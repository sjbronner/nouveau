/*
 * Copyright 2013 Red Hat Inc.
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
#include "nv50.h"

#include <subdev/bios.h>
#include <subdev/bios/init.h>

static u64
g84_devinit_disable(struct nvkm_devinit *devinit)
{
	struct nv50_devinit *init = (void *)devinit;
	u32 r001540 = nv_rd32(init, 0x001540);
	u32 r00154c = nv_rd32(init, 0x00154c);
	u64 disable = 0ULL;

	if (!(r001540 & 0x40000000)) {
		disable |= (1ULL << NVDEV_ENGINE_MPEG);
		disable |= (1ULL << NVDEV_ENGINE_VP);
		disable |= (1ULL << NVDEV_ENGINE_BSP);
		disable |= (1ULL << NVDEV_ENGINE_CIPHER);
	}

	if (!(r00154c & 0x00000004))
		disable |= (1ULL << NVDEV_ENGINE_DISP);
	if (!(r00154c & 0x00000020))
		disable |= (1ULL << NVDEV_ENGINE_BSP);
	if (!(r00154c & 0x00000040))
		disable |= (1ULL << NVDEV_ENGINE_CIPHER);

	return disable;
}

struct nvkm_oclass *
g84_devinit_oclass = &(struct nvkm_devinit_impl) {
	.base.handle = NV_SUBDEV(DEVINIT, 0x84),
	.base.ofuncs = &(struct nvkm_ofuncs) {
		.ctor = nv50_devinit_ctor,
		.dtor = _nvkm_devinit_dtor,
		.init = nv50_devinit_init,
		.fini = _nvkm_devinit_fini,
	},
	.pll_set = nv50_devinit_pll_set,
	.disable = g84_devinit_disable,
	.post = nvbios_init,
}.base;
