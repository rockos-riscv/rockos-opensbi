/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2019 Western Digital Corporation or its affiliates.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_asm.h>
#include <sbi/riscv_barrier.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_fp.h>
#include <sbi/sbi_bitops.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_csr_detect.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_math.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_trap.h>
#include <sbi/riscv_io.h>

extern void __sbi_expected_trap(void);
extern void __sbi_expected_trap_hext(void);

void (*sbi_hart_expected_trap)(void) = &__sbi_expected_trap;

struct hart_features {
	unsigned long features;
	unsigned int pmp_count;
	unsigned int pmp_addr_bits;
	unsigned long pmp_gran;
	unsigned int mhpm_count;
	unsigned int mhpm_bits;
};
static unsigned long hart_features_offset;

static void mstatus_init(struct sbi_scratch *scratch)
{
	unsigned long mstatus_val = 0;

	/* Enable FPU */
	if (misa_extension('D') || misa_extension('F'))
		mstatus_val |=  MSTATUS_FS;

	/* Enable Vector context */
	if (misa_extension('V'))
		mstatus_val |=  MSTATUS_VS;

	csr_write(CSR_MSTATUS, mstatus_val);

	/* Disable user mode usage of all perf counters except default ones (CY, TM, IR) */
	if (misa_extension('S') &&
	    sbi_hart_has_feature(scratch, SBI_HART_HAS_SCOUNTEREN))
		csr_write(CSR_SCOUNTEREN, 7);

	/**
	 * OpenSBI doesn't use any PMU counters in M-mode.
	 * Supervisor mode usage for all counters are enabled by default
	 * But counters will not run until mcountinhibit is set.
	 */
	if (sbi_hart_has_feature(scratch, SBI_HART_HAS_MCOUNTEREN))
		csr_write(CSR_MCOUNTEREN, -1);

	/* All programmable counters will start running at runtime after S-mode request */
	if (sbi_hart_has_feature(scratch, SBI_HART_HAS_MCOUNTINHIBIT))
		csr_write(CSR_MCOUNTINHIBIT, 0xFFFFFFF8);

	/* Disable all interrupts */
	csr_write(CSR_MIE, 0);

	/* Disable S-mode paging */
	if (misa_extension('S'))
		csr_write(CSR_SATP, 0);
}

static int fp_init(struct sbi_scratch *scratch)
{
#ifdef __riscv_flen
	int i;
#endif

	if (!misa_extension('D') && !misa_extension('F'))
		return 0;

	if (!(csr_read(CSR_MSTATUS) & MSTATUS_FS))
		return SBI_EINVAL;

#ifdef __riscv_flen
	for (i = 0; i < 32; i++)
		init_fp_reg(i);
	csr_write(CSR_FCSR, 0);
#endif

	return 0;
}

static int delegate_traps(struct sbi_scratch *scratch)
{
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	unsigned long interrupts, exceptions;

	if (!misa_extension('S'))
		/* No delegation possible as mideleg does not exist */
		return 0;

	/* Send M-mode interrupts and most exceptions to S-mode */
	interrupts = MIP_SSIP | MIP_STIP | MIP_SEIP;
	if (sbi_hart_has_feature(scratch, SBI_HART_HAS_SSCOFPMF))
		interrupts |= MIP_LCOFIP;

	exceptions = (1U << CAUSE_MISALIGNED_FETCH) | (1U << CAUSE_BREAKPOINT) |
		     (1U << CAUSE_USER_ECALL);
	if (sbi_platform_has_mfaults_delegation(plat))
		exceptions |= (1U << CAUSE_FETCH_PAGE_FAULT) |
			      (1U << CAUSE_LOAD_PAGE_FAULT) |
			      (1U << CAUSE_STORE_PAGE_FAULT);

	/*
	 * If hypervisor extension available then we only handle hypervisor
	 * calls (i.e. ecalls from HS-mode) in M-mode.
	 *
	 * The HS-mode will additionally handle supervisor calls (i.e. ecalls
	 * from VS-mode), Guest page faults and Virtual interrupts.
	 */
	if (misa_extension('H')) {
		exceptions |= (1U << CAUSE_VIRTUAL_SUPERVISOR_ECALL);
		exceptions |= (1U << CAUSE_FETCH_GUEST_PAGE_FAULT);
		exceptions |= (1U << CAUSE_LOAD_GUEST_PAGE_FAULT);
		exceptions |= (1U << CAUSE_VIRTUAL_INST_FAULT);
		exceptions |= (1U << CAUSE_STORE_GUEST_PAGE_FAULT);
	}

	csr_write(CSR_MIDELEG, interrupts);
	csr_write(CSR_MEDELEG, exceptions);

	return 0;
}

void sbi_hart_delegation_dump(struct sbi_scratch *scratch,
			      const char *prefix, const char *suffix)
{
	if (!misa_extension('S'))
		/* No delegation possible as mideleg does not exist*/
		return;

	sbi_printf("%sMIDELEG%s: 0x%" PRILX "\n",
		   prefix, suffix, csr_read(CSR_MIDELEG));
	sbi_printf("%sMEDELEG%s: 0x%" PRILX "\n",
		   prefix, suffix, csr_read(CSR_MEDELEG));
}

unsigned int sbi_hart_mhpm_count(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->mhpm_count;
}

unsigned int sbi_hart_pmp_count(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->pmp_count;
}

unsigned long sbi_hart_pmp_granularity(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->pmp_gran;
}

unsigned int sbi_hart_pmp_addrbits(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->pmp_addr_bits;
}

unsigned int sbi_hart_mhpm_bits(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->mhpm_bits;
}

int pmp_set_tor(unsigned int n, unsigned long prot, unsigned long addr_start, unsigned long addr_end)
{
	/* PMP addresses are 4-byte aligned, drop the bottom two bits */
	unsigned long protected_start = ((size_t) addr_start)>>2;
	unsigned long protected_end = ((size_t) addr_end)>>2;
	unsigned long cfgmask = 0xffff, pmpcfg;
	int pmpcfg_csr, pmpcfg_shift, pmpaddr_csr;
	#define NAPOT_SIZE 4096
	/* Clear the bit corresponding with alignment */
	protected_start &= ~(NAPOT_SIZE >> 3);
	protected_end &= ~(NAPOT_SIZE >> 3);
	
	/* start region */
#if __riscv_xlen == 32
	pmpcfg_csr   = CSR_PMPCFG0 + (n >> 2);
	pmpcfg_shift = (n & 3) << 3;
#elif __riscv_xlen == 64
	pmpcfg_csr   = (CSR_PMPCFG0 + (n >> 2)) & ~1;
	pmpcfg_shift = (n & 7) << 3;
#else
	return SBI_ENOTSUPP;
#endif	
	pmpaddr_csr = CSR_PMPADDR0 + n;

	/* encode PMP config */
	prot &= ~PMP_A;
	cfgmask = ~(0xffUL << pmpcfg_shift);
	pmpcfg	= (csr_read_num(pmpcfg_csr) & cfgmask);
	pmpcfg |= ((prot << pmpcfg_shift) & ~cfgmask);

	/* write csrs */
	csr_write_num(pmpaddr_csr, protected_start);
	csr_write_num(pmpcfg_csr, pmpcfg);

	/* end region */
	n++;	
#if __riscv_xlen == 32
	pmpcfg_csr   = CSR_PMPCFG0 + (n >> 2);
	pmpcfg_shift = (n & 3) << 3;
#elif __riscv_xlen == 64
	pmpcfg_csr   = (CSR_PMPCFG0 + (n >> 2)) & ~1;
	pmpcfg_shift = (n & 7) << 3;
#else
	return SBI_ENOTSUPP;
#endif	
	pmpaddr_csr = CSR_PMPADDR0 + n;

	/* encode PMP config */
	prot &= ~PMP_A;
	prot |= PMP_A_TOR;
	cfgmask = ~(0xffUL << pmpcfg_shift);
	pmpcfg	= (csr_read_num(pmpcfg_csr) & cfgmask);
	pmpcfg |= ((prot << pmpcfg_shift) & ~cfgmask);

	/* write csrs */
	csr_write_num(pmpaddr_csr, protected_end);
	csr_write_num(pmpcfg_csr, pmpcfg);

	// sbi_printf("\nsaddr:%lx eaddr:%lx  pmpcfg:%lx\n", addr_start,addr_end,pmpcfg);

	return 0;
}


int sbi_hart_pmp_configure(struct sbi_scratch *scratch)
{
	struct sbi_domain_memregion *reg;
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	unsigned int pmp_idx = 0, pmp_flags, pmp_bits, pmp_gran_log2;
	unsigned int pmp_count = sbi_hart_pmp_count(scratch);
	unsigned long pmp_addr = 0, pmp_addr_max = 0;

	if (!pmp_count)
		return 0;

	pmp_gran_log2 = log2roundup(sbi_hart_pmp_granularity(scratch));
	pmp_bits = sbi_hart_pmp_addrbits(scratch) - 1;
	pmp_addr_max = (1UL << pmp_bits) | ((1UL << pmp_bits) - 1);

	sbi_domain_for_each_memregion(dom, reg) {
		if (pmp_count <= pmp_idx)
			break;

		pmp_flags = 0;
		if (reg->flags & SBI_DOMAIN_MEMREGION_READABLE)
			pmp_flags |= PMP_R;
		if (reg->flags & SBI_DOMAIN_MEMREGION_WRITEABLE)
			pmp_flags |= PMP_W;
		if (reg->flags & SBI_DOMAIN_MEMREGION_EXECUTABLE)
			pmp_flags |= PMP_X;
		if (reg->flags & SBI_DOMAIN_MEMREGION_MMODE)
			pmp_flags |= PMP_L;

		if (!reg->tor) {
			pmp_addr =  reg->base >> PMP_SHIFT;
			if (pmp_gran_log2 <= reg->order && pmp_addr < pmp_addr_max)
				pmp_set(pmp_idx++, pmp_flags, reg->base, reg->order);
			else {
				sbi_printf("Can not configure pmp for domain %s", dom->name);
				sbi_printf(" because memory region address %lx or size %lx is not in range\n",
					reg->base, reg->order);
			}
		} else {
			pmp_addr =  reg->base;
			pmp_set_tor(pmp_idx, pmp_flags, reg->base, reg->base+reg->tor);
			pmp_idx+=2;
		}
		
	}

	return 0;
}

#ifndef BR2_CHIPLET_2
static void init_bus_blocker(void)
{
#if (defined BR2_CHIPLET_1) && (defined BR2_CHIPLET_1_DIE0_AVAILABLE)
	#define BLOCKER_TL64D2D_OUT	(void *)0x200000
	#define BLOCKER_TL256D2D_OUT	(void *)0x202000
	#define BLOCKER_TL256D2D_IN	(void *)0x204000
#elif (defined BR2_CHIPLET_1) && (defined BR2_CHIPLET_1_DIE1_AVAILABLE)
	#define BLOCKER_TL64D2D_OUT	(void *)(0x200000+0x20000000)
	#define BLOCKER_TL256D2D_OUT	(void *)(0x202000+0x20000000)
	#define BLOCKER_TL256D2D_IN	(void *)(0x204000+0x20000000)
#endif
	writel(1,BLOCKER_TL64D2D_OUT);
	writel(1,BLOCKER_TL256D2D_OUT);
	writel(1,BLOCKER_TL256D2D_IN);

}
#endif

static void init_fcsr(void)
{
	unsigned long hwpf;	// Hardware Prefetcher 0 : 0x104095C1BE241 | Hardware Prefetcher 1 : 0x929F
	
	hwpf = 0x104095C1BE241UL;
	__asm__ volatile("csrw 0x7c3 , %0" : : "r"(hwpf));
	hwpf = 0x929FUL;
        //cleanup fields
        hwpf &= (~(0x1f << 5)); //[9:5]  cleanup  hitCacheThrdL2
        hwpf &= (~(0x7  << 14)); //[16:14] cleanup numL2PFIssQEnt

        //set new value
        hwpf |= (0x1f << 5); //[9:5]    hitCacheThrdL2
        hwpf |= (0x7  << 14); //[16:14] numL2PFIssQEnt
	__asm__ volatile("csrw 0x7c4 , %0" : : "r"(hwpf));

	/* enable speculative icache refill */
	// __asm__ volatile("csrw 0x7c1 , x0" : :);
	// __asm__ volatile("csrw 0x7c2 , x0" : :);

}


void sbi_hart_blocker_fscr_configure(struct sbi_scratch *scratch)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();

	if (dom->boot_hartid == current_hartid()) {
		#ifndef BR2_CHIPLET_2
		/* if only one die, need config blocker to 
		generate fake response when access remote target */
		init_bus_blocker();
		#endif
	}

	init_fcsr();
}

/**
 * Check whether a particular hart feature is available
 *
 * @param scratch pointer to the HART scratch space
 * @param feature the feature to check
 * @returns true (feature available) or false (feature not available)
 */
bool sbi_hart_has_feature(struct sbi_scratch *scratch, unsigned long feature)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	if (hfeatures->features & feature)
		return true;
	else
		return false;
}

static unsigned long hart_get_features(struct sbi_scratch *scratch)
{
	struct hart_features *hfeatures =
			sbi_scratch_offset_ptr(scratch, hart_features_offset);

	return hfeatures->features;
}

static inline char *sbi_hart_feature_id2string(unsigned long feature)
{
	char *fstr = NULL;

	if (!feature)
		return NULL;

	switch (feature) {
	case SBI_HART_HAS_SCOUNTEREN:
		fstr = "scounteren";
		break;
	case SBI_HART_HAS_MCOUNTEREN:
		fstr = "mcounteren";
		break;
	case SBI_HART_HAS_MCOUNTINHIBIT:
		fstr = "mcountinhibit";
		break;
	case SBI_HART_HAS_SSCOFPMF:
		fstr = "sscofpmf";
		break;
	case SBI_HART_HAS_TIME:
		fstr = "time";
		break;
	default:
		break;
	}

	return fstr;
}

/**
 * Get the hart features in string format
 *
 * @param scratch pointer to the HART scratch space
 * @param features_str pointer to a char array where the features string will be
 *		       updated
 * @param nfstr length of the features_str. The feature string will be truncated
 *		if nfstr is not long enough.
 */
void sbi_hart_get_features_str(struct sbi_scratch *scratch,
			       char *features_str, int nfstr)
{
	unsigned long features, feat = 1UL;
	char *temp;
	int offset = 0;

	if (!features_str || nfstr <= 0)
		return;
	sbi_memset(features_str, 0, nfstr);

	features = hart_get_features(scratch);
	if (!features)
		goto done;

	do {
		if (features & feat) {
			temp = sbi_hart_feature_id2string(feat);
			if (temp) {
				sbi_snprintf(features_str + offset, nfstr,
					     "%s,", temp);
				offset = offset + sbi_strlen(temp) + 1;
			}
		}
		feat = feat << 1;
	} while (feat <= SBI_HART_HAS_LAST_FEATURE);

done:
	if (offset)
		features_str[offset - 1] = '\0';
	else
		sbi_strncpy(features_str, "none", nfstr);
}

static unsigned long hart_pmp_get_allowed_addr(void)
{
	unsigned long val = 0;
	struct sbi_trap_info trap = {0};

	csr_write_allowed(CSR_PMPCFG0, (ulong)&trap, 0);
	if (trap.cause)
		return 0;

	csr_write_allowed(CSR_PMPADDR0, (ulong)&trap, PMP_ADDR_MASK);
	if (!trap.cause) {
		val = csr_read_allowed(CSR_PMPADDR0, (ulong)&trap);
		if (trap.cause)
			val = 0;
	}

	return val;
}

static int hart_pmu_get_allowed_bits(void)
{
	unsigned long val = ~(0UL);
	struct sbi_trap_info trap = {0};
	int num_bits = 0;

	/**
	 * It is assumed that platforms will implement same number of bits for
	 * all the performance counters including mcycle/minstret.
	 */
	csr_write_allowed(CSR_MHPMCOUNTER3, (ulong)&trap, val);
	if (!trap.cause) {
		val = csr_read_allowed(CSR_MHPMCOUNTER3, (ulong)&trap);
		if (trap.cause)
			return 0;
	}
	num_bits = __fls(val) + 1;
#if __riscv_xlen == 32
	csr_write_allowed(CSR_MHPMCOUNTER3H, (ulong)&trap, val);
	if (!trap.cause) {
		val = csr_read_allowed(CSR_MHPMCOUNTER3H, (ulong)&trap);
		if (trap.cause)
			return num_bits;
	}
	num_bits += __fls(val) + 1;

#endif

	return num_bits;
}

static void hart_detect_features(struct sbi_scratch *scratch)
{
	struct sbi_trap_info trap = {0};
	struct hart_features *hfeatures;
	unsigned long val;

	/* Reset hart features */
	hfeatures = sbi_scratch_offset_ptr(scratch, hart_features_offset);
	hfeatures->features = 0;
	hfeatures->pmp_count = 0;
	hfeatures->mhpm_count = 0;

#define __check_csr(__csr, __rdonly, __wrval, __field, __skip)	\
	val = csr_read_allowed(__csr, (ulong)&trap);			\
	if (!trap.cause) {						\
		if (__rdonly) {						\
			(hfeatures->__field)++;				\
		} else {						\
			csr_write_allowed(__csr, (ulong)&trap, __wrval);\
			if (!trap.cause) {				\
				if (csr_swap(__csr, val) == __wrval)	\
					(hfeatures->__field)++;		\
				else					\
					goto __skip;			\
			} else {					\
				goto __skip;				\
			}						\
		}							\
	} else {							\
		goto __skip;						\
	}
#define __check_csr_2(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr(__csr + 1, __rdonly, __wrval, __field, __skip)
#define __check_csr_4(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_2(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_2(__csr + 2, __rdonly, __wrval, __field, __skip)
#define __check_csr_8(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_4(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_4(__csr + 4, __rdonly, __wrval, __field, __skip)
#define __check_csr_16(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_8(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_8(__csr + 8, __rdonly, __wrval, __field, __skip)
#define __check_csr_32(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_16(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_16(__csr + 16, __rdonly, __wrval, __field, __skip)
#define __check_csr_64(__csr, __rdonly, __wrval, __field, __skip)	\
	__check_csr_32(__csr + 0, __rdonly, __wrval, __field, __skip)	\
	__check_csr_32(__csr + 32, __rdonly, __wrval, __field, __skip)

	/**
	 * Detect the allowed address bits & granularity. At least PMPADDR0
	 * should be implemented.
	 */
	val = hart_pmp_get_allowed_addr();
	if (val) {
		hfeatures->pmp_gran =  1 << (__ffs(val) + 2);
		hfeatures->pmp_addr_bits = __fls(val) + 1;
		/* Detect number of PMP regions. At least PMPADDR0 should be implemented*/
		__check_csr_64(CSR_PMPADDR0, 0, val, pmp_count, __pmp_skip);
	}
__pmp_skip:

	/* Detect number of MHPM counters */
	__check_csr(CSR_MHPMCOUNTER3, 0, 1UL, mhpm_count, __mhpm_skip);
	hfeatures->mhpm_bits = hart_pmu_get_allowed_bits();

	__check_csr_4(CSR_MHPMCOUNTER4, 0, 1UL, mhpm_count, __mhpm_skip);
	__check_csr_8(CSR_MHPMCOUNTER8, 0, 1UL, mhpm_count, __mhpm_skip);
	__check_csr_16(CSR_MHPMCOUNTER16, 0, 1UL, mhpm_count, __mhpm_skip);

	/**
	 * No need to check for MHPMCOUNTERH for RV32 as they are expected to be
	 * implemented if MHPMCOUNTER is implemented.
	 */

__mhpm_skip:

#undef __check_csr_64
#undef __check_csr_32
#undef __check_csr_16
#undef __check_csr_8
#undef __check_csr_4
#undef __check_csr_2
#undef __check_csr

	/* Detect if hart supports SCOUNTEREN feature */
	val = csr_read_allowed(CSR_SCOUNTEREN, (unsigned long)&trap);
	if (!trap.cause) {
		csr_write_allowed(CSR_SCOUNTEREN, (unsigned long)&trap, val);
		if (!trap.cause)
			hfeatures->features |= SBI_HART_HAS_SCOUNTEREN;
	}

	/* Detect if hart supports MCOUNTEREN feature */
	val = csr_read_allowed(CSR_MCOUNTEREN, (unsigned long)&trap);
	if (!trap.cause) {
		csr_write_allowed(CSR_MCOUNTEREN, (unsigned long)&trap, val);
		if (!trap.cause)
			hfeatures->features |= SBI_HART_HAS_MCOUNTEREN;
	}

	/* Detect if hart supports MCOUNTINHIBIT feature */
	val = csr_read_allowed(CSR_MCOUNTINHIBIT, (unsigned long)&trap);
	if (!trap.cause) {
		csr_write_allowed(CSR_MCOUNTINHIBIT, (unsigned long)&trap, val);
		if (!trap.cause)
			hfeatures->features |= SBI_HART_HAS_MCOUNTINHIBIT;
	}

	/* Counter overflow/filtering is not useful without mcounter/inhibit */
	if (hfeatures->features & SBI_HART_HAS_MCOUNTINHIBIT &&
	    hfeatures->features & SBI_HART_HAS_MCOUNTEREN) {
		/* Detect if hart supports sscofpmf */
		csr_read_allowed(CSR_SCOUNTOVF, (unsigned long)&trap);
		if (!trap.cause)
			hfeatures->features |= SBI_HART_HAS_SSCOFPMF;
	}

	/* Detect if hart supports time CSR */
	csr_read_allowed(CSR_TIME, (unsigned long)&trap);
	if (!trap.cause)
		hfeatures->features |= SBI_HART_HAS_TIME;
}

int sbi_hart_reinit(struct sbi_scratch *scratch)
{
	int rc;

	mstatus_init(scratch);

	rc = fp_init(scratch);
	if (rc)
		return rc;

	rc = delegate_traps(scratch);
	if (rc)
		return rc;

	return 0;
}

int sbi_hart_init(struct sbi_scratch *scratch, bool cold_boot)
{
	if (cold_boot) {
		if (misa_extension('H'))
			sbi_hart_expected_trap = &__sbi_expected_trap_hext;

		hart_features_offset = sbi_scratch_alloc_offset(
						sizeof(struct hart_features));
		if (!hart_features_offset)
			return SBI_ENOMEM;
	}

	hart_detect_features(scratch);

	return sbi_hart_reinit(scratch);
}

void __attribute__((noreturn)) sbi_hart_hang(void)
{
	while (1)
		wfi();
	__builtin_unreachable();
}

void __attribute__((noreturn))
sbi_hart_switch_mode(unsigned long arg0, unsigned long arg1,
		     unsigned long next_addr, unsigned long next_mode,
		     bool next_virt)
{
#if __riscv_xlen == 32
	unsigned long val, valH;
#else
	unsigned long val;
#endif

	switch (next_mode) {
	case PRV_M:
		break;
	case PRV_S:
		if (!misa_extension('S'))
			sbi_hart_hang();
		break;
	case PRV_U:
		if (!misa_extension('U'))
			sbi_hart_hang();
		break;
	default:
		sbi_hart_hang();
	}

	val = csr_read(CSR_MSTATUS);
	val = INSERT_FIELD(val, MSTATUS_MPP, next_mode);
	val = INSERT_FIELD(val, MSTATUS_MPIE, 0);
#if __riscv_xlen == 32
	if (misa_extension('H')) {
		valH = csr_read(CSR_MSTATUSH);
		if (next_virt)
			valH = INSERT_FIELD(valH, MSTATUSH_MPV, 1);
		else
			valH = INSERT_FIELD(valH, MSTATUSH_MPV, 0);
		csr_write(CSR_MSTATUSH, valH);
	}
#else
	if (misa_extension('H')) {
		if (next_virt)
			val = INSERT_FIELD(val, MSTATUS_MPV, 1);
		else
			val = INSERT_FIELD(val, MSTATUS_MPV, 0);
	}
#endif
	csr_write(CSR_MSTATUS, val);
	csr_write(CSR_MEPC, next_addr);

	if (next_mode == PRV_S) {
		csr_write(CSR_STVEC, next_addr);
		csr_write(CSR_SSCRATCH, 0);
		csr_write(CSR_SIE, 0);
		csr_write(CSR_SATP, 0);
	} else if (next_mode == PRV_U) {
		if (misa_extension('N')) {
			csr_write(CSR_UTVEC, next_addr);
			csr_write(CSR_USCRATCH, 0);
			csr_write(CSR_UIE, 0);
		}
	}

	register unsigned long a0 asm("a0") = arg0;
	register unsigned long a1 asm("a1") = arg1;
	__asm__ __volatile__("mret" : : "r"(a0), "r"(a1));
	__builtin_unreachable();
}
