/*
 * lxc: linux Container library
 *
 * (C) Copyright IBM Corp. 2007, 2008
 *
 * Authors:
 * Daniel Lezcano <daniel.lezcano at free.fr>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 */

/*
 * this is all just a first shot for experiment.  If we go this route, much
 * shoudl change.  bdev should be a directory with per-bdev file.  Things which
 * I'm doing by calling out to userspace should sometimes be done through
 * libraries like liblvm2
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <sched.h>
#include <sys/mount.h>
#include <sys/wait.h>
#include <libgen.h>
#include "lxc.h"
#include "config.h"
#include "conf.h"
#include "bdev.h"
#include "log.h"
#include "error.h"
#include "utils.h"
#include "namespace.h"
#include "parse.h"

lxc_log_define(bdev, lxc);

/* Define unshare() if missing from the C library */
/* this is also in attach.c and lxccontainer.c: commonize it in utils.c */
#ifndef HAVE_UNSHARE
static int unshare(int flags)
{
#ifdef __NR_unshare
return syscall(__NR_unshare, flags);
#else
errno = ENOSYS;
return -1;
#endif
}
#endif

static int do_rsync(const char *src, const char *dest)
{
	// call out to rsync
	pid_t pid;
	char *s;
	size_t l;

	pid = fork();
	if (pid < 0)
		return -1;
	if (pid > 0)
		return wait_for_pid(pid);
	l = strlen(src) + 2;
	s = malloc(l);
	if (!s)
		exit(1);
	strcpy(s, src);
	s[l-2] = '/';
	s[l-1] = '\0';

	execlp("rsync", "rsync", "-a", s, dest, (char *)NULL);
	exit(1);
}

static int blk_getsize(const char *path, unsigned long *size)
{
	int fd, ret;

	fd = open(path, O_RDONLY);
	if (!fd)
		return -1;
	ret = ioctl(fd, BLKGETSIZE64, size);
	close(fd);
	return ret;
}

/*
 * These are copied from conf.c.  However as conf.c will be moved to using
 * the callback system, they can be pulled from there eventually, so we
 * don't need to pollute utils.c with these low level functions
 */
static int find_fstype_cb(char* buffer, void *data)
{
	struct cbarg {
		const char *rootfs;
		const char *target;
		int mntopt;
	} *cbarg = data;

	char *fstype;

	/* we don't try 'nodev' entries */
	if (strstr(buffer, "nodev"))
		return 0;

	fstype = buffer;
	fstype += lxc_char_left_gc(fstype, strlen(fstype));
	fstype[lxc_char_right_gc(fstype, strlen(fstype))] = '\0';

	DEBUG("trying to mount '%s'->'%s' with fstype '%s'",
	      cbarg->rootfs, cbarg->target, fstype);

	if (mount(cbarg->rootfs, cbarg->target, fstype, cbarg->mntopt, NULL)) {
		DEBUG("mount failed with error: %s", strerror(errno));
		return 0;
	}

	INFO("mounted '%s' on '%s', with fstype '%s'",
	     cbarg->rootfs, cbarg->target, fstype);

	return 1;
}

static int mount_unknow_fs(const char *rootfs, const char *target, int mntopt)
{
	int i;

	struct cbarg {
		const char *rootfs;
		const char *target;
		int mntopt;
	} cbarg = {
		.rootfs = rootfs,
		.target = target,
		.mntopt = mntopt,
	};

	/*
	 * find the filesystem type with brute force:
	 * first we check with /etc/filesystems, in case the modules
	 * are auto-loaded and fall back to the supported kernel fs
	 */
	char *fsfile[] = {
		"/etc/filesystems",
		"/proc/filesystems",
	};

	for (i = 0; i < sizeof(fsfile)/sizeof(fsfile[0]); i++) {

		int ret;

		if (access(fsfile[i], F_OK))
			continue;

		ret = lxc_file_for_each_line(fsfile[i], find_fstype_cb, &cbarg);
		if (ret < 0) {
			ERROR("failed to parse '%s'", fsfile[i]);
			return -1;
		}

		if (ret)
			return 0;
	}

	ERROR("failed to determine fs type for '%s'", rootfs);
	return -1;
}

static int do_mkfs(const char *path, const char *fstype)
{
	pid_t pid;

	if ((pid = fork()) < 0) {
		ERROR("error forking");
		return -1;
	}
	if (pid > 0)
		return wait_for_pid(pid);

	execlp("mkfs", "mkfs", "-t", fstype, path, NULL);
	exit(1);
}

static char *linkderef(char *path, char *dest)
{
	struct stat sbuf;
	ssize_t ret;

	ret = stat(path, &sbuf);
	if (ret < 0)
		return NULL;
	if (!S_ISLNK(sbuf.st_mode))
		return path;
	ret = readlink(path, dest, MAXPATHLEN);
	if (ret < 0) {
		SYSERROR("error reading link %s", path);
		return NULL;
	} else if (ret >= MAXPATHLEN) {
		ERROR("link in %s too long", path);
		return NULL;
	}
	dest[ret] = '\0';
	return dest;
}

/*
 * Given a bdev (presumably blockdev-based), detect the fstype
 * by trying mounting (in a private mntns) it.
 * @bdev: bdev to investigate
 * @type: preallocated char* in which to write the fstype
 * @len: length of passed in char*
 * Returns length of fstype, of -1 on error
 */
static int detect_fs(struct bdev *bdev, char *type, int len)
{
	int  p[2], ret;
	size_t linelen;
	pid_t pid;
	FILE *f;
	char *sp1, *sp2, *sp3, *line = NULL;

	if (!bdev || !bdev->src || !bdev->dest)
		return -1;

	if (pipe(p) < 0)
		return -1;
	if ((pid = fork()) < 0)
		return -1;
	if (pid > 0) {
		int status;
		close(p[1]);
		memset(type, 0, len);
		ret = read(p[0], type, len-1);
		close(p[0]);
		if (ret < 0) {
			SYSERROR("error reading from pipe");
			wait(&status);
			return -1;
		} else if (ret == 0) {
			ERROR("child exited early - fstype not found");
			wait(&status);
			return -1;
		}
		wait(&status);
		type[len-1] = '\0';
		INFO("detected fstype %s for %s", type, bdev->src);
		return ret;
	}

	if (unshare(CLONE_NEWNS) < 0)
		exit(1);

	ret = mount_unknow_fs(bdev->src, bdev->dest, 0);
	if (ret < 0) {
		ERROR("failed mounting %s onto %s to detect fstype", bdev->src, bdev->dest);
		exit(1);
	}
	// if symlink, get the real dev name
	char devpath[MAXPATHLEN];
	char *l = linkderef(bdev->src, devpath);
	if (!l)
		exit(1);
	f = fopen("/proc/self/mounts", "r");
	if (!f)
		exit(1);
	while (getline(&line, &linelen, f) != -1) {
		sp1 = index(line, ' ');
		if (!sp1)
			exit(1);
		*sp1 = '\0';
		if (strcmp(line, l))
			continue;
		sp2 = index(sp1+1, ' ');
		if (!sp2)
			exit(1);
		*sp2 = '\0';
		sp3 = index(sp2+1, ' ');
		if (!sp3)
			exit(1);
		*sp3 = '\0';
		sp2++;
		if (write(p[1], sp2, strlen(sp2)) != strlen(sp2))
			exit(1);
		exit(0);
	}
	exit(1);
}

struct bdev_type {
	char *name;
	struct bdev_ops *ops;
};

static int is_dir(const char *path)
{
	struct stat statbuf;
	int ret = stat(path, &statbuf);
	if (ret == 0 && S_ISDIR(statbuf.st_mode))
		return 1;
	return 0;
}

static int dir_detect(const char *path)
{
	if (strncmp(path, "dir:", 4) == 0)
		return 1; // take their word for it
	if (is_dir(path))
		return 1;
	return 0;
}

//
// XXXXXXX plain directory bind mount ops
//
int dir_mount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "dir"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return mount(bdev->src, bdev->dest, "bind", MS_BIND | MS_REC, NULL);
}

int dir_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "dir"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

/* the bulk of this needs to become a common helper */
static char *dir_new_path(char *src, const char *oldname, const char *name,
			const char *oldpath, const char *lxcpath)
{
	char *ret, *p, *p2;
	int l1, l2, nlen;

	nlen = strlen(src) + 1;
	l1 = strlen(oldpath);
	p = src;
	/* if src starts with oldpath, look for oldname only after
	 * that path */
	if (strncmp(src, oldpath, l1) == 0) {
		p += l1;
		nlen += (strlen(lxcpath) - l1);
	}
	l2 = strlen(oldname);
	while ((p = strstr(p, oldname)) != NULL) {
		p += l2;
		nlen += strlen(name) - l2;
	}

	ret = malloc(nlen);
	if (!ret)
		return NULL;

	p = ret;
	if (strncmp(src, oldpath, l1) == 0) {
		p += sprintf(p, "%s", lxcpath);
		src += l1;
	}

	while ((p2 = strstr(src, oldname)) != NULL) {
		strncpy(p, src, p2-src); // copy text up to oldname
		p += p2-src; // move target pointer (p)
		p += sprintf(p, "%s", name); // print new name in place of oldname
		src = p2 + l2;  // move src to end of oldname
	}
	sprintf(p, "%s", src);  // copy the rest of src
	return ret;
}

/*
 * for a simple directory bind mount, we substitute the old container
 * name and paths for the new
 */
static int dir_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		unsigned long newsize)
{
	int len, ret;

	if (snap) {
		ERROR("directories cannot be snapshotted.  Try overlayfs.");
		return -1;
	}

	if (!orig->dest || !orig->src)
		return -1;

	len = strlen(lxcpath) + strlen(cname) + strlen("rootfs") + 3;
	new->src = malloc(len);
	if (!new->src)
		return -1;
	ret = snprintf(new->src, len, "%s/%s/rootfs", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;
	if ((new->dest = strdup(new->src)) == NULL)
		return -1;

	return 0;
}

struct bdev_ops dir_ops = {
	.detect = &dir_detect,
	.mount = &dir_mount,
	.umount = &dir_umount,
	.clone_paths = &dir_clonepaths,
};


//
// XXXXXXX zfs ops
// There are two ways we could do this.  We could always specify the
// 'zfs device' (i.e. tank/lxc lxc/container) as rootfs.  But instead
// (at least right now) we have lxc-create specify $lxcpath/$lxcname/rootfs
// as the mountpoint, so that it is always mounted.
//
// That means 'mount' is really never needed and could be noop, but for the
// sake of flexibility let's always bind-mount.
//

static int zfs_list_entry(const char *path, char *output)
{
	FILE *f;
	int found=0;

	if ((f = popen("zfs list", "r")) == NULL) {
		SYSERROR("popen failed");
		return 0;
	}
	while (fgets(output, LXC_LOG_BUFFER_SIZE, f)) {
		if (strstr(output, path)) {
			found = 1;
			break;
		}
	}
	(void) pclose(f);

	return found;
}

static int zfs_detect(const char *path)
{
	char *output = malloc(LXC_LOG_BUFFER_SIZE);
	int found;

	if (!output) {
		ERROR("out of memory");
		return 0;
	}
	found = zfs_list_entry(path, output);
	free(output);
	return found;
}

int zfs_mount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "zfs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return mount(bdev->src, bdev->dest, "bind", MS_BIND | MS_REC, NULL);
}

int zfs_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "zfs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

static int zfs_clone(const char *opath, const char *npath, const char *oname,
			const char *nname, const char *lxcpath, int snapshot)
{
	// use the 'zfs list | grep opath' entry to get the zfsroot
	char output[MAXPATHLEN], option[MAXPATHLEN], *p;
	int ret;
	pid_t pid;

	if (!zfs_list_entry(opath, output))
		// default is tank.  I'd prefer lxc, but apparently this is
		// tradition.
		sprintf(output, "tank");

	if ((p = index(output, ' ')) == NULL)
		return -1;
	*p = '\0';
	if ((p = rindex(output, '/')) == NULL)
		return -1;
	*p = '\0';

	ret = snprintf(option, MAXPATHLEN, "-omountpoint=%s/%s/rootfs",
		lxcpath, nname);
	if (ret < 0  || ret >= MAXPATHLEN)
		return -1;

	// zfsroot is output up to ' '
	// zfs create -omountpoint=$lxcpath/$lxcname $zfsroot/$nname
	if (!snapshot) {
		if ((pid = fork()) < 0)
			return -1;
		if (!pid) {
			char dev[MAXPATHLEN];
			ret = snprintf(dev, MAXPATHLEN, "%s/%s", output, nname);
			if (ret < 0  || ret >= MAXPATHLEN)
				exit(1);
			execlp("zfs", "zfs", "create", option, dev, NULL);
			exit(1);
		}
		return wait_for_pid(pid);
	} else {
		// if snapshot, do
		// 'zfs snapshot zfsroot/oname@nname
		// zfs clone zfsroot/oname@nname zfsroot/nname
		char path1[MAXPATHLEN], path2[MAXPATHLEN];

		ret = snprintf(path1, MAXPATHLEN, "%s/%s@%s", output,
			oname, nname);
		if (ret < 0 || ret >= MAXPATHLEN)
			return -1;
		(void) snprintf(path2, MAXPATHLEN, "%s/%s", output, nname);

		// if the snapshot exists, delete it
		if ((pid = fork()) < 0)
			return -1;
		if (!pid) {
			execlp("zfs", "zfs", "destroy", path1, NULL);
			exit(1);
		}
		// it probably doesn't exist so destroy probably will fail.
		(void) wait_for_pid(pid);

		// run first (snapshot) command
		if ((pid = fork()) < 0)
			return -1;
		if (!pid) {
			execlp("zfs", "zfs", "snapshot", path1, NULL);
			exit(1);
		}
		if (wait_for_pid(pid) < 0)
			return -1;

		// run second (clone) command
		if ((pid = fork()) < 0)
			return -1;
		if (!pid) {
			execlp("zfs", "zfs", "clone", option, path1, path2, NULL);
			exit(1);
		}
		return wait_for_pid(pid);
	}
}

static int zfs_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		unsigned long newsize)
{
	int len, ret;

	if (!orig->src || !orig->dest)
		return -1;

	if (snap && strcmp(orig->type, "zfs")) {
		ERROR("zfs snapshot from %s backing store is not supported",
			orig->type);
		return -1;
	}

	len = strlen(lxcpath) + strlen(cname) + strlen("rootfs") + 3;
	new->src = malloc(len);
	if (!new->src)
		return -1;
	ret = snprintf(new->src, len, "%s/%s/rootfs", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;
	if ((new->dest = strdup(new->src)) == NULL)
		return -1;

	return zfs_clone(orig->src, new->src, oldname, cname, lxcpath, snap);
}

struct bdev_ops zfs_ops = {
	.detect = &zfs_detect,
	.mount = &zfs_mount,
	.umount = &zfs_umount,
	.clone_paths = &zfs_clonepaths,
};

//
// LVM ops
//

/*
 * Look at /sys/dev/block/maj:min/dm/uuid.  If it contains the hardcoded LVM
 * prefix "LVM-", then this is an lvm2 LV
 */
static int lvm_detect(const char *path)
{
	char devp[MAXPATHLEN], buf[4];
	FILE *fout;
	int ret;
	struct stat statbuf;

	if (strncmp(path, "lvm:", 4) == 0)
		return 1; // take their word for it

	ret = stat(path, &statbuf);
	if (ret != 0)
		return 0;
	if (!S_ISBLK(statbuf.st_mode))
		return 0;

	ret = snprintf(devp, MAXPATHLEN, "/sys/dev/block/%d:%d/dm/uuid",
			major(statbuf.st_rdev), minor(statbuf.st_rdev));
	if (ret < 0 || ret >= MAXPATHLEN) {
		ERROR("lvm uuid pathname too long");
		return 0;
	}
	fout = fopen(devp, "r");
	if (!fout)
		return 0;
	ret = fread(buf, 1, 4, fout);
	fclose(fout);
	if (ret != 4 || strncmp(buf, "LVM-", 4) != 0)
		return 0;
	return 1;
}

static int lvm_mount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "lvm"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	/* if we might pass in data sometime, then we'll have to enrich
	 * mount_unknow_fs */
	return mount_unknow_fs(bdev->src, bdev->dest, 0);
}

static int lvm_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "lvm"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

/*
 * path must be '/dev/$vg/$lv', $vg must be an existing VG, and $lv must
 * not yet exist.  This function will attempt to create /dev/$vg/$lv of
 * size $size.
 */
static int lvm_create(const char *path, unsigned long size)
{
	int ret, pid;
	char sz[24], *pathdup, *vg, *lv;

	if ((pid = fork()) < 0) {
		SYSERROR("failed fork");
		return -1;
	}
	if (pid > 0)
		return wait_for_pid(pid);

	// lvcreate default size is in M, not bytes.
	ret = snprintf(sz, 24, "%lu", size/1000000);
	if (ret < 0 || ret >= 24)
		exit(1);

	pathdup = strdup(path);
	if (!pathdup)
		exit(1);
	lv = rindex(pathdup, '/');
	if (!lv) {
		free(pathdup);
		exit(1);
	}
	*lv = '\0';
	lv++;
	vg = rindex(pathdup, '/');
	if (!vg)
		exit(1);
	vg++;
	execlp("lvcreate", "lvcreate", "-L", sz, vg, "-n", lv, (char *)NULL);
	free(pathdup);
	exit(1);
}

static int lvm_snapshot(const char *orig, const char *path, unsigned long size)
{
	int ret, pid;
	char sz[24], *pathdup, *lv;

	if ((pid = fork()) < 0) {
		SYSERROR("failed fork");
		return -1;
	}
	if (pid > 0)
		return wait_for_pid(pid);
	// lvcreate default size is in M, not bytes.
	ret = snprintf(sz, 24, "%lu", size/1000000);
	if (ret < 0 || ret >= 24)
		exit(1);

	pathdup = strdup(path);
	if (!pathdup)
		exit(1);
	lv = rindex(pathdup, '/');
	if (!lv) {
		free(pathdup);
		exit(1);
	}
	*lv = '\0';
	lv++;

	ret = execlp("lvcreate", "lvcreate", "-s", "-L", sz, "-n", lv, orig, (char *)NULL);
	free(pathdup);
	exit(1);
}

// this will return 1 for physical disks, qemu-nbd, loop, etc
// right now only lvm is a block device
static int is_blktype(struct bdev *b)
{
	if (strcmp(b->type, "lvm") == 0)
		return 1;
	return 0;
}

static int lvm_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		unsigned long newsize)
{
	char fstype[100];
	unsigned long size = newsize;
	int len, ret;

	if (!orig->src || !orig->dest)
		return -1;

	if (strcmp(orig->type, "lvm")) {
		if (snap) {
			ERROR("LVM snapshot from %s backing store is not supported",
				orig->type);
			return -1;
		}
		// Use VG 'lxc' by default
		// We will want to support custom VGs, at least as specified through
		// /etc/lxc/lxc.conf, preferably also over cmdline
		len = strlen("/dev/lxc/") + strlen(cname) + 1;
		if ((new->src = malloc(len)) == NULL)
			return -1;
		ret = snprintf(new->src, len, "/dev/lxc/%s", cname);
		if (ret < 0 || ret >= len)
			return -1;
	} else {
		new->src = dir_new_path(orig->src, oldname, cname, oldpath, lxcpath);
		if (!new->src)
			return -1;
	}

	if (orig->data) {
		new->data = strdup(orig->data);
		if (!new->data)
			return -1;
	}

	len = strlen(lxcpath) + strlen(cname) + strlen("rootfs") + 3;
	new->dest = malloc(len);
	if (!new->dest)
		return -1;
	ret = snprintf(new->dest, len, "%s/%s/rootfs", lxcpath, cname);
	if (ret < 0 || ret >= len)
		return -1;
	if (mkdir_p(new->dest, 0755) < 0)
		return -1;

	if (is_blktype(orig)) {
		if (!newsize && blk_getsize(orig->src, &size) < 0) {
			ERROR("Error getting size of %s", orig->src);
			return -1;
		}
		if (detect_fs(orig, fstype, 100) < 0) {
			INFO("could not find fstype for %s, using ext3", orig->src);
			return -1;
		}
	} else {
		sprintf(fstype, "ext3");
		if (!newsize)
			size = 1000000000; // default to 1G
	}

	if (snap) {
		if (lvm_snapshot(orig->src, new->src, size) < 0) {
			ERROR("could not create %s snapshot of %s", new->src, orig->src);
			return -1;
		}
	} else {
		if (lvm_create(new->src, size) < 0) {
			ERROR("Error creating new lvm blockdev");
			return -1;
		}
		if (do_mkfs(new->src, fstype) < 0) {
			ERROR("Error creating filesystem type %s on %s", fstype,
				new->src);
			return -1;
		}
	}

	return 0;
}

struct bdev_ops lvm_ops = {
	.detect = &lvm_detect,
	.mount = &lvm_mount,
	.umount = &lvm_umount,
	.clone_paths = &lvm_clonepaths,
};

//
// btrfs ops
//

struct btrfs_ioctl_space_info {
	unsigned long long flags;
	unsigned long long total_bytes;
	unsigned long long used_bytes;
};

struct btrfs_ioctl_space_args {
	unsigned long long space_slots;
	unsigned long long total_spaces;
	struct btrfs_ioctl_space_info spaces[0];
};

#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_IOC_SUBVOL_GETFLAGS _IOR(BTRFS_IOCTL_MAGIC, 25, unsigned long long)
#define BTRFS_IOC_SPACE_INFO _IOWR(BTRFS_IOCTL_MAGIC, 20, \
                                    struct btrfs_ioctl_space_args)

static int btrfs_detect(const char *path)
{
	struct stat st;
	int fd, ret;
	struct btrfs_ioctl_space_args sargs;

	// make sure this is a btrfs filesystem
	fd = open(path, O_RDONLY);
	if (fd < 0)
		return 0;
	sargs.space_slots = 0;
	sargs.total_spaces = 0;
	ret = ioctl(fd, BTRFS_IOC_SPACE_INFO, &sargs);
	close(fd);
	if (ret < 0)
		return 0;

	// and make sure it's a subvolume.
	ret = stat(path, &st);
	if (ret < 0)
		return 0;

	if (st.st_ino == 256 && S_ISDIR(st.st_mode))
		return 1;

	return 0;
}

int btrfs_mount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "btrfs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return mount(bdev->src, bdev->dest, "bind", MS_BIND | MS_REC, NULL);
}

int btrfs_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "btrfs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

#define BTRFS_SUBVOL_NAME_MAX 4039
#define BTRFS_PATH_NAME_MAX 4087

struct btrfs_ioctl_vol_args {
	signed long long fd;
	char name[BTRFS_PATH_NAME_MAX + 1];
};

#define BTRFS_IOCTL_MAGIC 0x94
#define BTRFS_IOC_SUBVOL_CREATE_V2 _IOW(BTRFS_IOCTL_MAGIC, 24, \
                                   struct btrfs_ioctl_vol_args_v2)
#define BTRFS_IOC_SNAP_CREATE_V2 _IOW(BTRFS_IOCTL_MAGIC, 23, \
                                   struct btrfs_ioctl_vol_args_v2)
#define BTRFS_IOC_SUBVOL_CREATE _IOW(BTRFS_IOCTL_MAGIC, 14, \
                                   struct btrfs_ioctl_vol_args)

#define BTRFS_QGROUP_INHERIT_SET_LIMITS (1ULL << 0)

struct btrfs_ioctl_vol_args_v2 {
	signed long long fd;
	unsigned long long transid;
	unsigned long long flags;
	union {
		struct {
			unsigned long long size;
			//struct btrfs_qgroup_inherit *qgroup_inherit;
			void *qgroup_inherit;
		};
		unsigned long long unused[4];
	};
	char name[BTRFS_SUBVOL_NAME_MAX + 1];
};

static int btrfs_subvolume_create(const char *path)
{
	int ret, fd = -1;
	struct btrfs_ioctl_vol_args  args;
	char *p, *newfull = strdup(path);

	if (!newfull) {
		ERROR("Error: out of memory");
		return -1;
	}

	p = rindex(newfull, '/');
	if (!p) {
		ERROR("bad path: %s", path);
		return -1;
	}
	*p = '\0';

	if ((fd = open(newfull, O_RDONLY)) < 0) {
		ERROR("Error opening %s", newfull);
		free(newfull);
		return -1;
	}

	memset(&args, 0, sizeof(args));
	strncpy(args.name, p+1, BTRFS_SUBVOL_NAME_MAX);
	args.name[BTRFS_SUBVOL_NAME_MAX-1] = 0;
	ret = ioctl(fd, BTRFS_IOC_SUBVOL_CREATE, &args);
	INFO("btrfs: snapshot create ioctl returned %d", ret);
{
	FILE *f = fopen("/tmp/a", "a");
	fprintf(f, "ioctl returned %d\n", ret);
	fclose(f);
}

	free(newfull);
	close(fd);
	return ret;
}

static int btrfs_snapshot(const char *orig, const char *new)
{
	int fd = -1, fddst = -1, ret = -1;
	struct btrfs_ioctl_vol_args_v2  args;
	char *newdir, *newname, *newfull = NULL;

	newfull = strdup(new);
	if (!newfull) {
		ERROR("Error: out of memory");
		goto out;
	}
	// make sure the directory doesn't already exist
	if (rmdir(newfull) < 0 && errno != -ENOENT) {
		SYSERROR("Error removing empty new rootfs");
		goto out;
	}
	newname = basename(newfull);
	newdir = dirname(newfull);
	fd = open(orig, O_RDONLY);
	if (fd < 0) {
		SYSERROR("Error opening original rootfs %s", orig);
		goto out;
	}
	fddst = open(newdir, O_RDONLY);
	if (fddst < 0) {
		SYSERROR("Error opening new container dir %s", newdir);
		goto out;
	}

	memset(&args, 0, sizeof(args));
	args.fd = fd;
	strncpy(args.name, newname, BTRFS_SUBVOL_NAME_MAX);
	args.name[BTRFS_SUBVOL_NAME_MAX-1] = 0;
	ret = ioctl(fddst, BTRFS_IOC_SNAP_CREATE_V2, &args);
	INFO("btrfs: snapshot create ioctl returned %d", ret);

out:
	if (fddst != -1)
		close(fddst);
	if (fd != -1)
		close(fd);
	if (newfull)
		free(newfull);
	return ret;
}

static int btrfs_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		unsigned long newsize)
{
	if (!orig->dest || !orig->src)
		return -1;

	if (strcmp(orig->type, "btrfs")) {
		int len, ret;
		if (snap) {
			ERROR("btrfs snapshot from %s backing store is not supported",
				orig->type);
			return -1;
		}
		len = strlen(lxcpath) + strlen(cname) + strlen("rootfs") + 3;
		new->src = malloc(len);
		if (!new->src)
			return -1;
		ret = snprintf(new->src, len, "%s/%s/rootfs", lxcpath, cname);
		if (ret < 0 || ret >= len)
			return -1;
	} else {
		// in case rootfs is in custom path, reuse it
		if ((new->src = dir_new_path(orig->src, oldname, cname, oldpath, lxcpath)) == NULL)
			return -1;

	}

	if ((new->dest = strdup(new->src)) == NULL)
		return -1;

	if (orig->data && (new->data = strdup(orig->data)) == NULL)
		return -1;

	if (snap)
		return btrfs_snapshot(orig->dest, new->dest);

	if (rmdir(new->dest) < 0 && errno != -ENOENT) {
		SYSERROR("removing %s\n", new->dest);
		return -1;
	}

	return btrfs_subvolume_create(new->dest);
}

struct bdev_ops btrfs_ops = {
	.detect = &btrfs_detect,
	.mount = &btrfs_mount,
	.umount = &btrfs_umount,
	.clone_paths = &btrfs_clonepaths,
};

//
// overlayfs ops
//

static int overlayfs_detect(const char *path)
{
	if (strncmp(path, "overlayfs:", 10) == 0)
		return 1; // take their word for it
	return 0;
}

//
// XXXXXXX plain directory bind mount ops
//
int overlayfs_mount(struct bdev *bdev)
{
	char *options, *dup, *lower, *upper;
	int len;
	int ret;

	if (strcmp(bdev->type, "overlayfs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;

	//  separately mount it first
	//  mount -t overlayfs -oupperdir=${upper},lowerdir=${lower} lower dest
	dup = strdupa(bdev->src);
	if (!(lower = index(dup, ':')))
		return -22;
	if (!(upper = index(++lower, ':')))
		return -22;
	*upper = '\0';
	upper++;

	// TODO We should check whether bdev->src is a blockdev, and if so
	// but for now, only support overlays of a basic directory

	len = strlen(lower) + strlen(upper) + strlen("upperdir=,lowerdir=") + 1;
	options = alloca(len);
	ret = snprintf(options, len, "upperdir=%s,lowerdir=%s", upper, lower);
	if (ret < 0 || ret >= len)
		return -1;
	ret = mount(lower, bdev->dest, "overlayfs", MS_MGC_VAL, options);
	if (ret < 0)
		SYSERROR("overlayfs: error mounting %s onto %s options %s",
			lower, bdev->dest, options);
	else
		INFO("overlayfs: mounted %s onto %s options %s",
			lower, bdev->dest, options);
	return ret;
}

int overlayfs_umount(struct bdev *bdev)
{
	if (strcmp(bdev->type, "overlayfs"))
		return -22;
	if (!bdev->src || !bdev->dest)
		return -22;
	return umount(bdev->dest);
}

static int overlayfs_clonepaths(struct bdev *orig, struct bdev *new, const char *oldname,
		const char *cname, const char *oldpath, const char *lxcpath, int snap,
		unsigned long newsize)
{
	if (!snap) {
		ERROR("overlayfs is only for snapshot clones");
		return -22;
	}

	if (!orig->src || !orig->dest)
		return -1;

	new->dest = dir_new_path(orig->dest, oldname, cname, oldpath, lxcpath);
	if (!new->dest)
		return -1;
	if (mkdir_p(new->dest, 0755) < 0)
		return -1;

	if (strcmp(orig->type, "dir") == 0) {
		char *delta;
		int ret, len;
		if (!snap)
			return -1;
		// if we have /var/lib/lxc/c2/rootfs, then delta will be
		//            /var/lib/lxc/c2/delta0
		delta = strdup(new->dest);
		if (!delta) {
			return -1;
		}
		if (strlen(delta) < 6) {
			free(delta);
			return -22;
		}
		strcpy(&delta[strlen(delta)-6], "delta0");
		if ((ret = mkdir(delta, 0755)) < 0) {
			SYSERROR("error: mkdir %s", delta);
			free(delta);
			return -1;
		}

		// the src will be 'overlayfs:lowerdir:upperdir'
		len = strlen(delta) + strlen(orig->src) + 12;
		new->src = malloc(len);
		if (!new->src) {
			free(delta);
			return -ENOMEM;
		}
		ret = snprintf(new->src, len, "overlayfs:%s:%s", orig->src, delta);
		free(delta);
		if (ret < 0 || ret >= len)
			return -ENOMEM;
	} else if (strcmp(orig->type, "lvm") == 0) {
		ERROR("overlayfs clone of lvm container is not yet supported");
		// Note, supporting this will require overlayfs_mount supporting
		// mounting of the underlay.  No big deal, just needs to be done.
		return -1;
	} else if (strcmp(orig->type, "overlayfs") == 0) {
		// What exactly do we want to do here?
		// I think we want to use the original lowerdir, with a
		// private delta which is originally rsynced from the
		// original delta
		char *osrc, *odelta, *nsrc, *ndelta;
		int len, ret;
		if (!(osrc = strdup(orig->src)))
			return -22;
		nsrc = index(osrc, ':') + 1;
		if (nsrc != osrc + 10 || (odelta = index(nsrc, ':')) == NULL) {
			free(osrc);
			return -22;
		}
		*odelta = '\0';
		odelta++;
		ndelta = dir_new_path(odelta, oldname, cname, oldpath, lxcpath);
		if (!ndelta) {
			free(osrc);
			return -ENOMEM;
		}
		if (do_rsync(odelta, ndelta) < 0) {
			ERROR("copying overlayfs delta");
			return -1;
		}
		len = strlen(nsrc) + strlen(ndelta) + 12;
		new->src = malloc(len);
		if (!new->src) {
			free(osrc);
			free(ndelta);
			return -ENOMEM;
		}
		ret = snprintf(new->src, len, "overlayfs:%s:%s", nsrc, ndelta);
		free(osrc);
		free(ndelta);
		if (ret < 0 || ret >= len)
			return -ENOMEM;
	}

	return 0;
}

struct bdev_ops overlayfs_ops = {
	.detect = &overlayfs_detect,
	.mount = &overlayfs_mount,
	.umount = &overlayfs_umount,
	.clone_paths = &overlayfs_clonepaths,
};

struct bdev_type bdevs[] = {
	{.name = "zfs", .ops = &zfs_ops,},
	{.name = "lvm", .ops = &lvm_ops,},
	{.name = "btrfs", .ops = &btrfs_ops,},
	{.name = "dir", .ops = &dir_ops,},
	{.name = "overlayfs", .ops = &overlayfs_ops,},
};

static const size_t numbdevs = sizeof(bdevs) / sizeof(struct bdev_type);

void bdev_put(struct bdev *bdev)
{
	if (bdev->data)
		free(bdev->data);
	if (bdev->src)
		free(bdev->src);
	if (bdev->dest)
		free(bdev->dest);
	free(bdev);
}

struct bdev *bdev_get(const char *type)
{
	int i;
	struct bdev *bdev;

	for (i=0; i<numbdevs; i++) {
		if (strcmp(bdevs[i].name, type) == 0)
			break;
	}
	if (i == numbdevs)
		return NULL;
	bdev = malloc(sizeof(struct bdev));
	if (!bdev)
		return NULL;
	memset(bdev, 0, sizeof(struct bdev));
	bdev->ops = bdevs[i].ops;
	bdev->type = bdevs[i].name;
	return bdev;
}

struct bdev *bdev_init(const char *src, const char *dst, const char *data)
{
	int i;
	struct bdev *bdev;

	for (i=0; i<numbdevs; i++) {
		int r;
		r = bdevs[i].ops->detect(src);
		if (r)
			break;
	}
	if (i == numbdevs)
		return NULL;
	bdev = malloc(sizeof(struct bdev));
	if (!bdev)
		return NULL;
	memset(bdev, 0, sizeof(struct bdev));
	bdev->ops = bdevs[i].ops;
	bdev->type = bdevs[i].name;
	if (data)
		bdev->data = strdup(data);
	if (src)
		bdev->src = strdup(src);
	if (dst)
		bdev->dest = strdup(dst);

	return bdev;
}

/*
 * If we're not snaphotting, then bdev_copy becomes a simple case of mount
 * the original, mount the new, and rsync the contents.
 */
struct bdev *bdev_copy(const char *src, const char *oldname, const char *cname,
			const char *oldpath, const char *lxcpath, const char *bdevtype,
			int snap, const char *bdevdata, unsigned long newsize)
{
	struct bdev *orig, *new;
	pid_t pid;

	/* if the container name doesn't show up in the rootfs path, then
	 * we don't know how to come up with a new name
	 */
	if (strstr(src, oldname) == NULL) {
		ERROR("original rootfs path %s doesn't include container name %s",
			src, oldname);
		return NULL;
	}

	orig = bdev_init(src, NULL, NULL);
	if (!orig) {
		ERROR("failed to detect blockdev type for %s\n", src);
		return NULL;
	}

	if (!orig->dest) {
		int ret;
		orig->dest = malloc(MAXPATHLEN);
		if (!orig->dest) {
			ERROR("out of memory");
			bdev_put(orig);
			return NULL;
		}
		ret = snprintf(orig->dest, MAXPATHLEN, "%s/%s/rootfs", oldpath, oldname);
		if (ret < 0 || ret >= MAXPATHLEN) {
			ERROR("rootfs path too long");
			bdev_put(orig);
			return NULL;
		}
	}

	new = bdev_get(bdevtype ? bdevtype : orig->type);
	if (!new) {
		ERROR("no such block device type: %s", bdevtype ? bdevtype : orig->type);
		bdev_put(orig);
		return NULL;
	}

	if (new->ops->clone_paths(orig, new, oldname, cname, oldpath, lxcpath, snap, newsize) < 0) {
		ERROR("failed getting pathnames for cloned storage: %s\n", src);
		bdev_put(orig);
		bdev_put(new);
		return NULL;
	}

	pid = fork();
	if (pid < 0) {
		SYSERROR("fork");
		bdev_put(orig);
		bdev_put(new);
		return NULL;
	}

	if (pid > 0) {
		int ret = wait_for_pid(pid);
		bdev_put(orig);
		if (ret < 0) {
			bdev_put(new);
			return NULL;
		}
		return new;
	}

	if (unshare(CLONE_NEWNS) < 0) {
		SYSERROR("unshare CLONE_NEWNS");
		exit(1);
	}
	if (snap)
		exit(0);

	// If not a snapshot, copy the fs.
	if (orig->ops->mount(orig) < 0) {
		ERROR("failed mounting %s onto %s\n", src, orig->dest);
		exit(1);
	}
	if (new->ops->mount(new) < 0) {
		ERROR("failed mounting %s onto %s\n", new->src, new->dest);
		exit(1);
	}
	if (do_rsync(orig->dest, new->dest) < 0) {
		ERROR("rsyncing %s to %s\n", orig->src, new->src);
		exit(1);
	}
	// don't bother umounting, ns exit will do that

	exit(0);
}
