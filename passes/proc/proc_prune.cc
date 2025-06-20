/*
 *  yosys -- Yosys Open SYnthesis Suite
 *
 *  Copyright (C) 2019  whitequark <whitequark@whitequark.org>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "kernel/register.h"
#include "kernel/sigtools.h"
#include "kernel/log.h"
#include <stdlib.h>
#include <stdio.h>

USING_YOSYS_NAMESPACE
PRIVATE_NAMESPACE_BEGIN

struct PruneWorker
{
	RTLIL::Module *module;
	SigMap sigmap;

	int removed_count = 0, promoted_count = 0;

	PruneWorker(RTLIL::Module *mod) : module(mod), sigmap(mod) {}

	pool<RTLIL::SigBit> do_switch(RTLIL::SwitchRule *sw, pool<RTLIL::SigBit> assigned, pool<RTLIL::SigBit> &affected)
	{
		pool<RTLIL::SigBit> all_assigned;
		bool full_case = sw->get_bool_attribute(ID::full_case);
		bool first = true;
		for (auto it : sw->cases) {
			if (it->compare.empty())
				full_case = true;
			pool<RTLIL::SigBit> case_assigned = do_case(it, assigned, affected);
			if (first) {
				first = false;
				all_assigned = case_assigned;
			} else {
				for (auto &bit : all_assigned)
					if (!case_assigned[bit])
						all_assigned.erase(bit);
			}
		}
		if (full_case)
			assigned.insert(all_assigned.begin(), all_assigned.end());
		return assigned;
	}

	pool<RTLIL::SigBit> do_case(RTLIL::CaseRule *cs, pool<RTLIL::SigBit> assigned, pool<RTLIL::SigBit> &affected,
	                            bool root = false)
	{
		for (auto it = cs->switches.rbegin(); it != cs->switches.rend(); ++it) {
			pool<RTLIL::SigBit> sw_assigned = do_switch((*it), assigned, affected);
			assigned.insert(sw_assigned.begin(), sw_assigned.end());
		}
		for (auto it = cs->actions.rbegin(); it != cs->actions.rend(); ) {
			RTLIL::SigSpec lhs = sigmap(it->first);
			RTLIL::SigSpec rhs = sigmap(it->second);
			SigSpec new_lhs, new_rhs;
			SigSpec conn_lhs, conn_rhs;
			for (int i = 0; i < GetSize(lhs); i++) {
				SigBit bit = lhs[i];
				if (bit.wire && !assigned[bit]) {
					if (!affected[bit] && root) {
						conn_lhs.append(bit);
						conn_rhs.append(rhs[i]);
					} else {
						new_lhs.append(bit);
						new_rhs.append(rhs[i]);
					}
					assigned.insert(bit);
					affected.insert(bit);
				}
			}
			if (GetSize(conn_lhs)) {
				promoted_count++;
				module->connect(conn_lhs, conn_rhs);
			}
			if (GetSize(new_lhs) == 0) {
				if (GetSize(conn_lhs) == 0)
					removed_count++;
				it = decltype(cs->actions)::reverse_iterator(cs->actions.erase(it.base() - 1));
			} else {
				it->first = new_lhs;
				it->second = new_rhs;
				it++;
			}
		}
		return assigned;
	}

	void do_process(RTLIL::Process *pr)
	{
		pool<RTLIL::SigBit> affected;
		do_case(&pr->root_case, {}, affected, /*root=*/true);
	}
};

struct ProcPrunePass : public Pass {
	ProcPrunePass() : Pass("proc_prune", "remove redundant assignments") { }
	void help() override
	{
		//   |---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|---v---|
		log("\n");
		log("    proc_prune [selection]\n");
		log("\n");
		log("This pass identifies assignments in processes that are always overwritten by\n");
		log("a later assignment to the same signal and removes them.\n");
		log("\n");
	}
	void execute(std::vector<std::string> args, RTLIL::Design *design) override
	{
		int total_removed_count = 0, total_promoted_count = 0;
		log_header(design, "Executing PROC_PRUNE pass (remove redundant assignments in processes).\n");

		extra_args(args, 1, design);

		for (auto mod : design->all_selected_modules()) {
			PruneWorker worker(mod);
			for (auto proc : mod->selected_processes())
				worker.do_process(proc);
			total_removed_count += worker.removed_count;
			total_promoted_count += worker.promoted_count;
		}

		log("Removed %d redundant assignment%s.\n",
		    total_removed_count, total_removed_count == 1 ? "" : "s");
		log("Promoted %d assignment%s to connection%s.\n",
		    total_promoted_count, total_promoted_count == 1 ? "" : "s", total_promoted_count == 1 ? "" : "s");
	}
} ProcPrunePass;

PRIVATE_NAMESPACE_END
