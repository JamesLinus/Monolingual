/*
 * Copyright (c) 1999 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */
/*
 * The lipo(1) program.  This program creates, thins and operates on fat files.
 * This program takes the following options:
 *   <input_file>
 *   -output <filename>
 *   -thin <arch_type>
 *   -remove <arch_type>
 */
#include "lipo.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ar.h>
#ifndef AR_EFMT1
#define	AR_EFMT1	"#1/"		/* extended format #1 */
#endif
#include <limits.h>
#include <errno.h>
#include <ctype.h>
#include <libc.h>
#include <utime.h>
#include <sys/file.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mach/mach.h>
#include <mach-o/loader.h>
#include <mach-o/fat.h>

/* The maximum section alignment allowed to be specified, as a power of two */
#define MAXSECTALIGN		15 /* 2**15 or 0x8000 */

/*
 * The structure describing an architecture flag with the string of the flag
 * name, and the cputype and cpusubtype.
 */
struct arch_flag {
	const char *name;
	cpu_type_t cputype;
	cpu_subtype_t cpusubtype;
};

enum byte_sex {
	UNKNOWN_BYTE_SEX,
	BIG_ENDIAN_BYTE_SEX,
	LITTLE_ENDIAN_BYTE_SEX
};

#define SWAP_LONG(a) ( ((a) << 24) | \
					   (((a) << 8) & 0x00ff0000) | \
					   (((a) >> 8) & 0x0000ff00) | \
					   ((unsigned long)(a) >> 24) )

static const struct arch_flag arch_flags[] = {
	{ "any",	    CPU_TYPE_ANY,	    CPU_SUBTYPE_MULTIPLE },
	{ "little",	    CPU_TYPE_ANY,	    CPU_SUBTYPE_LITTLE_ENDIAN },
	{ "big",	    CPU_TYPE_ANY,	    CPU_SUBTYPE_BIG_ENDIAN },

	/* 64-bit Mach-O architectures */

	/* architecture families */
	{ "ppc64", 	    CPU_TYPE_POWERPC64, CPU_SUBTYPE_POWERPC_ALL },
	/* specific architecture implementations */
	{ "ppc970-64",  CPU_TYPE_POWERPC64, CPU_SUBTYPE_POWERPC_970 },

	/* 32-bit Mach-O architectures */

	/* architecture families */
	{ "ppc",	    CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_ALL },
	{ "i386",       CPU_TYPE_I386,	    CPU_SUBTYPE_I386_ALL },
	{ "m68k",       CPU_TYPE_MC680x0,   CPU_SUBTYPE_MC680x0_ALL },
	{ "hppa",       CPU_TYPE_HPPA,	    CPU_SUBTYPE_HPPA_ALL },
	{ "sparc",	    CPU_TYPE_SPARC,     CPU_SUBTYPE_SPARC_ALL },
	{ "m88k",       CPU_TYPE_MC88000,   CPU_SUBTYPE_MC88000_ALL },
	{ "i860",       CPU_TYPE_I860,	    CPU_SUBTYPE_I860_ALL },
	/* specific architecture implementations */
	{ "ppc601",     CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_601 },
	{ "ppc603",     CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_603 },
	{ "ppc603e",    CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_603e },
	{ "ppc603ev",   CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_603ev },
	{ "ppc604",     CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_604 },
	{ "ppc604e",    CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_604e },
	{ "ppc750",     CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_750 },
	{ "ppc7400",    CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_7400 },
	{ "ppc7450",    CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_7450 },
	{ "ppc970",     CPU_TYPE_POWERPC,   CPU_SUBTYPE_POWERPC_970 },
	{ "i486",       CPU_TYPE_I386,	    CPU_SUBTYPE_486 },
	{ "i486SX",     CPU_TYPE_I386,	    CPU_SUBTYPE_486SX },
	{ "pentium",    CPU_TYPE_I386,	    CPU_SUBTYPE_PENT }, /* same as i586 */
	{ "i586",       CPU_TYPE_I386,	    CPU_SUBTYPE_586 },
	{ "pentpro",    CPU_TYPE_I386,      CPU_SUBTYPE_PENTPRO }, /* same as i686 */
	{ "i686",       CPU_TYPE_I386,      CPU_SUBTYPE_PENTPRO },
	{ "pentIIm3",   CPU_TYPE_I386,      CPU_SUBTYPE_PENTII_M3 },
	{ "pentIIm5",   CPU_TYPE_I386,      CPU_SUBTYPE_PENTII_M5 },
	{ "pentium4",   CPU_TYPE_I386,      CPU_SUBTYPE_PENTIUM_4 },
	{ "m68030",     CPU_TYPE_MC680x0,   CPU_SUBTYPE_MC68030_ONLY },
	{ "m68040",     CPU_TYPE_MC680x0,   CPU_SUBTYPE_MC68040 },
	{ "hppa7100LC", CPU_TYPE_HPPA,      CPU_SUBTYPE_HPPA_7100LC },
	{ NULL,	0,		  0 }
};

/* names and types (if any) of input file specified on the commmand line */
struct input_file {
	const char *name;
	unsigned long size;
	struct fat_header *fat_header;
	struct fat_arch *fat_arches;
};
static struct input_file input_file;

/* Thin files from the input files to operate on */
struct thin_file {
	const char *name;
	char *addr;
	struct fat_arch fat_arch;
	int from_fat;
	int remove;
};
static struct thin_file *thin_files = NULL;
static unsigned long nthin_files = 0;

/* The specified output file */
static const char *output_file = NULL;
static unsigned long output_filemode = 0;
static struct utimbuf output_timep;
static int archives_in_input = 0;

static struct arch_flag *remove_arch_flags = NULL;
static unsigned long nremove_arch_flags = 0;

static struct fat_header fat_header;

/*
 * get_arch_from_flag() is passed a name of an architecture flag and returns
 * zero if that flag is not known and non-zero if the flag is known.
 * If the pointer to the arch_flag is not NULL it is filled in with the
 * arch_flag struct that matches the name.
 */
static int get_arch_from_flag(const char *name, struct arch_flag *arch_flag)
{
	unsigned long i;

	for (i = 0; arch_flags[i].name; i++) {
		if (!strcmp(arch_flags[i].name, name)) {
			if (arch_flag)
				*arch_flag = arch_flags[i];
			return 1;
		}
	}
	if (arch_flag)
		memset(arch_flag, '\0', sizeof(struct arch_flag));
	return 0;
}

static void *reallocate(void *p, unsigned long size)
{
	if (p == NULL)
		return malloc(size);
	return realloc(p, size);
}

/*
 * Create a new thin file struct, clear it and return it.
 */
static struct thin_file *new_thin(void)
{
	struct thin_file *thin;

	thin_files = reallocate(thin_files, (nthin_files + 1) * sizeof(struct thin_file));
	thin = thin_files + nthin_files;
	nthin_files++;
	memset(thin, '\0', sizeof(struct thin_file));
	return thin;
}

/*
 * Create a new arch_flag struct on the specified list, clear it and return it.
 */
static struct arch_flag *new_arch_flag(struct arch_flag **p_arch_flags, unsigned long *narch_flags)
{
	struct arch_flag *arch_flag;

	*p_arch_flags = reallocate(*p_arch_flags, (*narch_flags + 1) * sizeof(struct arch_flag));
	arch_flag = *p_arch_flags + *narch_flags;
	*narch_flags = *narch_flags + 1;
	memset(arch_flag, '\0', sizeof(struct arch_flag));
	return arch_flag;
}

/*
 * Function for qsort for comparing thin file's alignment
 */
static int cmp_qsort(const struct thin_file *thin1, const struct thin_file *thin2)
{
	return thin1->fat_arch.align - thin2->fat_arch.align;
}

/*
 * myround() rounds v to a multiple of r.
 */
static unsigned long myround(unsigned long v, unsigned long r)
{
	r--;
	v += r;
	v &= ~(long)r;
	return v;
}


#ifdef __LITTLE_ENDIAN__
static void swap_fat_header(struct fat_header *p_fat_header)
{
	p_fat_header->magic 	= SWAP_LONG(p_fat_header->magic);
	p_fat_header->nfat_arch = SWAP_LONG(p_fat_header->nfat_arch);
}

static void swap_fat_arch(struct fat_arch *fat_archs, unsigned long nfat_arch)
{
	unsigned long i;

	for (i = 0; i < nfat_arch; i++) {
		fat_archs[i].cputype	= SWAP_LONG(fat_archs[i].cputype);
		fat_archs[i].cpusubtype = SWAP_LONG(fat_archs[i].cpusubtype);
		fat_archs[i].offset 	= SWAP_LONG(fat_archs[i].offset);
		fat_archs[i].size   	= SWAP_LONG(fat_archs[i].size);
		fat_archs[i].align  	= SWAP_LONG(fat_archs[i].align);
	}
}
#endif

/*
 * create_fat() creates a fat output file from the thin files.
 */
static void create_fat(void)
{
	unsigned long i, offset;
	int fd;

	/*
	 * Create the output file.  The unlink() is done to handle the
	 * problem when the outputfile is not writable but the directory
	 * allows the file to be removed and thus created (since the file
	 * may not be there the return code of the unlink() is ignored).
	 */
	(void)unlink(output_file);
	if ((fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, output_filemode)) == -1) {
		fprintf(stderr, "can't create output file: %s", output_file);
		return;
	}

	/* sort the files by alignment to save space in the output file */
	if (nthin_files > 1)
		qsort(thin_files, nthin_files, sizeof(struct thin_file),
			  (int (*)(const void *, const void *))cmp_qsort);

	/* Fill in the fat header and the fat_arch's offsets. */
	fat_header.magic = FAT_MAGIC;
	fat_header.nfat_arch = nthin_files;
	offset = sizeof(struct fat_header) + nthin_files * sizeof(struct fat_arch);
	for (i = 0; i < nthin_files; i++) {
		offset = myround(offset, 1 << thin_files[i].fat_arch.align);
		thin_files[i].fat_arch.offset = offset;
		offset += thin_files[i].fat_arch.size;
	}

	/*
	 * If there is just one thin file on the list don't create a fat file.
	 */
	if (nthin_files != 1) {
#ifdef __LITTLE_ENDIAN__
		swap_fat_header(&fat_header);
#endif /* __LITTLE_ENDIAN__ */
		if (write(fd, &fat_header, sizeof(struct fat_header)) != sizeof(struct fat_header)) {
			fprintf(stderr, "can't write fat header to output file: %s", output_file);
			close(fd);
			return;
		}
#ifdef __LITTLE_ENDIAN__
		swap_fat_header(&fat_header);
#endif /* __LITTLE_ENDIAN__ */
		for (i = 0; i < nthin_files; i++) {
#ifdef __LITTLE_ENDIAN__
		swap_fat_arch(&(thin_files[i].fat_arch), 1);
#endif /* __LITTLE_ENDIAN__ */
		if (write(fd, &(thin_files[i].fat_arch), sizeof(struct fat_arch)) != sizeof(struct fat_arch)) {
			fprintf(stderr, "can't write fat arch to output file: %s", output_file);
			close(fd);
			return;
		}
#ifdef __LITTLE_ENDIAN__
		swap_fat_arch(&(thin_files[i].fat_arch), 1);
	#endif /* __LITTLE_ENDIAN__ */
		}
	}
	for (i = 0; i < nthin_files; i++) {
		if (nthin_files != 1)
			if (lseek(fd, thin_files[i].fat_arch.offset, L_SET) == -1) {
				fprintf(stderr, "can't lseek in output file: %s", output_file);
				close(fd);
				return;
			}
		if (write(fd, thin_files[i].addr, thin_files[i].fat_arch.size) != (int)(thin_files[i].fat_arch.size)) {
			fprintf(stderr, "can't write to output file: %s", output_file);
			close(fd);
			return;
		}
	}
	if (close(fd) == -1)
		fprintf(stderr, "can't close output file: %s", output_file);
	if (archives_in_input)
		if (utime(output_file, &output_timep) == -1)
			fprintf(stderr, "can't set the modifiy times for output file: %s", output_file);
}

/*
 * process_input_file() checks input file and breaks it down into thin files
 * for later operations.
 */
static void process_input_file(struct input_file *input)
{
	int fd;
	struct stat stat_buf;
	unsigned long size, i, j;
	kern_return_t r;
	char *addr;
	struct thin_file *thin;

	/* Open the input file and map it in */
	if ((fd = open(input->name, O_RDONLY)) == -1) {
		fprintf(stderr, "can't open input file: %s", input->name);
		return;
	}
	if (fstat(fd, &stat_buf) == -1) {
		close(fd);
		fprintf(stderr, "Can't stat input file: %s", input->name);
		return;
	}
	size = stat_buf.st_size;
	input->size = size;
	/* pick up set uid, set gid and sticky text bits */
	output_filemode = stat_buf.st_mode & 07777;
	/*
	 * Select the eariliest modifiy time so that if the output file
	 * contains archives with table of contents lipo will not make them
	 * out of date.  This logic however could make an out of date table of
	 * contents appear up todate if another file is combined with it that
	 * has a date early enough.
	 */
	if (output_timep.modtime == 0 || output_timep.modtime > stat_buf.st_mtime) {
		output_timep.actime = stat_buf.st_atime;
		output_timep.modtime = stat_buf.st_mtime;
	}
	if (map_fd((int)fd, (vm_offset_t)0, (vm_offset_t *)&addr, (boolean_t)1, (vm_size_t)size) != KERN_SUCCESS) {
		fprintf(stderr, "Can't map input file: %s", input->name);
		close(fd);
		return;
	}
	close(fd);

	/* Try to figure out what kind of file this is */

	/* see if this file is a fat file */
	if (size >= sizeof(struct fat_header) &&
#ifdef __BIG_ENDIAN__
	   *((unsigned long *)addr) == FAT_MAGIC)
#endif /* __BIG_ENDIAN__ */
#ifdef __LITTLE_ENDIAN__
	   *((unsigned long *)addr) == SWAP_LONG(FAT_MAGIC))
#endif /* __LITTLE_ENDIAN__ */
	{
		input->fat_header = (struct fat_header *)addr;
#ifdef __LITTLE_ENDIAN__
		swap_fat_header(input->fat_header);
#endif /* __LITTLE_ENDIAN__ */
		if (sizeof(struct fat_header) + input->fat_header->nfat_arch * sizeof(struct fat_arch) > size) {
			fprintf(stderr, "truncated or malformed fat file (fat_arch structs would "
					"extend past the end of the file) %s", input->name);
			vm_deallocate(mach_task_self(), (vm_offset_t)addr, (vm_size_t)size);
			input->fat_header = NULL;
			return;
		}
		input->fat_arches = (struct fat_arch *)(addr + sizeof(struct fat_header));
#ifdef __LITTLE_ENDIAN__
		swap_fat_arch(input->fat_arches, input->fat_header->nfat_arch);
#endif /* __LITTLE_ENDIAN__ */
		for (i = 0; i < input->fat_header->nfat_arch; i++) {
			if (input->fat_arches[i].offset + input->fat_arches[i].size > size) {
				fprintf(stderr, "truncated or malformed fat file (offset plus size "
						"of cputype (%d) cpusubtype (%d) extends past the "
						"end of the file) %s", input->fat_arches[i].cputype,
						input->fat_arches[i].cpusubtype, input->name);
				vm_deallocate(mach_task_self(), (vm_offset_t)addr, (vm_size_t)size);
				input->fat_header = NULL;
				return;
			}
			if (input->fat_arches[i].align > MAXSECTALIGN) {
				fprintf(stderr, "align (2^%u) too large of fat file %s (cputype (%d)"
						" cpusubtype (%d)) (maximum 2^%d)",
						input->fat_arches[i].align, input->name,
						input->fat_arches[i].cputype,
						input->fat_arches[i].cpusubtype, MAXSECTALIGN);
				vm_deallocate(mach_task_self(), (vm_offset_t)addr, (vm_size_t)size);
				input->fat_header = NULL;
				return;
			}
			if (input->fat_arches[i].offset % (1 << input->fat_arches[i].align) != 0) {
				fprintf(stderr, "offset %u of fat file %s (cputype (%d) cpusubtype "
						"(%d)) not aligned on it's alignment (2^%u)",
						input->fat_arches[i].offset, input->name,
						input->fat_arches[i].cputype,
						input->fat_arches[i].cpusubtype,
						input->fat_arches[i].align);
				vm_deallocate(mach_task_self(), (vm_offset_t)addr, (vm_size_t)size);
				input->fat_header = NULL;
				return;
			}
		}
		for (i = 0; i < input->fat_header->nfat_arch; i++) {
			for (j = i + 1; j < input->fat_header->nfat_arch; j++) {
				if (input->fat_arches[i].cputype == input->fat_arches[j].cputype &&
				   input->fat_arches[i].cpusubtype == input->fat_arches[j].cpusubtype) {
					fprintf(stderr, "fat file %s contains two of the same architecture "
							"(cputype (%d) cpusubtype (%d))", input->name,
							input->fat_arches[i].cputype,
							input->fat_arches[i].cpusubtype);
					vm_deallocate(mach_task_self(), (vm_offset_t)addr, (vm_size_t)size);
					input->fat_header = NULL;
					return;
				}
			}
		}

		/* create a thin file struct for each arch in the fat file */
		for (i = 0; i < input->fat_header->nfat_arch; i++) {
			thin = new_thin();
			thin->name = input->name;
			thin->addr = addr + input->fat_arches[i].offset;
			thin->fat_arch = input->fat_arches[i];
			thin->from_fat = 1;
			if (input->fat_arches[i].size >= SARMAG && strncmp(thin->addr, ARMAG, SARMAG) == 0)
				archives_in_input = 1;
		}
	} else {
		r = vm_deallocate(mach_task_self(), (vm_offset_t)addr, (vm_size_t)size);
		if (r != KERN_SUCCESS)
			mach_error("vm_deallocate", r);
	}
}

int run_lipo(const char *path, const char *archs[], unsigned num_archs)
{
	unsigned a;
	unsigned long i, j;
	struct arch_flag *arch_flag;

	/*
	 * Check to see the specified arguments are valid.
	 */
	if (!num_archs || !path)
		return 1;

	/* reset static variables */
	thin_files = NULL;
	nthin_files = 0;
	output_filemode = 0;
	memset(&output_timep, 0, sizeof(output_timep));
	memset(&fat_header, 0, sizeof(fat_header));
	archives_in_input = 0;
	remove_arch_flags = NULL;
	nremove_arch_flags = 0;

	/*
	 * Process the arguments.
	 */
	output_file = path;
	input_file.name = path;
	input_file.size = 0;
	input_file.fat_header = NULL;
	input_file.fat_arches = NULL;
	for (a = 0; a < num_archs; a++) {
		arch_flag = new_arch_flag(&remove_arch_flags, &nremove_arch_flags);
		if (!get_arch_from_flag(archs[a], arch_flag)) {
			fprintf(stderr, "unknown architecture specification flag: %s", archs[a]);
			if (remove_arch_flags)
				free(remove_arch_flags);
			return 1;
		}
	}

	/*
	 * Determine the types of the input files.
	 */
	process_input_file(&input_file);

	/*
	 * Do the specified operation.
	 */

	if (!input_file.fat_header) {
		fprintf(stderr, "input file (%s) must be a fat file", input_file.name);
		if (remove_arch_flags)
			free(remove_arch_flags);
		return 1;
	}
	for (i = 0; i < nremove_arch_flags; i++) {
		for (j = i + 1; j < nremove_arch_flags; j++) {
			if (remove_arch_flags[i].cputype == remove_arch_flags[j].cputype &&
			   remove_arch_flags[i].cpusubtype == remove_arch_flags[j].cpusubtype)
				fprintf(stderr, "-remove %s specified multiple times", remove_arch_flags[i].name);
		}
	}
	/* mark those thin files for removal */
	for (i = 0; i < nremove_arch_flags; i++) {
		for (j = 0; j < nthin_files; j++) {
			if (remove_arch_flags[i].cputype == thin_files[j].fat_arch.cputype &&
			   remove_arch_flags[i].cpusubtype == thin_files[j].fat_arch.cpusubtype) {
				thin_files[j].remove = 1;
				break;
			}
		}
	}
	/* remove those thin files marked for removal */
	for (i = 0; i < nthin_files; ) {
		if (thin_files[i].remove) {
			for (j = i; j < nthin_files; j++)
				thin_files[j] = thin_files[j + 1];
			nthin_files--;
		} else
			i++;
	}
	if (nthin_files)
		create_fat();
	else
		fprintf(stderr, "-remove's specified would result in an empty fat file");

	kern_return_t ret = vm_deallocate(mach_task_self(), (vm_offset_t)input_file.fat_header, (vm_size_t)input_file.size);
	if (ret != KERN_SUCCESS)
		mach_error("vm_deallocate", ret);

	if (remove_arch_flags)
		free(remove_arch_flags);
	if (thin_files)
		free(thin_files);

	return 0;
}