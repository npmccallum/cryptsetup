/*
 * LUKS - Linux Unified Key Setup
 *
 * Copyright (C) 2004-2006, Clemens Fruhwirth <clemens@endorphin.org>
 * Copyright (C) 2009-2012, Red Hat, Inc. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include "internal.h"

static void _error_hint(struct crypt_device *ctx, const char *device,
			const char *cipher_spec, const char *mode, size_t keyLength)
{
	log_err(ctx, _("Failed to setup dm-crypt key mapping for device %s.\n"
			"Check that kernel supports %s cipher (check syslog for more info).\n"),
			device, cipher_spec);

	if (!strncmp(mode, "xts", 3) && (keyLength != 256 && keyLength != 512))
		log_err(ctx,  _("Key size in XTS mode must be 256 or 512 bits.\n"));
}

static int LUKS_endec_template(char *src, size_t srcLength,
			       const char *cipher, const char *cipher_mode,
			       struct volume_key *vk,
			       unsigned int sector,
			       ssize_t (*func)(int, int, void *, size_t),
			       int mode,
			       struct crypt_device *ctx)
{
	char name[PATH_MAX], path[PATH_MAX];
	char cipher_spec[MAX_CIPHER_LEN * 3];
	struct crypt_dm_active_device dmd = {
		.target = DM_CRYPT,
		.uuid   = NULL,
		.flags  = CRYPT_ACTIVATE_PRIVATE,
		.data_device = crypt_metadata_device(ctx),
		.u.crypt = {
			.cipher = cipher_spec,
			.vk     = vk,
			.offset = sector,
			.iv_offset = 0,
		}
	};
	int r, bsize, devfd = -1;

	bsize = device_block_size(dmd.data_device);
	if (bsize <= 0)
		return -EINVAL;

	dmd.size = size_round_up(srcLength, bsize) / SECTOR_SIZE;

	if (mode == O_RDONLY)
		dmd.flags |= CRYPT_ACTIVATE_READONLY;

	if (snprintf(name, sizeof(name), "temporary-cryptsetup-%d", getpid()) < 0)
		return -ENOMEM;
	if (snprintf(path, sizeof(path), "%s/%s", dm_get_dir(), name) < 0)
		return -ENOMEM;
	if (snprintf(cipher_spec, sizeof(cipher_spec), "%s-%s", cipher, cipher_mode) < 0)
		return -ENOMEM;

	r = device_block_adjust(ctx, dmd.data_device, DEV_OK,
				dmd.u.crypt.offset, &dmd.size, &dmd.flags);
	if (r < 0) {
		log_err(ctx, _("Device %s doesn't exist or access denied.\n"),
			device_path(dmd.data_device));
		return -EIO;
	}

	if (mode != O_RDONLY && dmd.flags & CRYPT_ACTIVATE_READONLY) {
		log_err(ctx, _("Cannot write to device %s, permission denied.\n"),
			device_path(dmd.data_device));
		return -EACCES;
	}

	r = dm_create_device(ctx, name, "TEMP", &dmd, 0);
	if (r < 0) {
		if (r != -EACCES && r != -ENOTSUP)
			_error_hint(ctx, device_path(dmd.data_device),
				    cipher_spec, cipher_mode, vk->keylength * 8);
		return -EIO;
	}

	devfd = open(path, mode | O_DIRECT | O_SYNC);
	if (devfd == -1) {
		log_err(ctx, _("Failed to open temporary keystore device.\n"));
		r = -EIO;
		goto out;
	}

	r = func(devfd, bsize, src, srcLength);
	if (r < 0) {
		log_err(ctx, _("Failed to access temporary keystore device.\n"));
		r = -EIO;
	} else
		r = 0;
 out:
	if(devfd != -1)
		close(devfd);
	dm_remove_device(ctx, name, 1, dmd.size);
	return r;
}

int LUKS_encrypt_to_storage(char *src, size_t srcLength,
			    const char *cipher,
			    const char *cipher_mode,
			    struct volume_key *vk,
			    unsigned int sector,
			    struct crypt_device *ctx)
{
	return LUKS_endec_template(src, srcLength, cipher, cipher_mode,
				   vk, sector, write_blockwise, O_RDWR, ctx);
}

int LUKS_decrypt_from_storage(char *dst, size_t dstLength,
			      const char *cipher,
			      const char *cipher_mode,
			      struct volume_key *vk,
			      unsigned int sector,
			      struct crypt_device *ctx)
{
	return LUKS_endec_template(dst, dstLength, cipher, cipher_mode,
				   vk, sector, read_blockwise, O_RDONLY, ctx);
}
