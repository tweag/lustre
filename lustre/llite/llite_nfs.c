/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *   NFS export of Lustre Light File System 
 *
 *   Copyright (c) 2002, 2006 Cluster File Systems, Inc.
 *
 *   Author: Yury Umanets <umka@clusterfs.com>
 *           Huang Hua <huanghua@clusterfs.com>
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

#define DEBUG_SUBSYSTEM S_LLITE
#include <lustre_lite.h>
#include "llite_internal.h"

static int ll_nfs_test_inode(struct inode *inode, void *opaque)
{
        struct lu_fid *ifid = &ll_i2info(inode)->lli_fid;
        struct lu_fid *lfid = opaque;

        if (lu_fid_eq(ifid, lfid))
                return 1;

        return 0;
}

static struct inode *search_inode_for_lustre(struct super_block *sb,
                                             struct lu_fid *fid,
                                             struct lustre_capa *capa,
                                             int mode)
{
        struct ll_sb_info     *sbi = ll_s2sbi(sb);
        struct obd_capa       *oc = NULL;
        struct ptlrpc_request *req = NULL;
        struct inode          *inode = NULL;
        unsigned long         valid = 0;
        int                   eadatalen = 0;
        ino_t                 ino = ll_fid_build_ino(sbi, fid);
        int                   rc;
        ENTRY;

        CDEBUG(D_INFO, "searching inode for:(%lu,"DFID")\n", ino, PFID(fid));

        inode = ILOOKUP(sb, ino, ll_nfs_test_inode, fid);
        if (inode)
                RETURN(inode);

        if (S_ISREG(mode)) {
                rc = ll_get_max_mdsize(sbi, &eadatalen);
                if (rc) 
                        RETURN(ERR_PTR(rc)); 
                valid |= OBD_MD_FLEASIZE;
        }

        if (capa) {
                oc = alloc_capa(CAPA_SITE_CLIENT);
                if (!oc)
                        RETURN(ERR_PTR(-ENOMEM));
                oc->c_capa = *capa;
        }

        rc = md_getattr(sbi->ll_md_exp, fid, oc, valid, eadatalen, &req);
        if (oc)
                free_capa(oc);
        if (rc) {
                CERROR("can't get object attrs, fid "DFID", rc %d\n",
                       PFID(fid), rc);
                RETURN(ERR_PTR(rc));
        }

        rc = ll_prep_inode(&inode, req, REPLY_REC_OFF, sb);
        ptlrpc_req_finished(req);
        if (rc)
                RETURN(ERR_PTR(rc));

        RETURN(inode);
}

extern struct dentry_operations ll_d_ops;

static struct dentry *ll_iget_for_nfs(struct super_block *sb,
                                      struct lu_fid *fid,
                                      struct lustre_capa *capa,
                                      umode_t mode)
{
        struct inode  *inode;
        struct dentry *result;
        ENTRY;

        CDEBUG(D_INFO, "Get dentry for fid: "DFID"\n", PFID(fid));
        if (!fid_is_sane(fid))
                RETURN(ERR_PTR(-ESTALE));

        inode = search_inode_for_lustre(sb, fid, capa, mode);
        if (IS_ERR(inode))
                RETURN(ERR_PTR(PTR_ERR(inode)));

        if (is_bad_inode(inode)) {
                /* we didn't find the right inode.. */
                CERROR("can't get inode by fid "DFID"\n",
                       PFID(fid));
                iput(inode);
                RETURN(ERR_PTR(-ESTALE));
        }

        result = d_alloc_anon(inode);
        if (!result) {
                iput(inode);
                RETURN(ERR_PTR(-ENOMEM));
        }
        ll_set_dd(result);
        result->d_op = &ll_d_ops;
        RETURN(result);
}

/*
 * This length is counted as amount of __u32,
 *  It is composed of a fid and a mode 
 */
#define ONE_FH_LEN (sizeof(struct lu_fid)/4 + 1)

static struct dentry *ll_decode_fh(struct super_block *sb, __u32 *fh, int fh_len,
                                   int fh_type,
                                   int (*acceptable)(void *, struct dentry *),
                                   void *context)
{
        struct lu_fid *parent = NULL;
        struct lu_fid *child;
        struct dentry *entry;
        ENTRY;

        CDEBUG(D_INFO, "decoding for "DFID" fh_len=%d fh_type=%d\n", 
                PFID((struct lu_fid*)fh), fh_len, fh_type);

        if (fh_type != 1 && fh_type != 2)
                RETURN(ERR_PTR(-ESTALE));
        if (fh_len < ONE_FH_LEN * fh_type)
                RETURN(ERR_PTR(-ESTALE));

        child = (struct lu_fid*)fh;
        if (fh_type == 2)
                parent = (struct lu_fid*)(fh + ONE_FH_LEN);
                
        entry = sb->s_export_op->find_exported_dentry(sb, child, parent,
                                                      acceptable, context);
        RETURN(entry);
}

/* The return value is file handle type:
 * 1 -- contains child file handle;
 * 2 -- contains child file handle and parent file handle;
 * 255 -- error.
 */
static int ll_encode_fh(struct dentry *de, __u32 *fh, int *plen, int connectable)
{
        struct inode    *inode = de->d_inode;
        struct lu_fid   *fid = ll_inode2fid(inode);
        ENTRY;

        CDEBUG(D_INFO, "encoding for (%lu,"DFID") maxlen=%d minlen=%d\n",
                       inode->i_ino, PFID(fid), *plen, ONE_FH_LEN);

        if (*plen < ONE_FH_LEN)
                RETURN(255);

        memcpy((char*)fh, fid, sizeof(*fid));
        *(fh + ONE_FH_LEN - 1) = (__u32)(S_IFMT & inode->i_mode);

        if (de->d_parent && *plen >= ONE_FH_LEN * 2) {
                struct inode *parent = de->d_parent->d_inode;
                fh += ONE_FH_LEN;
                memcpy((char*)fh, &ll_i2info(parent)->lli_fid, sizeof(*fid));
                *(fh + ONE_FH_LEN - 1) = (__u32)(S_IFMT & parent->i_mode);
                *plen = ONE_FH_LEN * 2;
                RETURN(2);
        } else {
                *plen = ONE_FH_LEN;
                RETURN(1);
        }
}

static struct dentry *ll_get_dentry(struct super_block *sb, void *data)
{
        struct lu_fid      *fid;
        struct dentry      *entry;
        __u32               mode;
        ENTRY;

        fid = (struct lu_fid *)data;
        mode = *((__u32*)data + ONE_FH_LEN - 1);
        
        entry = ll_iget_for_nfs(sb, fid, NULL, mode);
        RETURN(entry);
}

static struct dentry *ll_get_parent(struct dentry *dchild)
{
        struct ptlrpc_request *req = NULL;
        struct inode          *dir = dchild->d_inode;
        struct obd_capa       *oc;
        struct ll_sb_info     *sbi;
        struct dentry         *result = NULL;
        struct mdt_body       *body;
        static char           dotdot[] = "..";
        int                   rc;
        ENTRY;
        
        LASSERT(dir && S_ISDIR(dir->i_mode));
        
        sbi = ll_s2sbi(dir->i_sb);
 
        CDEBUG(D_INFO, "getting parent for (%lu,"DFID")\n", 
                        dir->i_ino, PFID(ll_inode2fid(dir)));

        oc = ll_mdscapa_get(dir);
        rc = md_getattr_name(sbi->ll_md_exp, ll_inode2fid(dir), oc,
                             dotdot, strlen(dotdot) + 1, 0, 0, &req);
        if (rc) {
                capa_put(oc);
                CERROR("failure %d inode %lu get parent\n", rc, dir->i_ino);
                RETURN(ERR_PTR(rc));
        }
        body = lustre_msg_buf(req->rq_repmsg, REPLY_REC_OFF, sizeof(*body)); 
       
        LASSERT(body->valid & OBD_MD_FLID);
        
        CDEBUG(D_INFO, "parent for "DFID" is "DFID"\n", 
                PFID(ll_inode2fid(dir)), PFID(&body->fid1));

        result = ll_iget_for_nfs(dir->i_sb, &body->fid1,
                                 oc ? &oc->c_capa : NULL, S_IFDIR);
        capa_put(oc);

        ptlrpc_req_finished(req);
        RETURN(result);
} 

struct export_operations lustre_export_operations = {
       .get_parent = ll_get_parent,
       .get_dentry = ll_get_dentry,
       .encode_fh  = ll_encode_fh,
       .decode_fh  = ll_decode_fh,
};
