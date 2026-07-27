#include "uni_scenarios/uni_scenario_tx_history_ec.h"
#include "nvmeib_jdr.h"
#include "uni_framework/range_algorithms.h"
#include "common/nvmeib_macro_magic.h"

void jdr_write_nvmeib_lock_id(struct jdr* jdr, char const * name, union nvmeib_lock_id const * lock)
{
	jdr_object_scope(jdr, name);
	
	__auto_type bits = &lock->bits;
	jdr_write_bitfield(jdr, bits, idx_in_praid);
	jdr_write_bitfield(jdr, bits, lock_id);
	jdr_write_bitfield(jdr, bits, is_stale);
	jdr_write_bitfield(jdr, bits, is_read);
}

void jdr_write_io_perms_bitfield(struct jdr* jdr, char const * name, union io_perms_bitfield const * io_perms)
{
	jdr_object_scope(jdr, name);
	__auto_type bits = &io_perms->bits;
	jdr_write_bitfield(jdr, bits, is_io_R);
	jdr_write_bitfield(jdr, bits, is_io_W);
	jdr_write_bitfield(jdr, bits, is_cold_recovery);
	jdr_write_bitfield(jdr, bits, is_hot_recovery);
	jdr_write_bitfield(jdr, bits, is_jgc_recovery);
}

void jdr_write_nvmeib_blkset_info(struct jdr* jdr, char const * name, union nvmeib_blkset_info const * binfo)
{
	char dirty[32] = {0};
	jdr_object_scope(jdr, name);

	__auto_type bits = &binfo->bits;
	nvmeibc_dbits_entry_to_str(dirty, sizeof(dirty), binfo->all);

	jdr_write_bitfield(jdr, bits, txid);
	jdr_write_var(jdr, dirty , (char const*)dirty);
}

void jdr_write_nvmeib_lock_blkset_entry(struct jdr* jdr, char const * name, union nvmeib_lock_blkset_entry const * blkset_entry)
{
	jdr_object_scope(jdr, name);
	jdr_write_nvmeib_lock_id(jdr, "lock_id", &blkset_entry->lock_id);
	jdr_write_nvmeib_blkset_info(jdr, "lock_id", &blkset_entry->blkset_info);
}


void jdr_write_nvmeibc_roles_bmps(struct jdr* jdr, char const * name, struct nvmeibc_roles_bmps const *rbmps)
{
	jdr_object_scope(jdr, name);
	{
		__auto_type raid = &rbmps->raid;
		jdr_write_bitmap(jdr, raid, data);
		jdr_write_bitmap(jdr, raid, pari);
		jdr_write_bitmap(jdr, raid, all);
	}

	jdr_write_bitmap(jdr, rbmps, data_sgmnts);
	jdr_write_bitmap(jdr, rbmps, pari_sgmnts);

	jdr_write_bitmap(jdr, rbmps, rw);
	jdr_write_bitmap(jdr, rbmps, wp);
	jdr_write_bitmap(jdr, rbmps, w);
	jdr_write_bitmap(jdr, rbmps, wm);
	jdr_write_bitmap(jdr, rbmps, dead);
	jdr_write_bitmap(jdr, rbmps, readable);
	jdr_write_bitmap(jdr, rbmps, readable_sync);

	jdr_write_bitmap(jdr, rbmps, dbits_off_mask);
	jdr_write_bitmap(jdr, rbmps, dbits_on_mask);

	jdr_write_bitmap(jdr, rbmps, local_access);

	jdr_write_bitfield(jdr, rbmps, has_protection);
	jdr_write_bitfield(jdr, rbmps, has_rw_pari);
	jdr_write_bitfield(jdr, rbmps, has_writable_pari);
}
										
void jdr_write_ec_recov_tx(struct jdr* jdr, char const * name, struct t_ec_recov_tx const* rtx)
{
	jdr_object_scope(jdr, name);
	{//input{{{
		__auto_type input = &rtx->inp;
		jdr_object_scope(jdr, STRINGIFY(input));

		jdr_write_var(jdr, sraid, (char const*)"todo!!!");
		jdr_write(jdr, input, tx_height);
		jdr_write(jdr, input, slba);
		{//pre{{{
			__auto_type pre = &input->pre;
			jdr_object_scope(jdr, STRINGIFY(pre));

			jdr_write(jdr, pre, txid);
			jdr_write_bitmap(jdr, pre, history_ram_dbits);
			jdr_write_fundamental_s_array(jdr, "is_never_written", pre->is_never_written);
			jdr_write(jdr, pre, is_never_written_union);
			jdr_write_fundamental_s_array(jdr, "is_parity_explicitly_marked_neverwritten", pre->is_parity_explicitly_marked_neverwritten);
			jdr_write(jdr, pre, is_parity_explicitly_marked_neverwritten_union);
		}//}}}

		{//ree{{{
			__auto_type ree = &input->ree;
			jdr_object_scope(jdr, STRINGIFY(ree));

			jdr_write_optional(jdr, ree, uuid);
			jdr_write_bitmap_s_array(jdr, "tx_bm", ree->tx_bm);
			jdr_write_bitmap(jdr, ree, tx_bm_union);
			jdr_write(jdr, ree, txid);
			jdr_write_bitfield(jdr, ree, is_topo_not_eq_to_rer);
			jdr_write_bitfield(jdr, ree, inject_allien_lock_id);
			jdr_write_bitfield(jdr, ree, is_old_completed);
			jdr_write_bitfield(jdr, ree, is_garbage_for_jgc);
		}//}}}

		{//nat {{{
			__auto_type natural_disaster = &input->nat;
			jdr_object_scope(jdr, STRINGIFY(natural_disaster));

			jdr_write_bitfield(jdr, natural_disaster, allow_data_loss);
			jdr_write_bitfield(jdr, natural_disaster, allow_rer_network_fail);
			jdr_write_bitfield(jdr, natural_disaster, is_journal_overriden);
			jdr_write_bitfield(jdr, natural_disaster, allow_bad_sectors);
			jdr_write_bitfield(jdr, natural_disaster, destory_slice);
		}//}}}

		{//rer{{{
			__auto_type rer = &input->rer;
			jdr_object_scope(jdr, STRINGIFY(rer));
			jdr_write_var(jdr, rer, (char const *)nvmeib_block_io_op_str(rer->bio_type));
			jdr_write(jdr, rer, tx_height);
			jdr_write_bitmap_s_array(jdr, "tx_bm", rer->tx_bm);
			
			{
				jdr_array_scope(jdr, STRINGIFY(topo));
				array_foreach(mode, rer->topo){
					jdr->ops.ascii(jdr, NULL, nvmeibt_client_topo_seg_access_mode_to_str(*mode)); 
				}
			}

			jdr_write_io_perms_bitfield(jdr, "io_perm", &rer->io_perm);

		}//}}}
	}//}}}

	{//pre{{{
		__auto_type pre_tx_params = &rtx->pre;
		jdr_object_scope(jdr, STRINGIFY(pre_tx_params));
		jdr_write_bitmap(jdr, pre_tx_params, ram_dbits);
		jdr_write_bitmap(jdr, pre_tx_params, ram_dconv);
		jdr_write_bitmap_s_array(jdr, "slice_dbits", pre_tx_params->slice_dbits);
		jdr_write_bitmap(jdr, pre_tx_params, slice_dbits_union);
	}//}}}
	
	{//traits{{{
		jdr_object_scope(jdr, "traits");
		{//ree_bmps{{{
			__auto_type ree_bmp = &rtx->ree_bmp;
			jdr_object_scope(jdr, STRINGIFY(ree_bmp));

			jdr_write_nvmeibc_roles_bmps(jdr, "topo", &ree_bmp->topo);
			jdr_write_bitmap_s_array(jdr, "jmd_writn", ree_bmp->jmd_writn);
			jdr_write_bitmap(jdr, ree_bmp, jmd_writn_union);
			jdr_write_bitmap_s_array(jdr, "dmd_writn", ree_bmp->dmd_writn);
			jdr_write_bitmap_s_array(jdr, "jmd_with_correct_j2d", ree_bmp->jmd_with_correct_j2d);
			jdr_write_bitmap(jdr, ree_bmp, jmd_with_correct_j2d_union);
			jdr_write_bitmap(jdr, ree_bmp, jmd_with_valid_chain);
			jdr_write_bitmap(jdr, ree_bmp, jmd_with_fake_chain);
			jdr_write_fundamental_s_array(jdr, "chain_height", ree_bmp->chain_height);
			jdr_write_bitmap(jdr, ree_bmp, abandoned_jent);
			jdr_write_bitfield(jdr, ree_bmp, is_jour_committed);
			jdr_write_bitmap(jdr, ree_bmp, ram_txid_no_wr);
			jdr_write_bitmap(jdr, ree_bmp, ram_dbits_no_wr);
		}//}}}		
		{//rer_bmp{{{
			__auto_type rer_bmp = &rtx->rer_bmp;
			jdr_object_scope(jdr, STRINGIFY(rer_bmp));

			jdr_write_nvmeibc_roles_bmps(jdr, "topo", &rer_bmp->topo);
			jdr_write_bitmap_s_array(jdr, "bad_sec_bmp", rer_bmp->bad_sec_bmp);
			jdr_write_bitmap_s_array(jdr, "jmd_ready", rer_bmp->jmd_ready);
			jdr_write_bitmap(jdr, rer_bmp, jmd_ready_union);
			jdr_write_bitmap_s_array(jdr, "jmd_kosher", rer_bmp->jmd_kosher);
			jdr_write_bitmap(jdr, rer_bmp, jmd_kosher_intersect);
			jdr_write_bitmap(jdr, rer_bmp, jmd_kosher_union);
			jdr_write_bitmap_s_array(jdr, "dmd_kosher", rer_bmp->dmd_kosher);
			jdr_write_bitfield(jdr, rer_bmp, any_dmd_kosher);
			jdr_write_bitmap_s_array(jdr, "roll_fwd", rer_bmp->roll_fwd);
			jdr_write_bitfield(jdr, rer_bmp, any_roll_fwd);
			jdr_write_bitmap_s_array(jdr, "roll_fwd_by_dbits_turnon", rer_bmp->roll_fwd_by_dbits_turnon);
			jdr_write_bitmap_s_array(jdr, "regen_fwd", rer_bmp->regen_fwd);
			jdr_write_bitfield(jdr, rer_bmp, can_change_slice_dbits);
			jdr_write_bitmap(jdr, rer_bmp, send_blkst_recov);
			jdr_write_bitfield(jdr, rer_bmp, is_jour_committed);
			jdr_write_bitfield(jdr, rer_bmp, does_know_txbm);
			jdr_write_bitfield(jdr, rer_bmp, can_change_ram_dbits);
			jdr_write_bitfield(jdr, rer_bmp, can_turnof_ram_dbits);
			jdr_write_bitfield(jdr, rer_bmp, can_turnon_ram_dbits);
			jdr_write_bitfield(jdr, rer_bmp, can_turnon_ram_dbist_on_w_segs);
			jdr_write_fundamental_s_array(jdr, "can_see_ree_dbits_in_slice", rer_bmp->can_see_ree_dbits_in_slice);
			jdr_write_bitfield(jdr, rer_bmp, can_see_all_ree_dbits_in_slice);
			jdr_write_bitfield(jdr, rer_bmp, can_see_any_ree_dbits_in_slice);
			jdr_write_fundamental_s_array(jdr, "ree_dbits_turnon_on_slice", rer_bmp->ree_dbits_turnon_on_slice);
			jdr_write_bitfield(jdr, rer_bmp, can_see_ree_txid_in_ram);
			jdr_write_bitfield(jdr, rer_bmp, can_see_any_ree_dbits_in_ram);
			jdr_write_bitfield(jdr, rer_bmp, can_see_all_ree_dbits_in_ram);
			jdr_write_bitfield(jdr, rer_bmp, can_see_ree_dbits);
			jdr_write_bitmap(jdr, rer_bmp, ree_ram_dbits_turnon_and_can_be_turnof_by_rer);

			{//whole{{{
				__auto_type whole = &rer_bmp->whole;
				jdr_object_scope(jdr, STRINGIFY(whole));

				jdr_write_bitmap_s_array(jdr, "regen_bkw", whole->regen_bkw);
				jdr_write_bitfield(jdr, whole, any_regen_bkw);
				jdr_write_bitmap_s_array(jdr, "roll_bkw", whole->roll_bkw);
				jdr_write_bitfield(jdr, whole, any_roll_bkw);
				jdr_write_bitmap_s_array(jdr, "regen", whole->regen);
				jdr_write_bitfield(jdr, whole, any_regen);
				jdr_write_fundamental_s_array(jdr, "will_rollfwd", whole->will_rollfwd);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_slice_by_data_blocks", whole->is_neverwritten_slice_by_data_blocks);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_readable_parity", whole->is_neverwritten_readable_parity);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_slice", whole->is_neverwritten_slice);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_source_parity_for_regen", whole->is_neverwritten_source_parity_for_regen);
				jdr_write_bitmap_s_array(jdr, "slice_dbits_rebuild", whole->slice_dbits_rebuild);
				jdr_write_bitmap_s_array(jdr, "slice_dbits_turnon", whole->slice_dbits_turnon);
				jdr_write_bitmap_s_array(jdr, "slice_dbits_change", whole->slice_dbits_change);
			}//}}}
			
			{//nwhole{{{
				__auto_type nwhole = &rer_bmp->nwhole;
				jdr_object_scope(jdr, STRINGIFY(nwhole));
				jdr_write_bitfield(jdr, nwhole, will_call_nwhole_sync);
				jdr_write_bitmap(jdr, nwhole, ram_dbits_turnof);
				jdr_write_bitmap(jdr, nwhole, ram_dbits_turnon_on_turnoff);
				jdr_write_bitmap(jdr, nwhole, regen);
				jdr_write_bitmap(jdr, nwhole, roll_bkw);
				jdr_write_bitmap(jdr, nwhole, regen_bkw);
				jdr_write_bitmap(jdr, nwhole, syn_blkst_data_change);
				jdr_write_bitmap(jdr, nwhole, syn_blkst_dmd_change);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_source_parity_for_regen", nwhole->is_neverwritten_source_parity_for_regen);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_slice", nwhole->is_neverwritten_slice);
			}//}}}
			
			{//total{{{
				__auto_type total_holes = &rer_bmp->total;
				jdr_object_scope(jdr, STRINGIFY(total));

				jdr_write_bitmap_s_array(jdr, "slice_dbits_rebuild", total_holes->slice_dbits_rebuild);
				jdr_write_bitmap_s_array(jdr, "slice_dbits_turnon", total_holes->slice_dbits_turnon);
				jdr_write_bitmap_s_array(jdr, "slice_dbits_change", total_holes->slice_dbits_change);
				jdr_write_bitmap_s_array(jdr, "slice_data_touched", total_holes->slice_data_touched);
				jdr_write_fundamental_s_array(jdr, "max_txid_in_slice", total_holes->max_txid_in_slice);
			}//}}}

			{//post_recov_tx{{{
				__auto_type post_recov_tx = &rer_bmp->tx;
				jdr_object_scope(jdr, STRINGIFY(post_recov_tx));

				jdr_write_bitfield(jdr, post_recov_tx, will_call_nwhole_sync);
				jdr_write_fundamental_s_array(jdr, "syn_blkst_data_change", post_recov_tx->syn_blkst_data_change);
				jdr_write_fundamental_s_array(jdr, "syn_blkst_dmd_change", post_recov_tx->syn_blkst_dmd_change);
				jdr_write_fundamental_s_array(jdr, "fixed_bad_sec", post_recov_tx->fixed_bad_sec);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_readable_parity", post_recov_tx->is_neverwritten_readable_parity);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_source_parity_for_regen", post_recov_tx->is_neverwritten_source_parity_for_regen);
				jdr_write_fundamental_s_array(jdr, "is_neverwritten_slice", post_recov_tx->is_neverwritten_slice);
			}//}}}
		}//}}}

		{//blkset_entries
			__auto_type blkst_entries = &rtx->lid;
			jdr_object_scope(jdr, STRINGIFY(blkst_entries));

			jdr_write_nvmeib_lock_blkset_entry(jdr, "pre", &blkst_entries->pre);
			jdr_write_nvmeib_lock_blkset_entry(jdr, "ree", &blkst_entries->ree);
			jdr_write_nvmeib_lock_blkset_entry(jdr, "nat", &blkst_entries->nat);
			jdr_write_nvmeib_lock_blkset_entry(jdr, "post_recov", &blkst_entries->post_recov);
			jdr_write_nvmeib_lock_blkset_entry(jdr, "rer", &blkst_entries->rer);
		}

		jdr_write_fundamental_s_array(jdr, "jour_offset", rtx->jour_offset);
		jdr_write_var(jdr, jtx, (char const*)"todo!!!");
		jdr_write_var(jdr, tpd, (char const*)"todo!!!");

		{//flow 
			__auto_type flow = &rtx->flow;
			jdr_object_scope(jdr, STRINGIFY(flow));

			jdr_write_bitmap(jdr, flow, any_j2d);
			jdr_write_bitfield(jdr, flow, htr_process_me);
			jdr_write_bitmap(jdr, flow, jgc_free_me);
		}//}}}
	}//}}}
}

void jdr_write_ec_recov_blkset(struct jdr* jdr, char const * name, struct t_ec_recov_blkset const* rblkset)
{
	jdr_object_scope(jdr, name);
	jdr_write_s_array(jdr, "tx", rblkset->tx, jdr_write_ec_recov_tx);
	jdr_write_bitfield(jdr, rblkset, will_htr_sync_called_on_blockset);
	jdr_write_bitfield(jdr, rblkset, will_nwhole_sync_called_on_blockset);

	{//resolve_dbits{{{
		__auto_type resolve_dbits = &rblkset->resolve_dbits;
		jdr_object_scope(jdr, STRINGIFY(resolve_dbits));
		jdr_write_bitfield(jdr, resolve_dbits, turnon_all_deg_segs);
		jdr_write_bitmap(jdr, resolve_dbits, after_resolve_dbits);
	}//}}}

	{//no_write_hole{{{
		__auto_type no_write_hole = &rblkset->nwhole;
		jdr_object_scope(jdr, STRINGIFY(no_write_hole));
		jdr_write_bitmap(jdr, no_write_hole, turnoff_dbits_bmp);
		jdr_write_bitmap(jdr, no_write_hole, regen_bkw_bmp);
		jdr_write_bitfield(jdr, no_write_hole, will_turnoff_dbits);
		jdr_write_bitfield(jdr, no_write_hole, will_turnon_dbits);
		jdr_write_bitfield(jdr, no_write_hole, is_sl_by_sl);
		jdr_write_bitmap(jdr, no_write_hole, bad_sec_bmp);
		jdr_write_bitmap(jdr, no_write_hole, ram_dbits_turnon_on_turnoff);
	}//}}}

	{//lock_id{{{
		__auto_type lid = &rblkset->lid;
		jdr_object_scope(jdr, STRINGIFY(lid));
		jdr_write_nvmeib_lock_blkset_entry(jdr, "post_recov", &lid->post_recov);
	}//}}}

	{//last_tx{{{
		__auto_type last_tx = &rblkset->last_tx;
		jdr_object_scope(jdr, STRINGIFY(last_tx));
		jdr_write(jdr, last_tx, is_wraparound);
	}//}}}
	//
	jdr_write_bitfield(jdr, rblkset, is_unknown_txid_injected_to_ram);
	jdr_write_bitfield(jdr, rblkset, is_unknown_dbits_injected_to_ram);
	jdr_write_bitfield(jdr, rblkset, is_double_deg_and_deg_parity);
}

void jdr_write_ec_tx_history(struct jdr* jdr, char const * name, struct t_ec_tx_history const * hist)
{
	jdr_object_scope(jdr, name);
	jdr_write(jdr, hist, msn);
	{//iter{{{
		__auto_type recov_iteration = &hist->iter;
		jdr_object_scope(jdr, STRINGIFY(recov_iteration));
		jdr_write(jdr, recov_iteration, curr_permutation);
		jdr_write(jdr, recov_iteration, iner_iteration);
		jdr_write(jdr, recov_iteration, n_recov_retries);
	}//}}}
	{//recov_properties{{{
		__auto_type recov_properties = &(hist->prop);

		jdr_object_scope(jdr, STRINGIFY(recov_properties));
		jdr_write(jdr, recov_properties, is_trans_errors_inject_enabled);
		jdr_write_bitfield(jdr, recov_properties, n_tx_in_test);
		jdr_write_bitfield(jdr, recov_properties, n_blksets);
		jdr_write_bitfield(jdr, recov_properties, n_tx_in_blkset);

		{
			__auto_type recov_segments = &(recov_properties->rec);
			jdr_object_scope(jdr, STRINGIFY(recov_segments));
			
			jdr_write_var(jdr, type, nvmeibt_recov_type_to_3str(recov_segments->type));
			jdr_write_bitfield(jdr, recov_segments, seg_start);
			jdr_write_bitfield(jdr, recov_segments, seg_end);
		}
	}//}}}
	
	jdr_write_s_array(jdr, "blkst", hist->blkst, jdr_write_ec_recov_blkset);

	{//post_recovery_conclusions{{{
		__auto_type post_recovery_conclusions = &hist->prc;
		jdr_object_scope(jdr, STRINGIFY(post_recovery_conclusions));
		jdr_write(jdr, post_recovery_conclusions, is_unexpected_htr_executed);
	}//}}}

}
