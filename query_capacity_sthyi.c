/* Copyright IBM Corp. 2013, 2019 */

#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include <fcntl.h>
#include <endian.h>
#include <linux/types.h>
#if (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 16) || __GLIBC__ > 2
#include <sys/auxv.h>
#else
#include <signal.h>
#endif

#include "query_capacity_data.h"


/* we are packing the structures in the header file generated by VM */
#pragma pack(push)
#pragma pack(1)
#include "hcpinfbk_qclib.h"
#pragma pack(pop)


#define STHYI_BUF_SIZE		4096
#define STHYI_BUF_ALIGNMENT	4096
#define STHYI_DATA_FILE_ENV_VAR	"QUERY_CAPACITY_STHYI_DATA_FILE"
#define STHYI_FACILITY_BIT	74

#define STHYI_NA	0
#define STHYI_AVAILABLE	1
struct sthyi_priv {
	char   *data;
	int 	avail;
};



#if defined __s390__
#ifndef _SYS_AUXV_H
static void qc_stfle_signal_handler(int signal) {
	qc_debug(NULL, "Signal handler invoked with signal %d\n", signal);

	return;
}
#endif

static int qc_is_sthyi_facility_available(void) {
	unsigned long long stfle_buffer[(STHYI_FACILITY_BIT/64)+1] __attribute__ ((aligned (8))) = { 0,};
#ifndef _SYS_AUXV_H
	sighandler_t old_handler = signal(SIGILL, qc_stfle_signal_handler);
#endif
	{
		register unsigned long reg0 asm("0") = STHYI_FACILITY_BIT/64 ;
		asm volatile (".insn s,0xb2b00000,%0"
			      : "=m" (stfle_buffer), "+d" (reg0) :
			      : "cc", "memory");
	}
#ifndef _SYS_AUXV_H
	signal(SIGILL, old_handler);
#endif

	return (stfle_buffer[STHYI_FACILITY_BIT/64] >> (63 - (STHYI_FACILITY_BIT%64))) & 1;
}
#endif

static int qc_is_sthyi_available_vm(struct qc_handle *hdl) {
#if defined __s390__
#ifdef _SYS_AUXV_H
	unsigned long aux = getauxval(AT_HWCAP);

	qc_debug(hdl, "Using getauxval()\n");
	if (~aux & HWCAP_S390_STFLE)
		qc_debug(hdl, "STFLE not available\n");
#else
	qc_debug(hdl, "Perform raw STFLE retrieval\n");
#endif
	return qc_is_sthyi_facility_available();
#endif

	return 0;
}

static int qc_sthyi_vm(struct sthyi_priv *priv) {
#if defined __s390__
	register unsigned long function_code asm("2") = 0;
	register unsigned long buffer asm("4") = (unsigned long) priv->data;
	register unsigned long return_code asm("5");
	int cc = -1;

	asm volatile (".insn rre,0xb2560000,%2,%3 \n"
		      "ipm %0\n"
		      "srl %0,28\n"
		      : "=d" (cc), "=d" (return_code)
		      : "d" (function_code), "d" (buffer)
		      : "memory", "cc");
	if (cc == 3 && return_code == 4)
		return 1;
	if (cc == 0)
		/* buffer was updated */
		priv->avail = STHYI_AVAILABLE;
#endif

	return 0;
}

static int qc_sthyi_lpar(struct qc_handle *hdl, struct sthyi_priv *priv) {
#if defined __s390__
	uint64_t cc;
	long sthyi_syscall = 380;

#ifdef __NR_s390_sthyi
	sthyi_syscall = __NR_s390_sthyi
#endif
	qc_debug(hdl, "Try STHYI syscall\n");
	if (syscall(sthyi_syscall, 0, priv->data, &cc, 0) || cc) {
		if (errno == ENOSYS) {
			qc_debug(hdl, "STHYI syscall is not available\n");
			return 0;
		}
		qc_debug(hdl, "Error: STHYI syscall execution failed: errno='%s', cc=%" PRIu64 "\n", strerror(errno), cc);
		return -1;
	}
	qc_debug(hdl, "STHYI syscall succeeded\n");
	priv->avail = STHYI_AVAILABLE;
#endif

	return 0;
}

static int qc_parse_sthyi_machine(struct qc_handle *cec, struct inf0mac *machine) {
	qc_debug(cec, "Add CEC values\n");
	qc_debug_indent_inc();
	if (machine->infmval1 & infmproc) {
		qc_debug(cec, "Add processor counts information\n");
		if (qc_set_attr_int(cec, qc_num_cp_dedicated, htobe16(machine->infmdcps), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_cp_shared, htobe16(machine->infmscps), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_cp_total, htobe16(machine->infmdcps) + htobe16(machine->infmscps), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_ifl_dedicated, htobe16(machine->infmdifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_ifl_shared, htobe16(machine->infmsifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_ifl_total, htobe16(machine->infmdifl) + htobe16(machine->infmsifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_core_dedicated, htobe16(machine->infmdcps) + htobe16(machine->infmdifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(cec, qc_num_core_shared, htobe16(machine->infmscps) + htobe16(machine->infmsifl), ATTR_SRC_STHYI))
			return -1;

		if (machine->infmval1 & infmziipv) {
			qc_debug(cec, "Add zIIP counts information\n");
			if (qc_set_attr_int(cec, qc_num_ziip_total, htobe16(machine->infmdziip) + htobe16(machine->infmsziip), ATTR_SRC_STHYI) ||
			    qc_set_attr_int(cec, qc_num_ziip_dedicated, htobe16(machine->infmdziip), ATTR_SRC_STHYI) ||
			    qc_set_attr_int(cec, qc_num_ziip_shared, htobe16(machine->infmsziip), ATTR_SRC_STHYI))
				return -1;
		}
	}

	if (machine->infmval1 & infmmid) {
		qc_debug(cec, "Add machine ID information\n");
		if (qc_set_attr_ebcdic_string(cec, qc_type, machine->infmtype, sizeof(machine->infmtype), ATTR_SRC_STHYI) ||
		    qc_set_attr_ebcdic_string(cec, qc_manufacturer, machine->infmmanu, sizeof(machine->infmmanu), ATTR_SRC_STHYI) ||
		    qc_set_attr_ebcdic_string(cec, qc_sequence_code, machine->infmseq, sizeof(machine->infmseq), ATTR_SRC_STHYI) ||
		    qc_set_attr_ebcdic_string(cec, qc_plant, machine->infmpman, sizeof(machine->infmpman), ATTR_SRC_STHYI))
			return -2;
	}
	if (machine->infmval1 & infmmnam) {
		qc_debug(cec, "Add machine name information\n");
		if (qc_set_attr_ebcdic_string(cec, qc_layer_name, machine->infmname, sizeof(machine->infmname), ATTR_SRC_STHYI))
			return -3;
	}
	qc_debug_indent_dec();

	return 0;
}

static int qc_parse_sthyi_partition(struct qc_handle *lpar, struct inf0par *partition) {
	struct qc_handle *group;
	int rc = -1;

	qc_debug(lpar, "Add LPAR values\n");
	qc_debug_indent_inc();
	if (partition->infpval1 & infpproc) {
		qc_debug(lpar, "Add processor counts information\n");
		if (qc_set_attr_int(lpar, qc_num_cp_total, htobe16(partition->infpscps) + htobe16(partition->infpdcps), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_num_cp_shared, htobe16(partition->infpscps), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_num_cp_dedicated, htobe16(partition->infpdcps), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_num_ifl_total, htobe16(partition->infpsifl) + htobe16(partition->infpdifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_num_ifl_shared, htobe16(partition->infpsifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_num_ifl_dedicated, htobe16(partition->infpdifl), ATTR_SRC_STHYI))
			goto out_err;

		if (partition->infpval1 & infpziipv) {
			qc_debug(lpar, "Add zIIP counts information\n");
			if (qc_set_attr_int(lpar, qc_num_ziip_total, htobe16(partition->infpsziip) + htobe16(partition->infpdziip), ATTR_SRC_STHYI) ||
			    qc_set_attr_int(lpar, qc_num_ziip_shared, htobe16(partition->infpsziip), ATTR_SRC_STHYI) ||
			    qc_set_attr_int(lpar, qc_num_ziip_dedicated, htobe16(partition->infpdziip), ATTR_SRC_STHYI))
				goto out_err;
		}
	}

	if (partition->infpval1 & infpwbcc) {
		qc_debug(lpar, "Add weight-based capped capacity information\n");
		if (qc_set_attr_int(lpar, qc_cp_weight_capping, htobe32(partition->infpwbcp), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_ifl_weight_capping, htobe32(partition->infpwbif), ATTR_SRC_STHYI))
			goto out_err;

		if (partition->infpval1 & infpziipv) {
			qc_debug(lpar, "Add ziip weight-based capped capacity information\n");
			if (qc_set_attr_int(lpar, qc_ziip_weight_capping, htobe32(partition->infpwbziip), ATTR_SRC_STHYI))
				goto out_err;
		}
	}

	if (partition->infpval1 & infpacc) {
		qc_debug(lpar, "Add absolute capped capacity information\n");
		if (qc_set_attr_int(lpar, qc_cp_absolute_capping, htobe32(partition->infpabcp), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(lpar, qc_ifl_absolute_capping, htobe32(partition->infpabif), ATTR_SRC_STHYI))
			goto out_err;

		if (partition->infpval1 & infpziipv) {
			qc_debug(lpar, "Add ziip absolute capped capacity information\n");
			if (qc_set_attr_int(lpar, qc_ziip_absolute_capping, htobe32(partition->infpabziip), ATTR_SRC_STHYI))
				goto out_err;
		}
	}

	if (partition->infpval1 & infppid) {
		qc_debug(lpar, "Add partition ID information\n");
		if (qc_set_attr_int(lpar, qc_partition_number, htobe16(partition->infppnum), ATTR_SRC_STHYI) ||
		    qc_set_attr_ebcdic_string(lpar, qc_layer_name, partition->infppnam, sizeof(partition->infppnam), ATTR_SRC_STHYI))
			goto out_err;
	}

	if (partition->infpval1 & infplgvl && (rc = qc_is_nonempty_ebcdic((__u64*)partition->infplgnm)) > 0) {
		/* LPAR group is only defined in case group name is not empty */
		qc_debug(lpar, "Insert LPAR group layer\n");
		if (qc_insert_handle(lpar, &group, QC_LAYER_TYPE_LPAR_GROUP)) {
			qc_debug(lpar, "Error: Failed to insert LPAR group layer\n");
			goto out_err;
		}
		rc = qc_set_attr_ebcdic_string(group, qc_layer_name, partition->infplgnm, sizeof(partition->infplgnm), ATTR_SRC_STHYI);
		if (htobe32(partition->infplgcp))
			rc |= qc_set_attr_int(group, qc_cp_absolute_capping, htobe32(partition->infplgcp), ATTR_SRC_STHYI);
		if (htobe32(partition->infplgif))
			rc |= qc_set_attr_int(group, qc_ifl_absolute_capping, htobe32(partition->infplgif), ATTR_SRC_STHYI);
		if (partition->infpval1 & infpziipv && htobe32(partition->infplgziip))
			rc |= qc_set_attr_int(group, qc_ziip_absolute_capping, htobe32(partition->infplgziip), ATTR_SRC_STHYI);
		if (rc)
			goto out_err;
	}
	rc = 0;

out_err:
	qc_debug_indent_dec();

	return rc;
}

static int qc_parse_sthyi_hypervisor(struct qc_handle *hdl, struct inf0hyp *hv) {
	int *ltype = qc_get_attr_value_int(hdl, qc_layer_type_num);
	int rc = -1;

	qc_debug(hdl, "Add HV values\n");
	qc_debug_indent_inc();
	if (hv->infytype == infytvm && *ltype != QC_LAYER_TYPE_ZVM_HYPERVISOR) {
		qc_debug(hdl, "Error: Layer %d type mismatch. Expected %d (z/VM), got %d\n", hdl->layer_no, QC_LAYER_TYPE_ZVM_HYPERVISOR, *ltype);
		goto err;
	}
	if (hv->infytype == infytzcx && *ltype != QC_LAYER_TYPE_ZOS_HYPERVISOR) {
		qc_debug(hdl, "Error: Layer %d type mismatch. Expected %d (zCX), got %d\n", hdl->layer_no, QC_LAYER_TYPE_ZOS_HYPERVISOR, *ltype);
		goto err;
	}
	if (hv->infytype == infytkvm && *ltype != QC_LAYER_TYPE_KVM_HYPERVISOR) {
		qc_debug(hdl, "Error: Layer %d type mismatch. Expected %d (KVM), got %d\n", hdl->layer_no, QC_LAYER_TYPE_KVM_HYPERVISOR, *ltype);
		goto err;
	}

	if (*ltype == QC_LAYER_TYPE_ZVM_HYPERVISOR) {
		// z/VM-specific attributes
		if (qc_set_attr_int(hdl, qc_limithard_consumption, (hv->infyflg1 & infylmcn) ? 1 : 0, ATTR_SRC_STHYI) ||
		    qc_set_attr_int(hdl, qc_prorated_core_time, (hv->infyflg1 & infylmpr) ? 1 : 0, ATTR_SRC_STHYI))
			goto err;
	}

	if (qc_set_attr_ebcdic_string(hdl, qc_layer_name, hv->infysyid, sizeof(hv->infysyid), ATTR_SRC_STHYI) ||
	    qc_set_attr_ebcdic_string(hdl, qc_cluster_name, hv->infyclnm, sizeof(hv->infyclnm), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(hdl, qc_num_core_total, htobe16(hv->infyscps) + htobe16(hv->infydcps) + htobe16(hv->infysifl) + htobe16(hv->infydifl), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(hdl, qc_num_core_shared, htobe16(hv->infyscps) + htobe16(hv->infysifl), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(hdl, qc_num_core_dedicated, htobe16(hv->infydcps) + htobe16(hv->infydifl), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(hdl, qc_num_cp_total, htobe16(hv->infyscps) + htobe16(hv->infydcps), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(hdl, qc_num_cp_shared, htobe16(hv->infyscps), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(hdl, qc_num_cp_dedicated, htobe16(hv->infydcps), ATTR_SRC_STHYI))
		goto err;

	if (hv->infyval1 & infyziipv) {
		// Note: Dedicating zIIPs is not possible in any z/VM release or elsewhere. Hence info is not defined in STHYI.
		//       For consistency, we provide a respective attribute, still, but hardcode the value to 0.
		if (qc_set_attr_int(hdl, qc_num_ziip_total, htobe16(hv->infysziip), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(hdl, qc_num_ziip_shared, htobe16(hv->infysziip), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(hdl, qc_num_ziip_dedicated, 0, ATTR_SRC_STHYI))
			goto err;
	}

	if (*ltype != QC_LAYER_TYPE_ZOS_HYPERVISOR) {
		// zOS doesn't have any IFLs
		if (qc_set_attr_int(hdl, qc_num_ifl_total, htobe16(hv->infysifl) + htobe16(hv->infydifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(hdl, qc_num_ifl_shared, htobe16(hv->infysifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(hdl, qc_num_ifl_dedicated, htobe16(hv->infydifl), ATTR_SRC_STHYI))
			goto err;
		if (hv->infyflg1 & infymten && qc_set_attr_int(hdl, qc_num_ifl_threads, hv->infyiflt, ATTR_SRC_STHYI))
			goto err;
	}

	if (hv->infyflg1 & infymten) {
		qc_debug(hdl, "Add MT info\n");
		if (qc_set_attr_int(hdl, qc_num_cp_threads, hv->infycpt, ATTR_SRC_STHYI))
			goto err;
		if (hv->infyval1 & infyziipv && qc_set_attr_int(hdl, qc_num_ziip_threads, hv->infyziipt, ATTR_SRC_STHYI))
			goto err;
	}
	rc = 0;
err:
	qc_debug_indent_dec();

	return rc;
}

static int qc_parse_sthyi_guest(struct qc_handle *gst, struct inf0gst *guest) {
	int *ltype = qc_get_attr_value_int(gst, qc_layer_type_num);
	struct qc_handle *pool_hdl;
	int rc=-1, total;

	qc_debug(gst, "Add Guest values\n");
	qc_debug_indent_inc();
	// z/VM-specific attribute
	if (*ltype == QC_LAYER_TYPE_ZVM_GUEST &&
	    qc_set_attr_int(gst, qc_mobility_enabled, (guest->infgflg1 & infgmob) ? 1 : 0, ATTR_SRC_STHYI))
		goto err;

	if (qc_set_attr_int(gst, qc_has_multiple_cpu_types, (guest->infgflg1 & infgmcpt) ? 1 : 0, ATTR_SRC_STHYI) ||
	    qc_set_attr_int(gst, qc_cp_dispatch_limithard, (guest->infgflg1 & infgcplh) ? 1 : 0, ATTR_SRC_STHYI) ||
	    qc_set_attr_ebcdic_string(gst, qc_layer_name, guest->infgusid, sizeof(guest->infgusid), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(gst, qc_num_cp_shared, htobe16(guest->infgscps), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(gst, qc_num_cp_dedicated, htobe16(guest->infgdcps), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(gst, qc_cp_capped_capacity, htobe32(guest->infgcpcc), ATTR_SRC_STHYI))
		goto err;

	if (guest->infgval1 & infgziipv) {
		// Note: See infysziip on why we handle qc_num_ziip_dedicated this way
		if (qc_set_attr_int(gst, qc_num_ziip_total, htobe16(guest->infgsziip), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(gst, qc_num_ziip_shared, htobe16(guest->infgsziip), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(gst, qc_num_ziip_dedicated, 0, ATTR_SRC_STHYI))
			goto err;
	}

	if ((guest->infgscps > 0 || guest->infgdcps > 0) &&
	    qc_set_attr_int(gst, qc_cp_dispatch_type, guest->infgcpdt, ATTR_SRC_STHYI))
		goto err;

	// zOS doesn't have any IFLs
	if (*ltype != QC_LAYER_TYPE_ZOS_ZCX_SERVER) {
		if (qc_set_attr_int(gst, qc_ifl_dispatch_limithard, (guest->infgflg1 & infgiflh) ? 1 : 0, ATTR_SRC_STHYI) ||
		    qc_set_attr_int(gst, qc_num_ifl_shared, htobe16(guest->infgsifl), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(gst, qc_num_ifl_dedicated, htobe16(guest->infgdifl), ATTR_SRC_STHYI))
			goto err;
		if ((guest->infgsifl > 0 || guest->infgdifl > 0) &&
		    qc_set_attr_int(gst, qc_ifl_dispatch_type, guest->infgifdt, ATTR_SRC_STHYI))
			goto err;
	} else if (guest->infgval1 & infgziipv) {   // *ltype == QC_LAYER_TYPE_ZOS_ZCX_SERVER
		if (qc_set_attr_int(gst, qc_cp_capped_capacity, htobe32(guest->infgziipcc), ATTR_SRC_STHYI))
			goto err;
		if ((guest->infgsziip > 0 /*|| guest->infgdziip > 0*/) &&
		    qc_set_attr_int(gst, qc_ziip_dispatch_type, guest->infgziipdt, ATTR_SRC_STHYI))
			goto err;
		if (qc_set_attr_int(gst, qc_ziip_dispatch_limithard, (guest->infgflg1 & infgziiph) ? 1 : 0, ATTR_SRC_STHYI))
			goto err;
	}

	if (qc_set_attr_int(gst, qc_num_cp_total, htobe16(guest->infgscps) + htobe16(guest->infgdcps), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(gst, qc_num_cpu_dedicated, htobe16(guest->infgdcps) + htobe16(guest->infgdifl), ATTR_SRC_STHYI) ||
	    qc_set_attr_int(gst, qc_num_cpu_shared, htobe16(guest->infgscps) + htobe16(guest->infgsifl), ATTR_SRC_STHYI))
		goto err;

	// zOS doesn't have any IFLs
	if (*ltype != QC_LAYER_TYPE_ZOS_ZCX_SERVER) {
		if (qc_set_attr_int(gst, qc_ifl_capped_capacity, htobe32(guest->infgifcc), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(gst, qc_num_ifl_total, htobe16(guest->infgsifl) + htobe16(guest->infgdifl), ATTR_SRC_STHYI))
			goto err;
	}

	total = htobe16(guest->infgscps) + htobe16(guest->infgdcps) + htobe16(guest->infgsifl) + htobe16(guest->infgdifl);
	if (guest->infgval1 & infgziipv)
		total += htobe16(guest->infgsziip) /*+ htobe16(guest->infgdziip)*/;
	if (qc_set_attr_int(gst, qc_num_cpu_total, total, ATTR_SRC_STHYI))
		goto err;

	/* if pool name is empty then we're done */
	if (qc_is_nonempty_ebcdic((__u64*)guest->infgpnam)) {
		int ptype;
		char *str;
		qc_debug(gst, "Add Pool values\n");
		if (*ltype == QC_LAYER_TYPE_ZVM_GUEST) {
			ptype = QC_LAYER_TYPE_ZVM_RESOURCE_POOL;
			str = "z/VM resource pool";
		} else if (*ltype == QC_LAYER_TYPE_ZOS_ZCX_SERVER) {
			ptype = QC_LAYER_TYPE_ZOS_TENANT_RESOURCE_GROUP;
			str = "z/OS tenant resource pool";
		} else {
			qc_debug(gst, "Error: Unhandled type for pool layer\n");
			goto err;
		}
		qc_debug(gst, "Layer %2d: %s\n", gst->layer_no, str);
		if (qc_insert_handle(gst, &pool_hdl, ptype)) {
			qc_debug(gst, "Error: Failed to insert pool layer\n");
			goto err;
		}
		if (qc_set_attr_ebcdic_string(pool_hdl, qc_layer_name, guest->infgpnam, sizeof(guest->infgpnam), ATTR_SRC_STHYI) ||
		    qc_set_attr_int(pool_hdl, qc_cp_limithard_cap, (guest->infgpflg & infgpclh) ? 1 : 0, ATTR_SRC_STHYI) ||
		    qc_set_attr_int(pool_hdl, qc_cp_capacity_cap, (guest->infgpflg & infgpcpc) ? 1 : 0, ATTR_SRC_STHYI) ||
		    qc_set_attr_int(pool_hdl, qc_cp_capped_capacity, htobe32(guest->infgpccc), ATTR_SRC_STHYI))
			goto err;

		// zOS doesn't have any IFLs
		if (*ltype != QC_LAYER_TYPE_ZOS_ZCX_SERVER) {
			if (qc_set_attr_int(pool_hdl, qc_ifl_limithard_cap, (guest->infgpflg & infgpilh) ? 1 : 0, ATTR_SRC_STHYI) ||
			    qc_set_attr_int(pool_hdl, qc_ifl_capacity_cap, (guest->infgpflg & infgpifc) ? 1 : 0, ATTR_SRC_STHYI) ||
			    qc_set_attr_int(pool_hdl, qc_ifl_capped_capacity, htobe32(guest->infgpicc), ATTR_SRC_STHYI))
				goto err;
		}
		if (guest->infgval1 & infgziipv) {
			if (qc_set_attr_int(pool_hdl, qc_ziip_limithard_cap, (guest->infgpflg & infgpzlh) ? 1 : 0, ATTR_SRC_STHYI) ||
			    qc_set_attr_int(pool_hdl, qc_ziip_capacity_cap, (guest->infgpflg & infgpzpc) ? 1 : 0, ATTR_SRC_STHYI) ||
			    qc_set_attr_int(pool_hdl, qc_ziip_capped_capacity, htobe32(guest->infgpzcc), ATTR_SRC_STHYI))
				goto err;
		}
	}
	rc = 0;
err:
	qc_debug_indent_dec();

	return rc;
}

/* Returns pointer to the n-th hypervisor handle. num starts at 0, and handles
   are returned in sequence from handle linked list */
static struct qc_handle *qc_get_HV_layer(struct qc_handle *hdl, int num) {
	struct qc_handle *h = hdl;
	int i, type;

	for (hdl = hdl->root, i = 0, num++; hdl != NULL; hdl = hdl->next) {
		type = *(int *)(hdl->layer);
		if ((type == QC_LAYER_TYPE_ZVM_HYPERVISOR || type == QC_LAYER_TYPE_KVM_HYPERVISOR
		     || type == QC_LAYER_TYPE_ZOS_HYPERVISOR) && ++i == num)
			return hdl;
	}
	qc_debug(h, "Error: Couldn't find HV layer %d, only %d layer(s) found\n", num, i);

	return NULL;
}

static int qc_sthyi_process(struct qc_handle *hdl, char *buf) {
	struct sthyi_priv *priv = (struct sthyi_priv *)buf;
	int no_hyp_gst, i, rc = 0;
	struct inf0gst *guest[inf0ygmx];
	struct inf0hyp *hv[inf0ygmx];
	struct inf0par *partition;
	struct inf0mac *machine;
	struct inf0hdr *header;
	char *sthyi_buffer;

	qc_debug(hdl, "Process STHYI\n");
	qc_debug_indent_inc();
	if (!priv) {
		qc_debug(hdl, "No priv, exiting\n");
		goto out;
	}
	if (priv->avail == STHYI_NA) {
		qc_debug(hdl, "No priv data, exiting\n");
		goto out;
	}
	sthyi_buffer = priv->data;
	if (!sthyi_buffer) {
		qc_debug(hdl, "No data, exiting\n");
		goto out;	// No data: nothing we can do about
	}
	header = (struct inf0hdr *) sthyi_buffer;
	machine = (struct inf0mac *) (sthyi_buffer + htobe16(header->infmoff));
	partition = (struct inf0par *) (sthyi_buffer + htobe16(header->infpoff));
	no_hyp_gst = (int)header->infhygct;
	if (no_hyp_gst > inf0ygmx) {
		qc_debug(hdl, "Error: STHYI reports %d layers, exceeding the supported "
			"maximum of %d\n", no_hyp_gst, inf0ygmx);
		rc = -1;
		goto out;
	}
	if (no_hyp_gst > 0) {
		hv[0] = (struct inf0hyp *)(sthyi_buffer + htobe16(header->infhoff1));
		guest[0] = (struct inf0gst *)(sthyi_buffer + htobe16(header->infgoff1));
	}
	if (no_hyp_gst > 1) {
		hv[1] = (struct inf0hyp *)(sthyi_buffer + htobe16(header->infhoff2));
		guest[1] = (struct inf0gst *)(sthyi_buffer + htobe16(header->infgoff2));
	}
	if (no_hyp_gst > 2) {
		hv[2] = (struct inf0hyp *)(sthyi_buffer + htobe16(header->infhoff3));
		guest[2] = (struct inf0gst *)(sthyi_buffer + htobe16(header->infgoff3));
	}

	if (qc_parse_sthyi_machine(hdl, machine)) {
		rc = -3;
		goto out;
	}

	hdl = qc_get_lpar_handle(hdl);
	if (!hdl) {
		qc_debug(hdl, "Error: No LPAR handle found\n");
		rc = -4;
		goto out;
	}
	if (qc_parse_sthyi_partition(hdl, partition)) {
		rc = -5;
		goto out;
	}

	for (i = 0; i < no_hyp_gst; i++) {
		if ((hdl = qc_get_HV_layer(hdl, i)) == NULL) {
			rc = -7;
			goto out;
		}
		if (qc_parse_sthyi_hypervisor(hdl, hv[i]) || qc_parse_sthyi_guest(hdl->next, guest[i])) {
			rc = -9;
			goto out;
		}
	}
out:
	qc_debug_indent_dec();

	return rc;
}

static void qc_sthyi_dump(struct qc_handle *hdl, char *buf) {
	struct sthyi_priv *priv = (struct sthyi_priv *)buf;
	int fd, rc, success = 0;
	char *fname = NULL;

	qc_debug(hdl, "Dump STHYI\n");
	qc_debug_indent_inc();
	if (!priv || priv->avail != STHYI_AVAILABLE) {
		qc_debug(hdl, "No data available\n");
		success = 1;
		goto out;
	}
	if (!priv->data) {
		qc_debug(hdl, "Error: Cannot dump sthyi, since priv->buf == NULL\n");
		goto out;
	}
	if (asprintf(&fname, "%s/sthyi", qc_dbg_dump_dir) == -1) {
		qc_debug(hdl, "Error: Mem alloc error, cannot write dump\n");
		goto out;
	}
	fd = open(fname, O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
	if (fd == -1) {
		qc_debug(hdl, "Error: Failed to open file '%s' to write dump\n", fname);
		goto out;
	}
	rc = write(fd, priv->data, STHYI_BUF_SIZE);
	close(fd);
	if (rc == -1) {
		qc_debug(hdl, "Error: Failed to write STHYI data into dump: %s\n", strerror(errno));
	} else {
		qc_debug(hdl, "STHYI data dumped to '%s'\n", fname);
		success = 1;
	}

out:
	free(fname);
	if (!success)
		qc_mark_dump_incomplete(hdl, "sthyi");
	qc_debug_indent_dec();
}

static int qc_read_sthyi_dump(struct qc_handle *hdl, char *buf) {
	char *fname = NULL;
	int fd , rc = -1;
	ssize_t lrc;

	if (asprintf(&fname, "%s/sthyi", qc_dbg_use_dump) == -1) {
		qc_debug(hdl, "Error: Mem alloc error, cannot read dump\n");
		goto out;
	}
	if (access(fname, F_OK)) {
		qc_debug(hdl, "No STHYI dump available\n");
		rc = 1;
		goto out;
	}
	if ((fd = open(fname, O_RDONLY)) == -1) {
		qc_debug(hdl, "Error: Failed to open file '%s' to read dump\n", fname);
		goto out;
	}
	lrc = read(fd, buf, STHYI_BUF_SIZE);
	close(fd);
	if (lrc == -1) {
		qc_debug(hdl, "Error: Failed to read STHYI data dump: %s\n", strerror(errno));
	} else {
		qc_debug(hdl, "STHYI data read from '%s'\n", fname);
		rc = 0;
	}

out:
	free(fname);

	return rc;
}

static int qc_sthyi_open(struct qc_handle *hdl, char **buf) {
	struct sthyi_priv *priv = NULL;
	void *p = NULL;
	int rc = 0;

	*buf = NULL;
	qc_debug(hdl, "Retrieve STHYI information\n");
	qc_debug_indent_inc();
	if ((priv = malloc(sizeof(struct sthyi_priv))) == NULL) {
		qc_debug(hdl, "Error: failed to alloc \n");
		rc = -1;
		goto out;
	}
	bzero(priv, sizeof(struct sthyi_priv));
	*buf = (char *)priv;
	if (posix_memalign(&p, STHYI_BUF_ALIGNMENT, STHYI_BUF_SIZE)) {
		qc_debug(hdl, "Error: posix_memalign() failed\n");
		rc = -2;
		goto out;
	}
	priv->data = (char *)p;
	bzero(priv->data, STHYI_BUF_SIZE);

	if (qc_dbg_use_dump) {
		if (qc_read_sthyi_dump(hdl, priv->data) != 0)
			goto out;
		priv->avail = STHYI_AVAILABLE;
	} else {
		/* There is no way for us to check programmatically whether
		   we're in an LPAR or in a VM, so we simply try out both */
		if (qc_is_sthyi_available_vm(hdl)) {
			qc_debug(hdl, "Executing STHYI instruction\n");
			/* we assume we are not relocated at this spot, between STFLE and STHYI */
			if (qc_sthyi_vm(priv)) {
				qc_debug(hdl, "Error: STHYI instruction execution failed\n");
				rc = -3;
				goto out;
			}
		} else {
			qc_debug(hdl, "STHYI instruction is not available\n");
			rc = qc_sthyi_lpar(hdl, priv);
		}
	}

out:
	qc_debug_indent_dec();

	return rc;
}

static void qc_sthyi_close(struct qc_handle *hdl, char *priv) {
	if (priv) {
		free(((struct sthyi_priv *)priv)->data);
		free(priv);
	}
}

struct qc_data_src sthyi = {qc_sthyi_open,
			    qc_sthyi_process,
			    qc_sthyi_dump,
			    qc_sthyi_close,
			    NULL,
			    NULL};
