// SPDX-License-Identifier: GPL-2.0-only
#include <linux/kdebug.h>
#include <linux/kprobes.h>
#include <linux/preempt.h>
#include <asm/break.h>

static const union loongarch_instruction breakpoint_insn = {
	.reg0i15_format = {
		.opcode = break_op,
		.immediate = BRK_KPROBE_BP,
	}
};

static const union loongarch_instruction singlestep_insn = {
	.reg0i15_format = {
		.opcode = break_op,
		.immediate = BRK_KPROBE_SSTEPBP,
	}
};

DEFINE_PER_CPU(struct kprobe *, current_kprobe);
DEFINE_PER_CPU(struct kprobe_ctlblk, kprobe_ctlblk);

static bool insns_not_supported(union loongarch_instruction insn)
{
	switch (insn.reg2i14_format.opcode) {
	case llw_op:
	case lld_op:
	case scw_op:
	case scd_op:
		pr_notice("kprobe: ll and sc instructions are not supported\n");
		return true;
	}

	switch (insn.reg1i21_format.opcode) {
	case bceqz_op:
		pr_notice("kprobe: bceqz and bcnez instructions are not supported\n");
		return true;
	}

	return false;
}
NOKPROBE_SYMBOL(insns_not_supported);

int arch_prepare_kprobe(struct kprobe *p)
{
	union loongarch_instruction insn;

	insn = p->addr[0];
	if (insns_not_supported(insn))
		return -EINVAL;

	p->ainsn.insn = get_insn_slot();
	if (!p->ainsn.insn)
		return -ENOMEM;

	p->ainsn.insn[0] = *p->addr;
	p->ainsn.insn[1] = singlestep_insn;

	p->opcode = *p->addr;

	return 0;
}
NOKPROBE_SYMBOL(arch_prepare_kprobe);

/* Install breakpoint in text */
void arch_arm_kprobe(struct kprobe *p)
{
	*p->addr = breakpoint_insn;
	flush_insn_slot(p);
}
NOKPROBE_SYMBOL(arch_arm_kprobe);

/* Remove breakpoint from text */
void arch_disarm_kprobe(struct kprobe *p)
{
	*p->addr = p->opcode;
	flush_insn_slot(p);
}
NOKPROBE_SYMBOL(arch_disarm_kprobe);

void arch_remove_kprobe(struct kprobe *p)
{
	if (p->ainsn.insn) {
		free_insn_slot(p->ainsn.insn, 0);
		p->ainsn.insn = NULL;
	}
}
NOKPROBE_SYMBOL(arch_remove_kprobe);

static void save_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	kcb->prev_kprobe.kp = kprobe_running();
	kcb->prev_kprobe.status = kcb->kprobe_status;
	kcb->prev_kprobe.saved_irq = kcb->kprobe_saved_irq;
	kcb->prev_kprobe.saved_era = kcb->kprobe_saved_era;
}
NOKPROBE_SYMBOL(save_previous_kprobe);

static void restore_previous_kprobe(struct kprobe_ctlblk *kcb)
{
	__this_cpu_write(current_kprobe, kcb->prev_kprobe.kp);
	kcb->kprobe_status = kcb->prev_kprobe.status;
	kcb->kprobe_saved_irq = kcb->prev_kprobe.saved_irq;
	kcb->kprobe_saved_era = kcb->prev_kprobe.saved_era;
}
NOKPROBE_SYMBOL(restore_previous_kprobe);

static void set_current_kprobe(struct kprobe *p, struct pt_regs *regs,
			       struct kprobe_ctlblk *kcb)
{
	__this_cpu_write(current_kprobe, p);
	kcb->kprobe_saved_irq = regs->csr_prmd & CSR_PRMD_PIE;
	kcb->kprobe_saved_era = regs->csr_era;
}
NOKPROBE_SYMBOL(set_current_kprobe);

static bool insns_not_simulated(struct kprobe *p, struct pt_regs *regs)
{
	if (is_pc_ins(&p->opcode)) {
		simu_pc(regs, p->opcode);
		return false;
	}
	if (is_branch_ins(&p->opcode)) {
		simu_branch(regs, p->opcode);
		return false;
	}
	return true;
}
NOKPROBE_SYMBOL(insns_not_simulated);

static void setup_singlestep(struct kprobe *p, struct pt_regs *regs,
			     struct kprobe_ctlblk *kcb, int reenter)
{
	if (reenter) {
		save_previous_kprobe(kcb);
		set_current_kprobe(p, regs, kcb);
		kcb->kprobe_status = KPROBE_REENTER;
	} else {
		kcb->kprobe_status = KPROBE_HIT_SS;
	}

	regs->csr_prmd &= ~CSR_PRMD_PIE;

	if (p->ainsn.insn->word == breakpoint_insn.word) {
		regs->csr_prmd |= kcb->kprobe_saved_irq;
		preempt_enable_no_resched();
		return;
	}

	if (insns_not_simulated(p, regs)) {
		kcb->kprobe_status = KPROBE_HIT_SS;
		regs->csr_era = (unsigned long)&p->ainsn.insn[0];
	} else {
		kcb->kprobe_status = KPROBE_HIT_SSDONE;
		if (p->post_handler)
			p->post_handler(p, regs, 0);
		reset_current_kprobe();
		preempt_enable_no_resched();
	}
}
NOKPROBE_SYMBOL(setup_singlestep);

static bool reenter_kprobe(struct kprobe *p, struct pt_regs *regs,
			  struct kprobe_ctlblk *kcb)
{
	switch (kcb->kprobe_status) {
	case KPROBE_HIT_SSDONE:
	case KPROBE_HIT_ACTIVE:
		kprobes_inc_nmissed_count(p);
		setup_singlestep(p, regs, kcb, 1);
		break;
	case KPROBE_HIT_SS:
	case KPROBE_REENTER:
		pr_warn("Failed to recover from reentered kprobes.\n");
		dump_kprobe(p);
		BUG();
		break;
	default:
		WARN_ON(1);
		return false;
	}

	return true;
}
NOKPROBE_SYMBOL(reenter_kprobe);

bool kprobe_breakpoint_handler(struct pt_regs *regs)
{
	struct kprobe_ctlblk *kcb;
	struct kprobe *p, *cur_kprobe;
	kprobe_opcode_t *addr = (kprobe_opcode_t *)regs->csr_era;

	/*
	 * We don't want to be preempted for the entire
	 * duration of kprobe processing.
	 */
	preempt_disable();
	kcb = get_kprobe_ctlblk();
	cur_kprobe = kprobe_running();

	p = get_kprobe(addr);
	if (p) {
		if (cur_kprobe) {
			if (reenter_kprobe(p, regs, kcb))
				return true;
		} else {
			/* Probe hit */
			set_current_kprobe(p, regs, kcb);
			kcb->kprobe_status = KPROBE_HIT_ACTIVE;

			/*
			 * If we have no pre-handler or it returned 0, we
			 * continue with normal processing.  If we have a
			 * pre-handler and it returned non-zero, it will
			 * modify the execution path and no need to single
			 * stepping. Let's just reset current kprobe and exit.
			 *
			 * pre_handler can hit a breakpoint and can step thru
			 * before return.
			 */
			if (!p->pre_handler || !p->pre_handler(p, regs)) {
				setup_singlestep(p, regs, kcb, 0);
			} else {
				reset_current_kprobe();
				preempt_enable_no_resched();
			}
		}
		return true;
	}

	if (addr->word != breakpoint_insn.word) {
		/*
		 * The breakpoint instruction was removed right
		 * after we hit it.  Another cpu has removed
		 * either a probepoint or a debugger breakpoint
		 * at this address.  In either case, no further
		 * handling of this interrupt is appropriate.
		 * Return back to original instruction, and continue.
		 */
		preempt_enable_no_resched();
		return true;
	}

	preempt_enable_no_resched();
	return false;
}
NOKPROBE_SYMBOL(kprobe_breakpoint_handler);

bool kprobe_singlestep_handler(struct pt_regs *regs)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	if (!cur)
		return false;

	/* Restore back the original saved kprobes variables and continue */
	if (kcb->kprobe_status == KPROBE_REENTER) {
		restore_previous_kprobe(kcb);
		goto out;
	}

	/* Call post handler */
	if (cur->post_handler) {
		kcb->kprobe_status = KPROBE_HIT_SSDONE;
		cur->post_handler(cur, regs, 0);
	}

	regs->csr_era = kcb->kprobe_saved_era + LOONGARCH_INSN_SIZE;
	regs->csr_prmd |= kcb->kprobe_saved_irq;

	reset_current_kprobe();
out:
	preempt_enable_no_resched();
	return true;
}
NOKPROBE_SYMBOL(kprobe_singlestep_handler);

bool kprobe_fault_handler(struct pt_regs *regs, int trapnr)
{
	struct kprobe *cur = kprobe_running();
	struct kprobe_ctlblk *kcb = get_kprobe_ctlblk();

	switch (kcb->kprobe_status) {
	case KPROBE_HIT_SS:
	case KPROBE_REENTER:
		/*
		 * We are here because the instruction being single
		 * stepped caused a page fault. We reset the current
		 * kprobe and the ip points back to the probe address
		 * and allow the page fault handler to continue as a
		 * normal page fault.
		 */
		regs->csr_era = (unsigned long) cur->addr;
		BUG_ON(!instruction_pointer(regs));

		if (kcb->kprobe_status == KPROBE_REENTER) {
			restore_previous_kprobe(kcb);
		} else {
			regs->csr_prmd |= kcb->kprobe_saved_irq;
			reset_current_kprobe();
		}
		preempt_enable_no_resched();
		break;
	case KPROBE_HIT_ACTIVE:
	case KPROBE_HIT_SSDONE:
		/*
		 * In case the user-specified fault handler returned
		 * zero, try to fix up.
		 */
		if (fixup_exception(regs))
			return true;

		/*
		 * If fixup_exception() could not handle it,
		 * let do_page_fault() fix it.
		 */
		break;
	default:
		break;
	}
	return false;
}
NOKPROBE_SYMBOL(kprobe_fault_handler);

/*
 * Provide a blacklist of symbols identifying ranges which cannot be kprobed.
 * This blacklist is exposed to userspace via debugfs (kprobes/blacklist).
 */
int __init arch_populate_kprobe_blacklist(void)
{
	return kprobe_add_area_blacklist((unsigned long)__irqentry_text_start,
					 (unsigned long)__irqentry_text_end);
}

/* Called from __kretprobe_trampoline */
void __used *trampoline_probe_handler(struct pt_regs *regs)
{
	return (void *)kretprobe_trampoline_handler(regs, NULL);
}
NOKPROBE_SYMBOL(trampoline_probe_handler);

void arch_prepare_kretprobe(struct kretprobe_instance *ri,
			    struct pt_regs *regs)
{
	ri->ret_addr = (kprobe_opcode_t *)regs->regs[1];
	ri->fp = NULL;

	/* Replace the return addr with trampoline addr */
	regs->regs[1] = (unsigned long)&__kretprobe_trampoline;
}
NOKPROBE_SYMBOL(arch_prepare_kretprobe);

int arch_trampoline_kprobe(struct kprobe *p)
{
	return 0;
}
NOKPROBE_SYMBOL(arch_trampoline_kprobe);

int __init arch_init_kprobes(void)
{
	return 0;
}
