/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation.
 *   All rights reserved.
 *   Copyright (c) 2022-2023 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "spdk/blob_bdev.h"
#include "spdk/blob.h"
#include "spdk/thread.h"
#include "spdk/log.h"
#include "spdk/endian.h"
#define __SPDK_BDEV_MODULE_ONLY
#include "spdk/bdev_module.h"

struct blob_bdev {
	struct spdk_bs_dev	bs_dev;
	struct spdk_bdev	*bdev;
	struct spdk_bdev_desc	*desc;
};

struct blob_resubmit {
	struct spdk_bdev_io_wait_entry bdev_io_wait;
	enum spdk_bdev_io_type io_type;
	struct spdk_bs_dev *dev;
	struct spdk_io_channel *channel;
	void *payload;
	int iovcnt;
	uint64_t lba;
	uint64_t src_lba;
	uint32_t lba_count;
	struct spdk_bs_dev_cb_args *cb_args;
	struct spdk_blob_ext_io_opts *ext_io_opts;
};
static void bdev_blob_resubmit(void *);

static inline struct spdk_bdev_desc *
__get_desc(struct spdk_bs_dev *dev)
{
	return ((struct blob_bdev *)dev)->desc;
}

static inline struct spdk_bdev *
__get_bdev(struct spdk_bs_dev *dev)
{
	return ((struct blob_bdev *)dev)->bdev;
}

static void
bdev_blob_io_complete(struct spdk_bdev_io *bdev_io, bool success, void *arg)
{
	struct spdk_bs_dev_cb_args *cb_args = arg;
	int bserrno;

	if (success) {
		bserrno = 0;
	} else {
		bserrno = -EIO;
	}
	cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, bserrno);
	spdk_bdev_free_io(bdev_io);
}

static void
bdev_blob_queue_io(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		   int iovcnt, uint64_t lba, uint64_t src_lba, uint32_t lba_count,
		   enum spdk_bdev_io_type io_type, struct spdk_bs_dev_cb_args *cb_args,
		   struct spdk_blob_ext_io_opts *ext_io_opts)
{
	int rc;
	struct spdk_bdev *bdev = __get_bdev(dev);
	struct blob_resubmit *ctx;

	ctx = calloc(1, sizeof(struct blob_resubmit));

	if (ctx == NULL) {
		SPDK_ERRLOG("Not enough memory to queue io\n");
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, -ENOMEM);
		return;
	}

	ctx->io_type = io_type;
	ctx->dev = dev;
	ctx->channel = channel;
	ctx->payload = payload;
	ctx->iovcnt = iovcnt;
	ctx->lba = lba;
	ctx->src_lba = src_lba;
	ctx->lba_count = lba_count;
	ctx->cb_args = cb_args;
	ctx->bdev_io_wait.bdev = bdev;
	ctx->bdev_io_wait.cb_fn = bdev_blob_resubmit;
	ctx->bdev_io_wait.cb_arg = ctx;
	ctx->ext_io_opts = ext_io_opts;

	rc = spdk_bdev_queue_io_wait(bdev, channel, &ctx->bdev_io_wait);
	if (rc != 0) {
		SPDK_ERRLOG("Queue io failed, rc=%d\n", rc);
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
		free(ctx);
		assert(false);
	}
}

static void
bdev_blob_read(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
	       uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_read_blocks(__get_desc(dev), channel, payload, lba,
				   lba_count, bdev_blob_io_complete, cb_args);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, payload, 0, lba, 0,
				   lba_count, SPDK_BDEV_IO_TYPE_READ, cb_args, NULL);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_write(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, void *payload,
		uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_write_blocks(__get_desc(dev), channel, payload, lba,
				    lba_count, bdev_blob_io_complete, cb_args);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, payload, 0, lba, 0,
				   lba_count, SPDK_BDEV_IO_TYPE_WRITE, cb_args, NULL);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_readv(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		struct iovec *iov, int iovcnt,
		uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_readv_blocks(__get_desc(dev), channel, iov, iovcnt, lba,
				    lba_count, bdev_blob_io_complete, cb_args);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, iov, iovcnt, lba, 0,
				   lba_count, SPDK_BDEV_IO_TYPE_READ, cb_args, NULL);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_writev(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		 struct iovec *iov, int iovcnt,
		 uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_writev_blocks(__get_desc(dev), channel, iov, iovcnt, lba,
				     lba_count, bdev_blob_io_complete, cb_args);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, iov, iovcnt, lba, 0,
				   lba_count, SPDK_BDEV_IO_TYPE_WRITE, cb_args, NULL);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static inline void
blob_ext_io_opts_to_bdev_opts(struct spdk_bdev_ext_io_opts *dst, struct spdk_blob_ext_io_opts *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->size = sizeof(*dst);
	dst->memory_domain = src->memory_domain;
	dst->memory_domain_ctx = src->memory_domain_ctx;
}

static void
bdev_blob_readv_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		    struct iovec *iov, int iovcnt,
		    uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		    struct spdk_blob_ext_io_opts *io_opts)
{
	struct spdk_bdev_ext_io_opts bdev_io_opts;
	int rc;

	blob_ext_io_opts_to_bdev_opts(&bdev_io_opts, io_opts);
	rc = spdk_bdev_readv_blocks_ext(__get_desc(dev), channel, iov, iovcnt, lba, lba_count,
					bdev_blob_io_complete, cb_args, &bdev_io_opts);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, iov, iovcnt, lba, 0, lba_count, SPDK_BDEV_IO_TYPE_READ, cb_args,
				   io_opts);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_writev_ext(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
		     struct iovec *iov, int iovcnt,
		     uint64_t lba, uint32_t lba_count, struct spdk_bs_dev_cb_args *cb_args,
		     struct spdk_blob_ext_io_opts *io_opts)
{
	struct spdk_bdev_ext_io_opts bdev_io_opts;
	int rc;

	blob_ext_io_opts_to_bdev_opts(&bdev_io_opts, io_opts);
	rc = spdk_bdev_writev_blocks_ext(__get_desc(dev), channel, iov, iovcnt, lba, lba_count,
					 bdev_blob_io_complete, cb_args, &bdev_io_opts);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, iov, iovcnt, lba, 0, lba_count, SPDK_BDEV_IO_TYPE_WRITE, cb_args,
				   io_opts);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_write_zeroes(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, uint64_t lba,
		       uint64_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_write_zeroes_blocks(__get_desc(dev), channel, lba,
					   lba_count, bdev_blob_io_complete, cb_args);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, NULL, 0, lba, 0,
				   lba_count, SPDK_BDEV_IO_TYPE_WRITE_ZEROES, cb_args, NULL);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_unmap(struct spdk_bs_dev *dev, struct spdk_io_channel *channel, uint64_t lba,
		uint64_t lba_count, struct spdk_bs_dev_cb_args *cb_args)
{
	struct blob_bdev *blob_bdev = (struct blob_bdev *)dev;
	int rc;

	if (spdk_bdev_io_type_supported(blob_bdev->bdev, SPDK_BDEV_IO_TYPE_UNMAP)) {
		rc = spdk_bdev_unmap_blocks(__get_desc(dev), channel, lba, lba_count,
					    bdev_blob_io_complete, cb_args);
		if (rc == -ENOMEM) {
			bdev_blob_queue_io(dev, channel, NULL, 0, lba, 0,
					   lba_count, SPDK_BDEV_IO_TYPE_UNMAP, cb_args, NULL);
		} else if (rc != 0) {
			cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
		}
	} else {
		/*
		 * If the device doesn't support unmap, immediately complete
		 * the request. Blobstore does not rely on unmap zeroing
		 * data.
		 */
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, 0);
	}
}

static void
bdev_blob_copy(struct spdk_bs_dev *dev, struct spdk_io_channel *channel,
	       uint64_t dst_lba, uint64_t src_lba, uint64_t lba_count,
	       struct spdk_bs_dev_cb_args *cb_args)
{
	int rc;

	rc = spdk_bdev_copy_blocks(__get_desc(dev), channel,
				   dst_lba, src_lba, lba_count,
				   bdev_blob_io_complete, cb_args);
	if (rc == -ENOMEM) {
		bdev_blob_queue_io(dev, channel, NULL, 0, dst_lba, src_lba,
				   lba_count, SPDK_BDEV_IO_TYPE_COPY, cb_args, NULL);
	} else if (rc != 0) {
		cb_args->cb_fn(cb_args->channel, cb_args->cb_arg, rc);
	}
}

static void
bdev_blob_resubmit(void *arg)
{
	struct blob_resubmit *ctx = (struct blob_resubmit *) arg;

	switch (ctx->io_type) {
	case SPDK_BDEV_IO_TYPE_READ:
		if (ctx->iovcnt > 0) {
			bdev_blob_readv_ext(ctx->dev, ctx->channel, (struct iovec *) ctx->payload, ctx->iovcnt,
					    ctx->lba, ctx->lba_count, ctx->cb_args, ctx->ext_io_opts);
		} else {
			bdev_blob_read(ctx->dev, ctx->channel, ctx->payload,
				       ctx->lba, ctx->lba_count, ctx->cb_args);
		}
		break;
	case SPDK_BDEV_IO_TYPE_WRITE:
		if (ctx->iovcnt > 0) {
			bdev_blob_writev_ext(ctx->dev, ctx->channel, (struct iovec *) ctx->payload, ctx->iovcnt,
					     ctx->lba, ctx->lba_count, ctx->cb_args, ctx->ext_io_opts);
		} else {
			bdev_blob_write(ctx->dev, ctx->channel, ctx->payload,
					ctx->lba, ctx->lba_count, ctx->cb_args);
		}
		break;
	case SPDK_BDEV_IO_TYPE_UNMAP:
		bdev_blob_unmap(ctx->dev, ctx->channel,
				ctx->lba, ctx->lba_count, ctx->cb_args);
		break;
	case SPDK_BDEV_IO_TYPE_WRITE_ZEROES:
		bdev_blob_write_zeroes(ctx->dev, ctx->channel,
				       ctx->lba, ctx->lba_count, ctx->cb_args);
		break;
	case SPDK_BDEV_IO_TYPE_COPY:
		bdev_blob_copy(ctx->dev, ctx->channel,
			       ctx->lba, ctx->src_lba, ctx->lba_count, ctx->cb_args);
		break;
	default:
		SPDK_ERRLOG("Unsupported io type %d\n", ctx->io_type);
		assert(false);
		break;
	}
	free(ctx);
}

int
spdk_bs_bdev_claim(struct spdk_bs_dev *bs_dev, struct spdk_bdev_module *module)
{
	struct spdk_bdev_desc *desc = __get_desc(bs_dev);
	int rc;

	rc = spdk_bdev_module_claim_bdev_desc(desc, SPDK_BDEV_CLAIM_READ_MANY_WRITE_ONE,
					      NULL, module);
	if (rc != 0) {
		SPDK_ERRLOG("could not claim bs dev\n");
		return rc;
	}

	return rc;
}

static struct spdk_io_channel *
bdev_blob_create_channel(struct spdk_bs_dev *dev)
{
	struct blob_bdev *blob_bdev = (struct blob_bdev *)dev;

	return spdk_bdev_get_io_channel(blob_bdev->desc);
}

static void
bdev_blob_destroy_channel(struct spdk_bs_dev *dev, struct spdk_io_channel *channel)
{
	spdk_put_io_channel(channel);
}

static void
bdev_blob_destroy(struct spdk_bs_dev *bs_dev)
{
	struct spdk_bdev_desc *desc = __get_desc(bs_dev);

	spdk_bdev_close(desc);
	free(bs_dev);
}

static struct spdk_bdev *
bdev_blob_get_base_bdev(struct spdk_bs_dev *bs_dev)
{
	return __get_bdev(bs_dev);
}

static bool
bdev_blob_is_zeroes(struct spdk_bs_dev *dev, uint64_t lba, uint64_t lba_count)
{
	return false;
}

static bool
bdev_blob_translate_lba(struct spdk_bs_dev *dev, uint64_t lba, uint64_t *base_lba)
{
	*base_lba = lba;
	return true;
}

static void
blob_bdev_init(struct blob_bdev *b, struct spdk_bdev_desc *desc)
{
	struct spdk_bdev *bdev;

	bdev = spdk_bdev_desc_get_bdev(desc);
	assert(bdev != NULL);

	b->bdev = bdev;
	b->desc = desc;
	b->bs_dev.blockcnt = spdk_bdev_get_num_blocks(bdev);
	b->bs_dev.blocklen = spdk_bdev_get_block_size(bdev);
	b->bs_dev.create_channel = bdev_blob_create_channel;
	b->bs_dev.destroy_channel = bdev_blob_destroy_channel;
	b->bs_dev.destroy = bdev_blob_destroy;
	b->bs_dev.read = bdev_blob_read;
	b->bs_dev.write = bdev_blob_write;
	b->bs_dev.readv = bdev_blob_readv;
	b->bs_dev.writev = bdev_blob_writev;
	b->bs_dev.readv_ext = bdev_blob_readv_ext;
	b->bs_dev.writev_ext = bdev_blob_writev_ext;
	b->bs_dev.write_zeroes = bdev_blob_write_zeroes;
	b->bs_dev.unmap = bdev_blob_unmap;
	if (spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_COPY)) {
		b->bs_dev.copy = bdev_blob_copy;
	}
	b->bs_dev.get_base_bdev = bdev_blob_get_base_bdev;
	b->bs_dev.is_zeroes = bdev_blob_is_zeroes;
	b->bs_dev.translate_lba = bdev_blob_translate_lba;
}

int
spdk_bdev_create_bs_dev_ext(const char *bdev_name, spdk_bdev_event_cb_t event_cb,
			    void *event_ctx, struct spdk_bs_dev **_bs_dev)
{
	struct blob_bdev *b;
	struct spdk_bdev_desc *desc;
	int rc;

	b = calloc(1, sizeof(*b));

	if (b == NULL) {
		SPDK_ERRLOG("could not allocate blob_bdev\n");
		return -ENOMEM;
	}

	rc = spdk_bdev_open_ext(bdev_name, true, event_cb, event_ctx, &desc);
	if (rc != 0) {
		free(b);
		return rc;
	}

	blob_bdev_init(b, desc);

	*_bs_dev = &b->bs_dev;

	return 0;
}
