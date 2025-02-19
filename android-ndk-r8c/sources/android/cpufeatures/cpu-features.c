/*
 * Copyright (C) 2010 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

/* ChangeLog for this library:
 *
 * NDK r??: Add new ARM CPU features: VFPv2, VFP_D32, VFP_FP16,
 *          VFP_FMA, NEON_FMA, IDIV_ARM, IDIV_THUMB2 and iWMMXt.
 *
 *          Rewrite the code to parse /proc/self/auxv instead of
 *          the "Features" field in /proc/cpuinfo.
 *
 *          Dynamically allocate the buffer that hold the content
 *          of /proc/cpuinfo to deal with newer hardware.
 *
 * NDK r7c: Fix CPU count computation. The old method only reported the
 *           number of _active_ CPUs when the library was initialized,
 *           which could be less than the real total.
 *
 * NDK r5: Handle buggy kernels which report a CPU Architecture number of 7
 *         for an ARMv6 CPU (see below).
 *
 *         Handle kernels that only report 'neon', and not 'vfpv3'
 *         (VFPv3 is mandated by the ARM architecture is Neon is implemented)
 *
 *         Handle kernels that only report 'vfpv3d16', and not 'vfpv3'
 *
 *         Fix x86 compilation. Report ANDROID_CPU_FAMILY_X86 in
 *         android_getCpuFamily().
 *
 * NDK r4: Initial release
 */
#include <sys/system_properties.h>
#ifdef __arm__
#include <machine/cpu-features.h>
#endif
#include <pthread.h>
#include "cpu-features.h"
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>

static  pthread_once_t     g_once;
static  AndroidCpuFamily   g_cpuFamily;
static  uint64_t           g_cpuFeatures;
static  int                g_cpuCount;

static const int  android_cpufeatures_debug = 0;

#ifdef __arm__
#  define DEFAULT_CPU_FAMILY  ANDROID_CPU_FAMILY_ARM
#elif defined __i386__
#  define DEFAULT_CPU_FAMILY  ANDROID_CPU_FAMILY_X86
#else
#  define DEFAULT_CPU_FAMILY  ANDROID_CPU_FAMILY_UNKNOWN
#endif

#define  D(...) \
    do { \
        if (android_cpufeatures_debug) { \
            printf(__VA_ARGS__); fflush(stdout); \
        } \
    } while (0)

#ifdef __i386__
static __inline__ void x86_cpuid(int func, int values[4])
{
    int a, b, c, d;
    /* We need to preserve ebx since we're compiling PIC code */
    /* this means we can't use "=b" for the second output register */
    __asm__ __volatile__ ( \
      "push %%ebx\n"
      "cpuid\n" \
      "mov %1, %%ebx\n"
      "pop %%ebx\n"
      : "=a" (a), "=r" (b), "=c" (c), "=d" (d) \
      : "a" (func) \
    );
    values[0] = a;
    values[1] = b;
    values[2] = c;
    values[3] = d;
}
#endif

/* Get the size of a file by reading it until the end. This is needed
 * because files under /proc do not always return a valid size when
 * using fseek(0, SEEK_END) + ftell(). Nor can they be mmap()-ed.
 */
static int
get_file_size(const char* pathname)
{
    int fd, ret, result = 0;
    char buffer[256];

    fd = open(pathname, O_RDONLY);
    if (fd < 0) {
      D("Can't open %s: %s\n", pathname, strerror(errno));
      return -1;
    }

    for (;;) {
      int ret = read(fd, buffer, sizeof buffer);
      if (ret < 0) {
        if (errno == EINTR)
          continue;
        D("Error while reading %s: %s\n", pathname, strerror(errno));
        break;
      }
      if (ret == 0)
        break;

      result += ret;
    }
    close(fd);
    return result;
}

/* Read the content of /proc/cpuinfo into a user-provided buffer.
 * Return the length of the data, or -1 on error. Does *not*
 * zero-terminate the content. Will not read more
 * than 'buffsize' bytes.
 */
static int
read_file(const char*  pathname, char*  buffer, size_t  buffsize)
{
    int  fd, count;

    fd = open(pathname, O_RDONLY);
    if (fd < 0) {
        D("Could not open %s: %s\n", pathname, strerror(errno));
        return -1;
    }
    count = 0;
    while (count < (int)buffsize) {
        int ret = read(fd, buffer + count, buffsize - count);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            D("Error while reading from %s: %s\n", pathname, strerror(errno));
            if (count == 0)
                count = -1;
            break;
        }
        if (ret == 0)
            break;
        count += ret;
    }
    close(fd);
    return count;
}

/* Extract the content of a the first occurence of a given field in
 * the content of /proc/cpuinfo and return it as a heap-allocated
 * string that must be freed by the caller.
 *
 * Return NULL if not found
 */
static char*
extract_cpuinfo_field(char* buffer, int buflen, const char* field)
{
    int  fieldlen = strlen(field);
    char* bufend = buffer + buflen;
    char* result = NULL;
    int len, ignore;
    const char *p, *q;

    /* Look for first field occurence, and ensures it starts the line. */
    p = buffer;
    bufend = buffer + buflen;
    for (;;) {
        p = memmem(p, bufend-p, field, fieldlen);
        if (p == NULL)
            goto EXIT;

        if (p == buffer || p[-1] == '\n')
            break;

        p += fieldlen;
    }

    /* Skip to the first column followed by a space */
    p += fieldlen;
    p  = memchr(p, ':', bufend-p);
    if (p == NULL || p[1] != ' ')
        goto EXIT;

    /* Find the end of the line */
    p += 2;
    q = memchr(p, '\n', bufend-p);
    if (q == NULL)
        q = bufend;

    /* Copy the line into a heap-allocated buffer */
    len = q-p;
    result = malloc(len+1);
    if (result == NULL)
        goto EXIT;

    memcpy(result, p, len);
    result[len] = '\0';

EXIT:
    return result;
}

/* Like strlen(), but for constant string literals */
#define STRLEN_CONST(x)  ((sizeof(x)-1)


/* Checks that a space-separated list of items contains one given 'item'.
 * Returns 1 if found, 0 otherwise.
 */
static int
has_list_item(const char* list, const char* item)
{
    const char*  p = list;
    int itemlen = strlen(item);

    if (list == NULL)
        return 0;

    while (*p) {
        const char*  q;

        /* skip spaces */
        while (*p == ' ' || *p == '\t')
            p++;

        /* find end of current list item */
        q = p;
        while (*q && *q != ' ' && *q != '\t')
            q++;

        if (itemlen == q-p && !memcmp(p, item, itemlen))
            return 1;

        /* skip to next item */
        p = q;
    }
    return 0;
}

/* Parse an decimal integer starting from 'input', but not going further
 * than 'limit'. Return the value into '*result'.
 *
 * NOTE: Does not skip over leading spaces, or deal with sign characters.
 * NOTE: Ignores overflows.
 *
 * The function returns NULL in case of error (bad format), or the new
 * position after the decimal number in case of success (which will always
 * be <= 'limit').
 */
static const char*
parse_decimal(const char* input, const char* limit, int* result)
{
    const char* p = input;
    int val = 0;
    while (p < limit) {
        int d = (*p - '0');
        if ((unsigned)d >= 10U)
            break;
        val = val*10 + d;
        p++;
    }
    if (p == input)
        return NULL;

    *result = val;
    return p;
}

/* This small data type is used to represent a CPU list / mask, as read
 * from sysfs on Linux. See http://www.kernel.org/doc/Documentation/cputopology.txt
 *
 * For now, we don't expect more than 32 cores on mobile devices, so keep
 * everything simple.
 */
typedef struct {
    uint32_t mask;
} CpuList;

static __inline__ void
cpulist_init(CpuList* list) {
    list->mask = 0;
}

static __inline__ void
cpulist_and(CpuList* list1, CpuList* list2) {
    list1->mask &= list2->mask;
}

static __inline__ void
cpulist_set(CpuList* list, int index) {
    if ((unsigned)index < 32) {
        list->mask |= (uint32_t)(1U << index);
    }
}

static __inline__ int
cpulist_count(CpuList* list) {
    return __builtin_popcount(list->mask);
}

/* Parse a textual list of cpus and store the result inside a CpuList object.
 * Input format is the following:
 * - comma-separated list of items (no spaces)
 * - each item is either a single decimal number (cpu index), or a range made
 *   of two numbers separated by a single dash (-). Ranges are inclusive.
 *
 * Examples:   0
 *             2,4-127,128-143
 *             0-1
 */
static void
cpulist_parse(CpuList* list, const char* line, int line_len)
{
    const char* p = line;
    const char* end = p + line_len;
    const char* q;

    /* NOTE: the input line coming from sysfs typically contains a
     * trailing newline, so take care of it in the code below
     */
    while (p < end && *p != '\n')
    {
        int val, start_value, end_value;

        /* Find the end of current item, and put it into 'q' */
        q = memchr(p, ',', end-p);
        if (q == NULL) {
            q = end;
        }

        /* Get first value */
        p = parse_decimal(p, q, &start_value);
        if (p == NULL)
            goto BAD_FORMAT;

        end_value = start_value;

        /* If we're not at the end of the item, expect a dash and
         * and integer; extract end value.
         */
        if (p < q && *p == '-') {
            p = parse_decimal(p+1, q, &end_value);
            if (p == NULL)
                goto BAD_FORMAT;
        }

        /* Set bits CPU list bits */
        for (val = start_value; val <= end_value; val++) {
            cpulist_set(list, val);
        }

        /* Jump to next item */
        p = q;
        if (p < end)
            p++;
    }

BAD_FORMAT:
    ;
}

/* Read a CPU list from one sysfs file */
static void
cpulist_read_from(CpuList* list, const char* filename)
{
    char   file[64];
    int    filelen;

    cpulist_init(list);

    filelen = read_file(filename, file, sizeof file);
    if (filelen < 0) {
        D("Could not read %s: %s\n", filename, strerror(errno));
        return;
    }

    cpulist_parse(list, file, filelen);
}

// See <asm/hwcap.h> kernel header.
#define HWCAP_VFP       (1 << 6)
#define HWCAP_IWMMXT    (1 << 9)
#define HWCAP_NEON      (1 << 12)
#define HWCAP_VFPv3     (1 << 13)
#define HWCAP_VFPv3D16  (1 << 14)
#define HWCAP_VFPv4     (1 << 16)
#define HWCAP_IDIVA     (1 << 17)
#define HWCAP_IDIVT     (1 << 18)

#define AT_HWCAP 16

/* Read the ELF HWCAP flags by parsing /proc/self/auxv
 */
static uint32_t
get_elf_hwcap(void)
{
    uint32_t result = 0;
    const char filepath[] = "/proc/self/auxv";
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        D("Could not open %s: %s\n", filepath, strerror(errno));
        return 0;
    }

    struct { uint32_t tag; uint32_t value; } entry;

    for (;;) {
        int ret = read(fd, (char*)&entry, sizeof entry);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            D("Error while reading %s: %s\n", filepath, strerror(errno));
            break;
        }
        // Detect end of list.
        if (ret == 0 || (entry.tag == 0 && entry.value == 0))
          break;
        if (entry.tag == AT_HWCAP) {
          result = entry.value;
          break;
        }
    }
    close(fd);
    return result;
}

/* Return the number of cpus present on a given device.
 *
 * To handle all weird kernel configurations, we need to compute the
 * intersection of the 'present' and 'possible' CPU lists and count
 * the result.
 */
static int
get_cpu_count(void)
{
    CpuList cpus_present[1];
    CpuList cpus_possible[1];

    cpulist_read_from(cpus_present, "/sys/devices/system/cpu/present");
    cpulist_read_from(cpus_possible, "/sys/devices/system/cpu/possible");

    /* Compute the intersection of both sets to get the actual number of
     * CPU cores that can be used on this device by the kernel.
     */
    cpulist_and(cpus_present, cpus_possible);

    return cpulist_count(cpus_present);
}

static void
android_cpuInit(void)
{
    char* cpuinfo = NULL;
    int   cpuinfo_len;

    g_cpuFamily   = DEFAULT_CPU_FAMILY;
    g_cpuFeatures = 0;
    g_cpuCount    = 1;

    cpuinfo_len = get_file_size("/proc/cpuinfo");
    if (cpuinfo_len < 0) {
      D("cpuinfo_len cannot be computed!");
      return;
    }
    cpuinfo = malloc(cpuinfo_len);
    if (cpuinfo == NULL) {
      D("cpuinfo buffer could not be allocated");
      return;
    }
    cpuinfo_len = read_file("/proc/cpuinfo", cpuinfo, cpuinfo_len);
    D("cpuinfo_len is (%d):\n%.*s\n", cpuinfo_len,
      cpuinfo_len >= 0 ? cpuinfo_len : 0, cpuinfo);

    if (cpuinfo_len < 0)  /* should not happen */ {
        free(cpuinfo);
        return;
    }

    /* Count the CPU cores, the value may be 0 for single-core CPUs */
    g_cpuCount = get_cpu_count();
    if (g_cpuCount == 0) {
        g_cpuCount = 1;
    }

    D("found cpuCount = %d\n", g_cpuCount);

#ifdef __ARM_ARCH__
    {
        char*  features = NULL;
        char*  architecture = NULL;

        /* Extract architecture from the "CPU Architecture" field.
         * The list is well-known, unlike the the output of
         * the 'Processor' field which can vary greatly.
         *
         * See the definition of the 'proc_arch' array in
         * $KERNEL/arch/arm/kernel/setup.c and the 'c_show' function in
         * same file.
         */
        char* cpuArch = extract_cpuinfo_field(cpuinfo, cpuinfo_len, "CPU architecture");

        if (cpuArch != NULL) {
            char*  end;
            long   archNumber;
            int    hasARMv7 = 0;

            D("found cpuArch = '%s'\n", cpuArch);

            /* read the initial decimal number, ignore the rest */
            archNumber = strtol(cpuArch, &end, 10);

            /* Here we assume that ARMv8 will be upwards compatible with v7
             * in the future. Unfortunately, there is no 'Features' field to
             * indicate that Thumb-2 is supported.
             */
            if (end > cpuArch && archNumber >= 7) {
                hasARMv7 = 1;
            }

            /* Unfortunately, it seems that certain ARMv6-based CPUs
             * report an incorrect architecture number of 7!
             *
             * See http://code.google.com/p/android/issues/detail?id=10812
             *
             * We try to correct this by looking at the 'elf_format'
             * field reported by the 'Processor' field, which is of the
             * form of "(v7l)" for an ARMv7-based CPU, and "(v6l)" for
             * an ARMv6-one.
             */
            if (hasARMv7) {
                char* cpuProc = extract_cpuinfo_field(cpuinfo, cpuinfo_len,
                                                      "Processor");
                if (cpuProc != NULL) {
                    D("found cpuProc = '%s'\n", cpuProc);
                    if (has_list_item(cpuProc, "(v6l)")) {
                        D("CPU processor and architecture mismatch!!\n");
                        hasARMv7 = 0;
                    }
                    free(cpuProc);
                }
            }

            if (hasARMv7) {
                g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_ARMv7;
            }

            /* The LDREX / STREX instructions are available from ARMv6 */
            if (archNumber >= 6) {
                g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_LDREX_STREX;
            }

            free(cpuArch);
        }

        /* Extract the list of CPU features from ELF hwcaps */
        uint32_t hwcaps = get_elf_hwcap();

        if (hwcaps != 0) {
            int has_vfp = (hwcaps & HWCAP_VFP);
            int has_vfpv3 = (hwcaps & HWCAP_VFPv3);
            int has_vfpv3d16 = (hwcaps & HWCAP_VFPv3D16);
            int has_vfpv4 = (hwcaps & HWCAP_VFPv4);
            int has_neon = (hwcaps & HWCAP_NEON);
            int has_idiva = (hwcaps & HWCAP_IDIVA);
            int has_idivt = (hwcaps & HWCAP_IDIVT);
            int has_iwmmxt = (hwcaps & HWCAP_IWMMXT);

            // The kernel does a poor job at ensuring consistency when
            // describing CPU features. So lots of guessing is needed.

            // 'vfpv4' implies VFPv3|VFP_FMA|FP16
            if (has_vfpv4)
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_VFPv3    |
                               ANDROID_CPU_ARM_FEATURE_VFP_FP16 |
                               ANDROID_CPU_ARM_FEATURE_VFP_FMA;

            // 'vfpv3' or 'vfpv3d16' imply VFPv3. Note that unlike GCC,
            // a value of 'vfpv3' doesn't necessarily mean that the D32
            // feature is present, so be conservative. All CPUs in the
            // field that support D32 also support NEON, so this should
            // not be a problem in practice.
            if (has_vfpv3 || has_vfpv3d16)
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_VFPv3;

            // 'vfp' is super ambiguous. Depending on the kernel, it can
            // either mean VFPv2 or VFPv3. Make it depend on ARMv7.
            if (has_vfp) {
              if (g_cpuFeatures & ANDROID_CPU_ARM_FEATURE_ARMv7)
                g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_VFPv3;
              else
                g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_VFPv2;
            }

            // Neon implies VFPv3|D32, and if vfpv4 is detected, NEON_FMA
            if (has_neon) {
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_VFPv3 |
                               ANDROID_CPU_ARM_FEATURE_NEON |
                               ANDROID_CPU_ARM_FEATURE_VFP_D32;
              if (has_vfpv4)
                g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_NEON_FMA;
            }

            // VFPv3 implies VFPv2 and ARMv7
            if (g_cpuFeatures & ANDROID_CPU_ARM_FEATURE_VFPv3)
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_VFPv2 |
                               ANDROID_CPU_ARM_FEATURE_ARMv7;

            // Note that some buggy kernels do not report these even when
            // the CPU actually support the division instructions. However,
            // assume that if 'vfpv4' is detected, then the CPU supports
            // sdiv/udiv properly.
            if (has_idiva || has_vfpv4)
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_IDIV_ARM;
            if (has_idivt || has_vfpv4)
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_IDIV_THUMB2;

            if (has_iwmmxt)
              g_cpuFeatures |= ANDROID_CPU_ARM_FEATURE_iWMMXt;
        }
    }
#endif /* __ARM_ARCH__ */

#ifdef __i386__
    g_cpuFamily = ANDROID_CPU_FAMILY_X86;

    int regs[4];

/* According to http://en.wikipedia.org/wiki/CPUID */
#define VENDOR_INTEL_b  0x756e6547
#define VENDOR_INTEL_c  0x6c65746e
#define VENDOR_INTEL_d  0x49656e69

    x86_cpuid(0, regs);
    int vendorIsIntel = (regs[1] == VENDOR_INTEL_b &&
                         regs[2] == VENDOR_INTEL_c &&
                         regs[3] == VENDOR_INTEL_d);

    x86_cpuid(1, regs);
    if ((regs[2] & (1 << 9)) != 0) {
        g_cpuFeatures |= ANDROID_CPU_X86_FEATURE_SSSE3;
    }
    if ((regs[2] & (1 << 23)) != 0) {
        g_cpuFeatures |= ANDROID_CPU_X86_FEATURE_POPCNT;
    }
    if (vendorIsIntel && (regs[2] & (1 << 22)) != 0) {
        g_cpuFeatures |= ANDROID_CPU_X86_FEATURE_MOVBE;
    }
#endif

#ifdef _MIPS_ARCH
    g_cpuFamily = ANDROID_CPU_FAMILY_MIPS;
#endif /* _MIPS_ARCH */

    free(cpuinfo);
}


AndroidCpuFamily
android_getCpuFamily(void)
{
    pthread_once(&g_once, android_cpuInit);
    return g_cpuFamily;
}


uint64_t
android_getCpuFeatures(void)
{
    pthread_once(&g_once, android_cpuInit);
    return g_cpuFeatures;
}


int
android_getCpuCount(void)
{
    pthread_once(&g_once, android_cpuInit);
    return g_cpuCount;
}


/*
 * Technical note: Making sense of ARM's FPU architecture versions.
 *
 * FPA was ARM's first attempt at an FPU architecture. There is no Android
 * device that actually uses it since this technology was already obsolete
 * when the project started. If you see references to FPA instructions
 * somewhere, you can be sure that this doesn't apply to Android at all.
 *
 * FPA was followed by "VFP", soon renamed "VFPv1" due to the emergence of
 * new versions / additions to it. ARM considers this obsolete right now,
 * and no known Android device implements it either.
 *
 * VFPv2 added a few instructions to VFPv1, and is an *optional* extension
 * supported by some ARMv5TE, ARMv6 and ARMv6T2 CPUs. Note that a device
 * supporting the 'armeabi' ABI doesn't necessarily support these.
 *
 * VFPv3-D16 adds a few instructions on top of VFPv2 and is typically used
 * on ARMv7-A CPUs which implement a FPU. Note that it is also mandated
 * by the Android 'armeabi-v7a' ABI. The -D16 suffix in its name means
 * that it provides 16 double-precision FPU registers (d0-d15) and 32
 * single-precision ones (s0-s31) which happen to be mapped to the same
 * register banks.
 *
 * VFPv3-D32 is the name of an extension to VFPv3-D16 that provides 16
 * additional double precision registers (d16-d31). Note that there are
 * still only 32 single precision registers.
 *
 * VFPv3xD is a *subset* of VFPv3-D16 that only provides single-precision
 * registers. It is only used on ARMv7-M (i.e. on micro-controllers) which
 * are not supported by Android. Note that it is not compatible with VFPv2.
 *
 * NOTE: The term 'VFPv3' usually designate either VFPv3-D16 or VFPv3-D32
 *       depending on context. For example GCC uses it for VFPv3-D32, but
 *       the Linux kernel code uses it for VFPv3-D16 (especially in
 *       /proc/cpuinfo). Always try to use the full designation when
 *       possible.
 *
 * NEON, a.k.a. "ARM Advanced SIMD" is an extension that provides
 * instructions to perform parallel computations on vectors of 8, 16,
 * 32, 64 and 128 bit quantities. NEON requires VFPv32-D32 since all
 * NEON registers are also mapped to the same register banks.
 *
 * VFPv4-D16, adds a few instructions on top of VFPv3-D16 in order to
 * perform fused multiply-accumulate on VFP registers, as well as
 * half-precision (16-bit) conversion operations.
 *
 * VFPv4-D32 is VFPv4-D16 with 32, instead of 16, FPU double precision
 * registers.
 *
 * VPFv4-NEON is VFPv4-D32 with NEON instructions. It also adds fused
 * multiply-accumulate instructions that work on the NEON registers.
 *
 * NOTE: Similarly, "VFPv4" might either reference VFPv4-D16 or VFPv4-D32
 *       depending on context.
 *
 * The following information was determined by scanning the binutils-2.22
 * sources:
 *
 * Basic VFP instruction subsets:
 *
 * #define FPU_VFP_EXT_V1xD 0x08000000     // Base VFP instruction set.
 * #define FPU_VFP_EXT_V1   0x04000000     // Double-precision insns.
 * #define FPU_VFP_EXT_V2   0x02000000     // ARM10E VFPr1.
 * #define FPU_VFP_EXT_V3xD 0x01000000     // VFPv3 single-precision.
 * #define FPU_VFP_EXT_V3   0x00800000     // VFPv3 double-precision.
 * #define FPU_NEON_EXT_V1  0x00400000     // Neon (SIMD) insns.
 * #define FPU_VFP_EXT_D32  0x00200000     // Registers D16-D31.
 * #define FPU_VFP_EXT_FP16 0x00100000     // Half-precision extensions.
 * #define FPU_NEON_EXT_FMA 0x00080000     // Neon fused multiply-add
 * #define FPU_VFP_EXT_FMA  0x00040000     // VFP fused multiply-add
 *
 * FPU types (excluding NEON)
 *
 * FPU_VFP_V1xD (EXT_V1xD)
 *    |
 *    +--------------------------+
 *    |                          |
 * FPU_VFP_V1 (+EXT_V1)       FPU_VFP_V3xD (+EXT_V2+EXT_V3xD)
 *    |                          |
 *    |                          |
 * FPU_VFP_V2 (+EXT_V2)       FPU_VFP_V4_SP_D16 (+EXT_FP16+EXT_FMA)
 *    |
 * FPU_VFP_V3D16 (+EXT_Vx3D+EXT_V3)
 *    |
 *    +--------------------------+
 *    |                          |
 * FPU_VFP_V3 (+EXT_D32)     FPU_VFP_V4D16 (+EXT_FP16+EXT_FMA)
 *    |                          |
 *    |                      FPU_VFP_V4 (+EXT_D32)
 *    |
 * FPU_VFP_HARD (+EXT_FMA+NEON_EXT_FMA)
 *
 * VFP architectures:
 *
 * ARCH_VFP_V1xD  (EXT_V1xD)
 *   |
 *   +------------------+
 *   |                  |
 *   |             ARCH_VFP_V3xD (+EXT_V2+EXT_V3xD)
 *   |                  |
 *   |             ARCH_VFP_V3xD_FP16 (+EXT_FP16)
 *   |                  |
 *   |             ARCH_VFP_V4_SP_D16 (+EXT_FMA)
 *   |
 * ARCH_VFP_V1 (+EXT_V1)
 *   |
 * ARCH_VFP_V2 (+EXT_V2)
 *   |
 * ARCH_VFP_V3D16 (+EXT_V3xD+EXT_V3)
 *   |
 *   +-------------------+
 *   |                   |
 *   |         ARCH_VFP_V3D16_FP16  (+EXT_FP16)
 *   |
 *   +-------------------+
 *   |                   |
 *   |         ARCH_VFP_V4_D16 (+EXT_FP16+EXT_FMA)
 *   |                   |
 *   |         ARCH_VFP_V4 (+EXT_D32)
 *   |                   |
 *   |         ARCH_NEON_VFP_V4 (+EXT_NEON+EXT_NEON_FMA)
 *   |
 * ARCH_VFP_V3 (+EXT_D32)
 *   |
 *   +-------------------+
 *   |                   |
 *   |         ARCH_VFP_V3_FP16 (+EXT_FP16)
 *   |
 * ARCH_VFP_V3_PLUS_NEON_V1 (+EXT_NEON)
 *   |
 * ARCH_NEON_FP16 (+EXT_FP16)
 *
 * -fpu=<name> values and their correspondance with FPU architectures above:
 *
 *   {"vfp",               FPU_ARCH_VFP_V2},
 *   {"vfp9",              FPU_ARCH_VFP_V2},
 *   {"vfp3",              FPU_ARCH_VFP_V3}, // For backwards compatbility.
 *   {"vfp10",             FPU_ARCH_VFP_V2},
 *   {"vfp10-r0",          FPU_ARCH_VFP_V1},
 *   {"vfpxd",             FPU_ARCH_VFP_V1xD},
 *   {"vfpv2",             FPU_ARCH_VFP_V2},
 *   {"vfpv3",             FPU_ARCH_VFP_V3},
 *   {"vfpv3-fp16",        FPU_ARCH_VFP_V3_FP16},
 *   {"vfpv3-d16",         FPU_ARCH_VFP_V3D16},
 *   {"vfpv3-d16-fp16",    FPU_ARCH_VFP_V3D16_FP16},
 *   {"vfpv3xd",           FPU_ARCH_VFP_V3xD},
 *   {"vfpv3xd-fp16",      FPU_ARCH_VFP_V3xD_FP16},
 *   {"neon",              FPU_ARCH_VFP_V3_PLUS_NEON_V1},
 *   {"neon-fp16",         FPU_ARCH_NEON_FP16},
 *   {"vfpv4",             FPU_ARCH_VFP_V4},
 *   {"vfpv4-d16",         FPU_ARCH_VFP_V4D16},
 *   {"fpv4-sp-d16",       FPU_ARCH_VFP_V4_SP_D16},
 *   {"neon-vfpv4",        FPU_ARCH_NEON_VFP_V4},
 *
 *
 * Simplified diagram that only includes FPUs supported by Android:
 * Only ARCH_VFP_V3D16 is actually mandated by the armeabi-v7a ABI,
 * all others are optional and must be probed at runtime.
 *
 * ARCH_VFP_V3D16 (EXT_V1xD+EXT_V1+EXT_V2+EXT_V3xD+EXT_V3)
 *   |
 *   +-------------------+
 *   |                   |
 *   |         ARCH_VFP_V3D16_FP16  (+EXT_FP16)
 *   |
 *   +-------------------+
 *   |                   |
 *   |         ARCH_VFP_V4_D16 (+EXT_FP16+EXT_FMA)
 *   |                   |
 *   |         ARCH_VFP_V4 (+EXT_D32)
 *   |                   |
 *   |         ARCH_NEON_VFP_V4 (+EXT_NEON+EXT_NEON_FMA)
 *   |
 * ARCH_VFP_V3 (+EXT_D32)
 *   |
 *   +-------------------+
 *   |                   |
 *   |         ARCH_VFP_V3_FP16 (+EXT_FP16)
 *   |
 * ARCH_VFP_V3_PLUS_NEON_V1 (+EXT_NEON)
 *   |
 * ARCH_NEON_FP16 (+EXT_FP16)
 *
 */
