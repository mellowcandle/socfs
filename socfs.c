/*
  FUSE: Filesystem in Userspace
  Copyright (C) 2020 Ramon Fried <rfried.dev@gmail.com>
  This program can be distributed under the terms of the GNU GPL.
  See the file COPYING.
*/

#include "config.h"

#ifdef HAVE_FUSE3
#define FUSE_USE_VERSION 31
#else
#define FUSE_USE_VERSION 29
#endif

#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include "misc.h"

#define MAX_SOC_NAME 32
#define MAX_REG_NAME 64
#define MAX_TOP_NAME 32

#define SOC_MAGIC 0x57a32bcd

struct reg {
	char name[MAX_REG_NAME];
	uint64_t addr;
	uint32_t width;
} __attribute__((packed));

struct top {
	char name[MAX_TOP_NAME];
	uint32_t reg_count;
	uint32_t next_offset;
	struct reg regs[];
} __attribute__((packed));

struct soc_header {
	uint32_t magic;
	uint32_t version;
	char soc_name[MAX_SOC_NAME];
	uint32_t top_count;
	struct top tops[];
} __attribute__((packed));

struct soc_private {
	struct soc_header *header;
	int mem_fd;
};

struct mem_map {
	void *virt_addr;
	void *map_base;
	uint32_t offset_in_page;
	uint32_t mapped_size;
};
/*
 * Command line options
 *
 * We can't set default values for the char* fields here because
 * fuse_opt_parse would attempt to free() them when the user specifies
 * different values on the command line.
 */
static struct options {
	const char *filename;
	int show_help;
} options;

#define OPTION(t, p)                           \
	{ t, offsetof(struct options, p), 1 }
static const struct fuse_opt option_spec[] = {
	OPTION("--soc_file=%s", filename),
	OPTION("-h", show_help),
	OPTION("--help", show_help),
	FUSE_OPT_END
};

#ifdef HAVE_FUSE2
/* fuse_log is not available under FUSE3 */
#define fuse_log(a,b,...) fprintf(stderr, b, ##__VA_ARGS__)
#define filler(a, b, c, d, e) filler(a, b, c, d)
#endif

#ifdef HAVE_FUSE2
static int soc_getattr(const char *path, struct stat *stbuf)
#else
static int soc_getattr(const char *path, struct stat *stbuf,
		       struct fuse_file_info *fi)
#endif
{
	int res = 0;

	fuse_log(FUSE_LOG_DEBUG, "%s: %s\n", __func__, path);

	memset(stbuf, 0, sizeof(struct stat));
	if ((strcmp(path, "/") == 0) || (!strchr(path + 1, '/'))) {
		stbuf->st_mode = S_IFDIR | 0755;
		stbuf->st_nlink = 2;
	} else {
		stbuf->st_mode = S_IFREG | 0666;
		stbuf->st_nlink = 1;
		stbuf->st_size = 256; //todo: fix this.
	}

	return res;
}

static struct top *find_top(struct soc_private *private, const char *name,
                            size_t len)
{
	int i;
	struct top *top;

	fuse_log(FUSE_LOG_DEBUG, "%s: %s len: %u", __func__, name, len);

	top = private->header->tops;
	for (i = 0; i < private->header->top_count; i++) {
		if (!strncmp(name, top->name, len)) {
			fuse_log(FUSE_LOG_DEBUG, "Found top: %s\n", top->name);
			return top;
		}
		top = (struct top *)((char *)private->header +
		                     top->next_offset);
	}

	return NULL;
}

static struct reg *find_reg(struct soc_private *private, const char *name)
{
	int i;
	char *idx;
	ptrdiff_t len;
	struct top *top;

	idx = index(name + 1, '/') - 1;
	len = idx - (name + 1);

	top = find_top(private, name + 1, len);
	if (!top)
		return NULL;

	fuse_log(FUSE_LOG_DEBUG, "Found top: %s\n", top->name);

	for (i = 0; i < top->reg_count; i++)
		if (!strcmp(idx + 2, top->regs[i].name)) {
			fuse_log(FUSE_LOG_DEBUG, "Found reg: %s\n",
				 top->regs[i].name);
			return &top->regs[i];
		}
	return NULL;
}

#ifdef HAVE_FUSE2
static int soc_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi)
#else
static int soc_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                       off_t offset, struct fuse_file_info *fi,
		       enum fuse_readdir_flags flags)
#endif
{
	(void) offset;
	(void) fi;
	int i;
	struct soc_private *private = fuse_get_context()->private_data;
	struct top *top;

	fuse_log(FUSE_LOG_DEBUG, "%s: %s\n", __func__, path);

	if (!strcmp(path, "/")) {

		filler(buf, ".", NULL, 0, 0);
		filler(buf, "..", NULL, 0, 0);

		top = private->header->tops;
		for (i = 0; i < private->header->top_count; i++) {
			filler(buf, top->name, NULL, 0, 0);
			top = (struct top *)((char *)private->header +
			                     top->next_offset);
		}
		return 0;
	} else if (!strchr(path + 1, '/')) {
		top = find_top(private, path + 1, strlen(path + 1));
		if (!top) {
			fuse_log(FUSE_LOG_ERR, "Couldn't find the file %s\n",
			         path);
			return -ENOENT;
		}

		filler(buf, ".", NULL, 0, 0);
		filler(buf, "..", NULL, 0, 0);

		for (i = 0; i < top->reg_count; i++)
			filler(buf, top->regs[i].name, NULL, 0, 0);

		return 0;
	}

	return -ENOENT;
}

static int map_mem(struct soc_private *private, struct mem_map *map,
                   off_t target, size_t width)
{
	unsigned int page_size;

	map->mapped_size = page_size = getpagesize();
	map->offset_in_page = (unsigned)target & (page_size - 1);
	if (map->offset_in_page + width > page_size) {
		/* This access spans pages.
		 * Must map two pages to make it possible: */
		map->mapped_size *= 2;
	}
	map->map_base = mmap(NULL,
	                     map->mapped_size,
	                     PROT_READ | PROT_WRITE,
	                     MAP_SHARED,
	                     private->mem_fd,
	                     target & ~(off_t)(page_size - 1));
	if (map->map_base == MAP_FAILED) {
		fuse_log(FUSE_LOG_ERR, "Can't map devmem\n");
		return 1;
	}

	map->virt_addr = (char *)map->map_base + map->offset_in_page;

	fuse_log(FUSE_LOG_INFO, "Register mapped to: 0x%llx\n",
	         (uint64_t)map->virt_addr);

	return 0;
}

static void unmap_mem(struct mem_map *map)
{
	if (munmap(map->map_base, map->mapped_size))
		fuse_log(FUSE_LOG_ERR, "Can't unmap devmem\n");
}

static int soc_read(const char *path, char *buf, size_t size, off_t offset,
                    struct fuse_file_info *fi)
{
	struct soc_private *private = fuse_get_context()->private_data;
	struct reg *reg;
	struct mem_map map;
	uint64_t result;

	fuse_log(FUSE_LOG_DEBUG, "%s: path: %s size: %u offset: %u\n", __func__,
		 path, size, offset);

	reg = find_reg(private, path);

	if (!reg)
		return -ENOENT;

	if (map_mem(private, &map, reg->addr, 4))
		return -EFAULT;

	switch (reg->width) {
	case 8:
		result = *(volatile uint8_t *)map.virt_addr;
		break;
	case 16:
		result = *(volatile uint16_t *)map.virt_addr;
		break;
	case 32:
		result = *(volatile uint32_t *)map.virt_addr;
		break;
	case 64:
		result = *(volatile uint64_t *)map.virt_addr;
		break;
	default:
		fprintf(stderr, "Reg width is wrong: %d\n", reg->width);
		return -EFAULT;
	}

	unmap_mem(&map);

	return sprintf(buf, "0x%llx -> 0x%llx\n", reg->addr, result);
}

static int soc_write(const char *path, const char *buf, size_t size,
		     off_t offset, struct fuse_file_info *fi)
{
	struct soc_private *private = fuse_get_context()->private_data;
	struct reg *reg;
	uint64_t writeval;
	struct mem_map map;

	fuse_log(FUSE_LOG_DEBUG, "%s: %s\n", __func__, path);

	reg = find_reg(private, path);

	if (!reg)
		return -ENOENT;

	if (parse_input(buf, &writeval)) {
		fuse_log(FUSE_LOG_ERR, "Can't parse write value\n");
		return -EINVAL;
	}

	if (map_mem(private, &map, reg->addr, 4))
		return -EFAULT;

	fuse_log(FUSE_LOG_INFO, "Writing 0x%llx to %s at %llx\n", writeval,
		 reg->name, reg->addr);

	switch (reg->width) {
	case 8:
		*(volatile uint8_t *)map.virt_addr = writeval;
		break;
	case 16:
		*(volatile uint16_t *)map.virt_addr = writeval;
		break;
	case 32:
		*(volatile uint32_t *)map.virt_addr = writeval;
		break;
	case 64:
		*(volatile uint64_t *)map.virt_addr = writeval;
		break;
	default:
		return -EFAULT;
	}

	unmap_mem(&map);

	return size;
}

#ifdef HAVE_FUSE2
static int soc_truncate(const char *path, off_t offset)
#else
static int soc_truncate(const char *path, off_t offset,
			struct fuse_file_info *fi)
#endif
{
	fuse_log(FUSE_LOG_DEBUG, "%s: %s\n", __func__, path);
	return 0;
}

static struct fuse_operations soc_oper = {
	.getattr	= soc_getattr,
	.readdir	= soc_readdir,
	.read		= soc_read,
	.write		= soc_write,
	.truncate	= soc_truncate,
};

static void show_help(const char *progname)
{
	printf("usage: %s [options] <mountpoint>\n\n", progname);
	printf("File-system specific options:\n"
	       "    --soc_file=<s>      Name of the \"soc\" file\n"
	       "\n");
}

int main(int argc, char *argv[])
{
	int ret;
	int soc_file;
	struct stat st;
	struct soc_private *private;

	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
	/* Set defaults -- we have to use strdup so that
	   fuse_opt_parse can free the defaults if other
	   values are specified */
	/* Parse options */
	if (fuse_opt_parse(&args, &options, option_spec, NULL) == -1)
		return 1;

	/* When --help is specified, first print our own file-system
	   specific help text, then signal fuse_main to show
	   additional help (by adding `--help` to the options again)
	   without usage: line (by setting argv[0] to the empty
	   string) */
	if (options.show_help) {
		show_help(argv[0]);
		assert(fuse_opt_add_arg(&args, "--help") == 0);
		args.argv[0][0] = '\0';
		goto skip_load;
	} else if (!options.filename) {
		printf("Error: --soc_file argument is mandatory\n");
		show_help(argv[0]);
		return 1;
	}

	private = malloc(sizeof(*private));
	if (!private) {
		printf("Error: Can't allocate memory\n");
		exit(1);
	}

	soc_file = open(options.filename, O_RDONLY);
	if (!soc_file) {
		perror("Can't open soc file for reading");
		exit(1);
	}

	fstat(soc_file, &st);

	private->header = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE,
	                       soc_file, 0);
	if (!private->header) {
		perror("Can't memory map the soc file for reading");
		exit(1);
	}
	close(soc_file);

	private->mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (!private->mem_fd) {
		perror("Can't open /dev/mem\n");
		exit(1);
	}

	if (private->header->magic != SOC_MAGIC ||
	    private->header->version != 1) {
		printf("Unsupported SOC file format\n");
		exit(1);
	}

skip_load:
	ret = fuse_main(args.argc, args.argv, &soc_oper, private);
	fuse_opt_free_args(&args);

	return ret;
}
