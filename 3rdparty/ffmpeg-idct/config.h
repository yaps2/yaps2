/* Minimal config.h stub for standalone FFmpeg NEON IDCT */
#ifndef FFMPEG_IDCT_CONFIG_H
#define FFMPEG_IDCT_CONFIG_H

/* Mach-O prefixes C symbols with '_'; ffmpeg's configure sets this on Darwin */
#ifdef __APPLE__
#define EXTERN_ASM _
#else
#define EXTERN_ASM
#endif
#define HAVE_AS_FUNC 0
#define HAVE_AS_ARCH_DIRECTIVE 0
#define HAVE_SECTION_DATA_REL_RO 0
/* adrp/add addressing; CONFIG_PIC 0 emits ldr =sym (ABS64), which -pie rejects */
#define CONFIG_PIC 1
#define HAVE_AS_ARCHEXT_CRC_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_DOTPROD_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_I8MM_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SVE_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SVE2_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SME_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SME_I16I64_DIRECTIVE 0
#define HAVE_AS_ARCHEXT_SME2_DIRECTIVE 0
#define HAVE_SME 0

#endif
