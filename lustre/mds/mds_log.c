/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  lustre/mds/mds_log.c
 *
 *  Copyright (c) 2001-2003 Cluster File Systems, Inc.
 *   Author: Peter Braam <braam@clusterfs.com>
 *   Author: Andreas Dilger <adilger@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#define DEBUG_SUBSYSTEM S_MDS

#include <linux/config.h>
#include <linux/module.h>
#include <linux/version.h>

#include <libcfs/list.h>
#include <linux/obd_class.h>
#include <linux/lustre_fsfilt.h>
#include <linux/lustre_commit_confd.h>
#include <linux/lustre_log.h>

#include "mds_internal.h"

/* callback function of lov to fill unlink log record */
static int mds_log_fill_unlink_rec(struct llog_rec_hdr *rec, void *data)
{
        struct llog_fill_rec_data *lfd = (struct llog_fill_rec_data *)data;
        struct llog_unlink_rec *lur = (struct llog_unlink_rec *)rec;

        lur->lur_oid = lfd->lfd_id;
        lur->lur_ogen = lfd->lfd_ogen;

        RETURN(0);
}

/* callback function of lov to fill setattr log record */
static int mds_log_fill_setattr_rec(struct llog_rec_hdr *rec, void *data)
{
        struct llog_fill_rec_data *lfd = (struct llog_fill_rec_data *)data;
        struct llog_setattr_rec *lsr = (struct llog_setattr_rec *)rec;

        lsr->lsr_oid = lfd->lfd_id;
        lsr->lsr_ogen = lfd->lfd_ogen;

        RETURN(0);
}

static int mds_llog_origin_add(struct llog_ctxt *ctxt,
                        struct llog_rec_hdr *rec, struct lov_stripe_md *lsm,
                        struct llog_cookie *logcookies, int numcookies,
                        llog_fill_rec_cb_t fill_cb)
{
        struct obd_device *obd = ctxt->loc_obd;
        struct obd_device *lov_obd = obd->u.mds.mds_osc_obd;
        struct llog_ctxt *lctxt;
        int rc;
        ENTRY;

        lctxt = llog_get_context(lov_obd, ctxt->loc_idx);
        rc = llog_add(lctxt, rec, lsm, logcookies, numcookies, fill_cb);
        RETURN(rc);
}

static int mds_llog_origin_connect(struct llog_ctxt *ctxt, int count,
                                   struct llog_logid *logid,
                                   struct llog_gen *gen,
                                   struct obd_uuid *uuid)
{
        struct obd_device *obd = ctxt->loc_obd;
        struct obd_device *lov_obd = obd->u.mds.mds_osc_obd;
        struct llog_ctxt *lctxt;
        int rc;
        ENTRY;

        lctxt = llog_get_context(lov_obd, ctxt->loc_idx);
        rc = llog_connect(lctxt, count, logid, gen, uuid);
        RETURN(rc);
}

static int mds_llog_repl_cancel(struct llog_ctxt *ctxt, struct lov_stripe_md *lsm,
                          int count, struct llog_cookie *cookies, int flags)
{
        struct obd_device *obd = ctxt->loc_obd;
        struct obd_device *lov_obd = obd->u.mds.mds_osc_obd;
        struct llog_ctxt *lctxt;
        int rc;
        ENTRY;

        lctxt = llog_get_context(lov_obd, ctxt->loc_idx);
        rc = llog_cancel(lctxt, lsm, count, cookies,flags);
        RETURN(rc);
}

int mds_log_op_unlink(struct obd_device *obd, struct inode *inode,
                      struct lov_mds_md *lmm, int lmm_size,
                      struct llog_cookie *logcookies, int cookies_size)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lov_stripe_md *lsm = NULL;
        struct llog_ctxt *ctxt;
        struct llog_unlink_rec *lur;
        int rc;
        ENTRY;

        if (IS_ERR(mds->mds_osc_obd))
                RETURN(PTR_ERR(mds->mds_osc_obd));

        rc = obd_unpackmd(mds->mds_osc_exp, &lsm,
                          lmm, lmm_size);
        if (rc < 0)
                RETURN(rc);

        /* first prepare unlink log record */
        OBD_ALLOC(lur, sizeof(*lur));
        if (!lur)
                RETURN(-ENOMEM);
        lur->lur_hdr.lrh_len = lur->lur_tail.lrt_len = sizeof(*lur);
        lur->lur_hdr.lrh_type = MDS_UNLINK_REC;

        ctxt = llog_get_context(obd, LLOG_MDS_OST_ORIG_CTXT);
        rc = llog_add(ctxt, &lur->lur_hdr, lsm, logcookies,
                      cookies_size / sizeof(struct llog_cookie),
                      mds_log_fill_unlink_rec);

        obd_free_memmd(mds->mds_osc_exp, &lsm);
        OBD_FREE(lur, sizeof(*lur));

        RETURN(rc);
}

int mds_log_op_setattr(struct obd_device *obd, struct inode *inode,
                      struct lov_mds_md *lmm, int lmm_size,
                      struct llog_cookie *logcookies, int cookies_size)
{
        struct mds_obd *mds = &obd->u.mds;
        struct lov_stripe_md *lsm = NULL;
        struct llog_ctxt *ctxt;
        struct llog_setattr_rec *lsr;
        int rc;
        ENTRY;

        if (IS_ERR(mds->mds_osc_obd))
                RETURN(PTR_ERR(mds->mds_osc_obd));

        rc = obd_unpackmd(mds->mds_osc_exp, &lsm, lmm, lmm_size);
        if (rc < 0)
                RETURN(rc);

        OBD_ALLOC(lsr, sizeof(*lsr));
        if (!lsr)
                GOTO(out, rc = -ENOMEM);

        /* prepare setattr log record */
        lsr->lsr_hdr.lrh_len = lsr->lsr_tail.lrt_len = sizeof(*lsr);
        lsr->lsr_hdr.lrh_type = MDS_SETATTR_REC;
        lsr->lsr_uid = inode->i_uid;
        lsr->lsr_gid = inode->i_gid;

        /* write setattr log */
        ctxt = llog_get_context(obd, LLOG_MDS_OST_ORIG_CTXT);
        rc = llog_add(ctxt, &lsr->lsr_hdr, lsm, logcookies,
                      cookies_size / sizeof(struct llog_cookie),
                      mds_log_fill_setattr_rec);

        OBD_FREE(lsr, sizeof(*lsr));
 out:
        obd_free_memmd(mds->mds_osc_exp, &lsm);
        RETURN(rc);
}

static struct llog_operations mds_ost_orig_logops = {
        lop_add:        mds_llog_origin_add,
        lop_connect:    mds_llog_origin_connect,
};

static struct llog_operations mds_size_repl_logops = {
        lop_cancel:     mds_llog_repl_cancel,
};

int mds_llog_init(struct obd_device *obd, struct obd_device *tgt,
                  int count, struct llog_catid *logid)
{
        struct obd_device *lov_obd = obd->u.mds.mds_osc_obd;
        int rc;
        ENTRY;

        rc = llog_setup(obd, LLOG_MDS_OST_ORIG_CTXT, tgt, 0, NULL,
                        &mds_ost_orig_logops);
        if (rc)
                RETURN(rc);

        rc = llog_setup(obd, LLOG_SIZE_REPL_CTXT, tgt, 0, NULL,
                        &mds_size_repl_logops);
        if (rc)
                RETURN(rc);

        rc = obd_llog_init(lov_obd, tgt, count, logid);
        if (rc)
                CERROR("error lov_llog_init\n");

        RETURN(rc);
}

int mds_llog_finish(struct obd_device *obd, int count)
{
        struct llog_ctxt *ctxt;
        int rc = 0, rc2 = 0;
        ENTRY;

        ctxt = llog_get_context(obd, LLOG_MDS_OST_ORIG_CTXT);
        if (ctxt)
                rc = llog_cleanup(ctxt);

        ctxt = llog_get_context(obd, LLOG_SIZE_REPL_CTXT);
        if (ctxt)
                rc2 = llog_cleanup(ctxt);
        if (!rc)
                rc = rc2;

        RETURN(rc);
}
